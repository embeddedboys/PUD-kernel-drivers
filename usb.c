// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2025 embeddedboys, Ltd.
 *
 * Author: Zheng Hua <hua.zheng@embeddedboys.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fb.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/timer.h>

#include "pud.h"

// A jpeg image of a panda, binary data
#include "panda.h"

#include "encoder.h"
#include "rgb565.h"

#define DRV_NAME "pud"

static int pud_transfer(struct pud *pud, u8 cmd, u8 request,
                        u8 addr, u8 *data, size_t len)
{
    struct usb_device *udev = pud->udev;
    int rc, actual_length;
    int pipe;

    pud->ctrl_buf[0] = cmd & 0xff;
    pud->ctrl_buf[1] = (cmd >> 8) & 0xff;
    pud->ctrl_buf[2] = len & 0xff;
    pud->ctrl_buf[3] = (len >> 8) & 0xff;

    rc = usb_control_msg(
        udev,
        usb_sndctrlpipe(udev, EP0_OUT_ADDR),
        request,
        TYPE_VENDOR | USB_DIR_OUT,
        0, 0,
        pud->ctrl_buf,
        sizeof(pud->ctrl_buf),
        PUD_DEFAULT_TIMEOUT
    );

    pipe = (addr & USB_DIR_OUT) ? usb_sndbulkpipe(udev, addr) : \
                                  usb_rcvbulkpipe(udev, addr);

    rc = usb_bulk_msg(
        udev,
        pipe,
        (void *)data,
        len,
        &actual_length,
        PUD_DEFAULT_TIMEOUT
    );
    if (rc)
        return rc;

    return actual_length;
}

struct pud_usb_bulk_context {
    struct timer_list timer;
    struct usb_sg_request sgr;
};

static void pud_usb_bulk_timeout(struct timer_list *t)
{
    struct pud_usb_bulk_context *ctx = from_timer(ctx, t, timer);

    usb_sg_cancel(&ctx->sgr);
}

ssize_t pud_flush(struct pud *pud, u16 x, u16 y, u16 xe, u16 ye,
                  const u8 jpeg_data[], size_t data_size)
{
    struct usb_device *udev = pud->udev;
    struct pud_usb_bulk_context ctx;
    struct pud_ep1_header *hdr = (struct pud_ep1_header *)pud->encoder_buf;
    int rc;

    /* data_size must be even for RP2350 */
    if (data_size % 2)
        data_size += 1;

    if (data_size > pud->encoder_buf_size - PUD_EP1_HEADER_SIZE)
        data_size = pud->encoder_buf_size - PUD_EP1_HEADER_SIZE;

    /* The header lives in the DMA-coherent buffer the SG table already covers,
     * right in front of the payload, so one bulk transfer carries both and the
     * rectangle no longer needs a control request. */
    hdr->xs = x;
    hdr->ys = y;
    hdr->xe = xe;
    hdr->ye = ye;
    hdr->size = data_size;

    /* Move the frame into the DMA buffer before the SG bulk send */
    if (jpeg_data != pud->encoder_buf + PUD_EP1_HEADER_SIZE)
        memcpy(pud->encoder_buf + PUD_EP1_HEADER_SIZE, jpeg_data, data_size);

    rc = usb_sg_init(&ctx.sgr, pud->udev, usb_sndbulkpipe(udev, EP1_OUT_ADDR), 0,
                    pud->bulk_sgt.sgl, pud->bulk_sgt.nents,
                    PUD_EP1_HEADER_SIZE + data_size, GFP_KERNEL);

    /* timeout routine in case the device stops reading */
    timer_setup_on_stack(&ctx.timer, pud_usb_bulk_timeout, 0);
    mod_timer(&ctx.timer, jiffies + msecs_to_jiffies(3000));

    usb_sg_wait(&ctx.sgr);

    if (!timer_delete_sync(&ctx.timer))
        rc = -ETIMEDOUT;
    else if (ctx.sgr.status < 0)
        rc = ctx.sgr.status;
    else if (ctx.sgr.bytes != PUD_EP1_HEADER_SIZE + data_size)
        rc = -EIO;
    else
        rc = data_size;     /* the callers count payload bytes */

    destroy_timer_on_stack(&ctx.timer);

    return rc;
}

static int pud_read_unique_id(struct usb_interface *intf, u8 serial[], size_t len)
{
    struct pud *pud = usb_get_intfdata(intf);
    int ret;

    if (len > 8) {
        pr_info("serial length should less than 8!\n");
        return -EINVAL;
    }

    // /* Dummy read, device need to prepares data */
    // ret = pud_transfer(pud, 0x01, REQ_EP2_IN, EP2_IN_ADDR, serial, len);

    ret = pud_transfer(pud, PUD_CMD_GET_SN, REQ_EP2_IN, EP2_IN_ADDR, serial,
                       len);

    return ret;
}

/* One PUD_CMD_GET_CAPS round trip.  Returns how many bytes the device answered
 * with -- it clamps its reply to what it knows, so a firmware from before the
 * panel parameters sends the first PUD_CAPS_V1_SIZE -- or a negative errno.
 *
 * The request/reply buffer has to be DMA-mappable and struct pud does not exist
 * yet (the DRM mode and the input axis ranges are built from this answer), so
 * the transfer borrows a throwaway heap object rather than a stack buffer. */
int pud_query_caps(struct usb_device *udev, struct pud_caps *caps)
{
    struct pud *tmp;
    int n;

    tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;

    tmp->udev = udev;
    n = pud_transfer(tmp, PUD_CMD_GET_CAPS, REQ_EP2_IN, EP2_IN_ADDR,
                     (u8 *)caps, sizeof(*caps));
    kfree(tmp);

    if (n < 0)
        return n;
    if (n < PUD_CAPS_V1_SIZE || caps->magic != PUD_CAPS_MAGIC)
        return -ENODATA;

    return min(n, (int)sizeof(*caps));
}

/* The panel parameters a device reports, into a display struct.  bpp was left
 * unset in the first firmware that reported parameters; this driver and the EP1
 * stream work in 16 bpp either way. */
void pud_caps_to_display(struct pud_display *disp, const struct pud_caps *caps)
{
    disp->xres = caps->xres;
    disp->yres = caps->yres;
    disp->rotate = caps->rotation;
    disp->bpp = caps->bpp ? caps->bpp : 16;
    disp->pixelclock_khz = caps->pixelclock_khz;
    if (caps->width_mm && caps->height_mm) {
        disp->width_mm = caps->width_mm;
        disp->height_mm = caps->height_mm;
    }
}

/* Fold a capability report into a device.  Anything the device does not report
 * keeps its host-side default: an RP2040 firmware accepts half-size transfers,
 * and only newer firmware knows the panel parameters at all. */
void pud_apply_caps(struct pud *pud, const struct pud_caps *caps, int caps_len)
{
    unsigned int frame_max;
    bool have_params = caps_len >= (int)sizeof(*caps);

    /* Defaults first: the firmware ships QOI, an unset decoder_type (0) would
     * otherwise read as tjpgd, and a device that stays silent is assumed to
     * have a panel until it says otherwise. */
    pud->frame_max = USB_TRANS_MAX_SIZE;
    pud->max_band_pixels = PUD_DEFAULT_BAND_PIXELS;
    pud->decoder_type = PUD_DECODER_QOI;
    pud->has_touch = true;

    if (caps_len < PUD_CAPS_V1_SIZE) {
        dev_warn(pud->dev,
                 "no capability report (%d bytes); keeping %u pixels per band\n",
                 caps_len, pud->max_band_pixels);
        return;
    }

    frame_max = min_t(u32, caps->frame_max, USB_TRANS_MAX_SIZE);
    if (frame_max <= 16) {
        dev_warn(pud->dev, "device reports an unusable frame_max (%u)\n",
                 caps->frame_max);
    } else {
        pud->frame_max = frame_max;
        /* the header rides in front of every payload, so it comes off the top */
        pud->max_band_pixels = (frame_max - PUD_EP1_HEADER_SIZE - 16) / 3;
    }

    pud->decoder_type = caps->decoder_type;

    /* Touch is optional: the firmware says whether it has a controller behind
     * EP4 (most board configs do not). */
    if (have_params)
        pud->has_touch = !!(caps->flags & PUD_CAPS_TOUCH);

    if (have_params) {
        pud_caps_to_display(&pud->display_data, caps);
        pud->touch_polling_period = caps->tp_polling_period;
    }

    dev_info(pud->dev,
             "caps: proto %u, frame_max %u, decoder %u -> %u pixels per band%s\n",
             caps->proto_ver, caps->frame_max, caps->decoder_type,
             pud->max_band_pixels,
             have_params ? "" : " (old firmware: no panel parameters)");

    if (have_params)
        dev_info(pud->dev,
                 "panel: %ux%u, rotation %u, %u bpp, %u kHz, interface %u, %ux%u mm, touch %s\n",
                 pud->display_data.xres, pud->display_data.yres,
                 pud->display_data.rotate, pud->display_data.bpp,
                 pud->display_data.pixelclock_khz, caps->intf_type,
                 pud->display_data.width_mm, pud->display_data.height_mm,
                 pud->has_touch ? "yes" : "no (not compiled into this firmware)");
}

static int pud_bmp_blit(struct pud *pud, uint8_t *bmp, size_t len)
{
    u8 *jpeg_data;
    ssize_t jpeg_length = 0, actual_length = 0;

    jpeg_data = jpeg_encode_bmp(bmp, len, &jpeg_length);
    if (!jpeg_data)
        return -1;
    actual_length = pud_flush(pud, 0, 0, pud->display->xres - 1,
                              pud->display->yres - 1, jpeg_data, jpeg_length);

    kvfree(jpeg_data);

    if (actual_length != jpeg_length) {
        dev_warn(pud->dev, "Failed to blit bmp data");
        return -1;
    }

    return 0;
}

/* Fallback panel configuration, and the starting point pud_apply_caps()
 * overwrites with whatever the device reports. */
const struct pud_display pud_default_display = {
    .xres   = 480,
    .yres   = 320,
    .bpp    = 16,
    .rotate = 0,
    .fps    = 24,
    /* a 3.5" 480x320 module; the device reports its own numbers when it can */
    .width_mm  = 74,
    .height_mm = 49,
};

static int __maybe_unused pud_fb_steup(struct usb_interface *intf,
                    const struct usb_device_id *id,
                    const struct pud_caps *caps, int caps_len)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct device *dev = &intf->dev;
    struct pud_display display = pud_default_display;
    struct fb_info *info;
    struct pud *pud;
    int rc;

    printk("\n\n%s\n", __func__);

    /* the framebuffer memory size comes from the panel parameters, so they
     * have to be known before it is allocated */
    if (caps_len >= (int)sizeof(*caps))
        pud_caps_to_display(&display, caps);

    info = pud_framebuffer_alloc(&display, dev);
    if (!info)
        return -ENOMEM;

    pud = info->par;
    pud->display_data = display;
    pud->display = &pud->display_data;
    pud->udev = udev;
    pud->dev = dev;
    pud->info = info;
    pud_apply_caps(pud, caps, caps_len);

    pud->encoder_buf = dma_alloc_coherent(pud->dev,
                        info->var.xres * info->var.yres * 2,
                        &pud->encoder_dma, GFP_KERNEL);
    if (!pud->encoder_buf)
        return -ENOMEM;
    pud->encoder_buf_size = info->var.xres * info->var.yres * 2;

    pud->encoder_quality = JPEGE_Q_LOW;

    dev_set_drvdata(dev, pud);

    pud_bmp_blit(pud, rgb565, ARRAY_SIZE(rgb565));

    rc = pud_register_framebuffer(info);
    if (rc) {
        dev_err(pud->dev, "failed to register framebuffer");
        return rc;
    }

    pr_info("%d KB video memory\n", info->fix.smem_len >> 10);

    return 0;
}

static void __maybe_unused pud_fb_cleanup(struct usb_interface *intf)
{
    struct pud *pud = dev_get_drvdata(&intf->dev);
    printk("%s\n", __func__);

    pud_unregister_framebuffer(pud->info);
    pud_framebuffer_release(pud->info);
    if (pud->encoder_buf) {
        dma_free_coherent(pud->dev, pud->encoder_buf_size,
                          pud->encoder_buf, pud->encoder_dma);
        pud->encoder_buf = NULL;
        pud->encoder_buf_size = 0;
    }
}

static int __maybe_unused pud_drm_setup(struct usb_interface *intf,
                    const struct usb_device_id *id,
                    const struct pud_caps *caps, int caps_len)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct device *dev = &intf->dev;
    struct drm_device *drm;
    struct pud *pud;
    int rc;

    printk("\n\n%s\n", __func__);

    /* the DRM mode is built from the panel parameters, so pud_drm_alloc()
     * takes the capability report */
    drm = pud_drm_alloc(dev, caps, caps_len);
    if (IS_ERR(drm))
        return PTR_ERR(drm);

    pud = container_of(drm, struct pud, drm);
    pud->udev = udev;
    pud->dev = dev;
    pud_apply_caps(pud, caps, caps_len);

    usb_set_intfdata(intf, pud);
    pud_bmp_blit(pud, rgb565, ARRAY_SIZE(rgb565));

    rc = pud_drm_register(drm);
    if (rc)
        goto err_free_drm;

    return 0;
err_free_drm:
    pud_drm_release(drm);
    return -1;
}

static void __maybe_unused pud_drm_cleanup(struct usb_interface *intf)
{
    struct pud *pud = usb_get_intfdata(intf);
    struct drm_device *drm = &pud->drm;

    pr_info("%s\n", __func__);
    pud_drm_unregister(drm);
}

/*
 * Touch can be exercised on its own: with input_only=1 the driver registers
 * only the input device, so nothing creates a DRM/fbdev node for a desktop
 * session to hold open and `rmmod` keeps working between test runs.
 */
#if PUD_ENABLE_INPUT_SUPPORT
static bool input_only;
module_param(input_only, bool, 0444);
MODULE_PARM_DESC(input_only,
                 "register only the touch input device, without a display");
#else
#define input_only false
#endif

static int pud_probe(struct usb_interface *intf,
                    const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct pud_caps *caps;
    struct pud *pud;
    u8 *serial;
    int caps_len, rc;

    /* usb_bulk_msg() needs DMA-mappable buffers, not stack ones */
    serial = kzalloc(8, GFP_KERNEL);
    caps = kzalloc(sizeof(*caps), GFP_KERNEL);
    if (!serial || !caps) {
        rc = -ENOMEM;
        goto out_free;
    }

    /* Ask the device for its parameters before anything is registered: the DRM
     * mode and the input device's axis ranges are built from them. */
    caps_len = pud_query_caps(udev, caps);
    if (caps_len < 0) {
        dev_warn(&intf->dev, "no capability report (%d), using defaults\n",
                 caps_len);
        caps_len = 0;
    }

    if (input_only) {
        pud = devm_kzalloc(&intf->dev, sizeof(*pud), GFP_KERNEL);
        if (!pud) {
            rc = -ENOMEM;
            goto out_free;
        }

        pud->dev = &intf->dev;
        pud->udev = udev;
        pud->intf = intf;
        pud->input_only = true;
        pud->display = &pud->display_data;
        pud->display_data = pud_default_display;
        pud_apply_caps(pud, caps, caps_len);
        usb_set_intfdata(intf, pud);
    } else {
#if PUD_DEF_DISP_BACKEND == PUD_DISP_BACKEND_FBDEV
        rc = pud_fb_steup(intf, id, caps, caps_len);
#else
        rc = pud_drm_setup(intf, id, caps, caps_len);
#endif
        if (rc)
            goto out_free;

        pud_read_unique_id(intf, serial, 8);

        pr_info("sn : 0x%02x%02x%02x%02x%02x%02x%02x%02x\n", serial[0], serial[1],
                                                            serial[2], serial[3],
                                                            serial[4], serial[5],
                                                            serial[6], serial[7]);
    }

#if PUD_ENABLE_INPUT_SUPPORT
    /* Input is a bonus, and optional on the device side: a firmware built
     * without a touch driver (most board configs) says so in the capability
     * flags and gets no input device here. */
    if (pud->has_touch) {
        rc = pud_input_setup(intf, id);
        if (rc)
            dev_warn(&intf->dev, "touch input not available: %d\n", rc);
    } else {
        dev_info(&intf->dev,
                 "device reports no touch controller, not registering input\n");
    }
#endif

    rc = 0;
out_free:
    kfree(caps);
    kfree(serial);
    return rc;
}

static void pud_disconnect(struct usb_interface *intf)
{
    struct pud *pud = usb_get_intfdata(intf);

    pud->disconnected = true;

    if (!pud->input_only) {
#if PUD_DEF_DISP_BACKEND == PUD_DISP_BACKEND_FBDEV
        pud_fb_cleanup(intf);
#else
        pud_drm_cleanup(intf);
#endif
    }

#if PUD_ENABLE_INPUT_SUPPORT
    pud_input_cleanup(intf);
#endif
}

static struct usb_device_id pud_ids[] = {
    { USB_DEVICE(0x2E8A, 0x0001) },
    { /* KEEP THIS */ }
};
MODULE_DEVICE_TABLE(usb, pud_ids);

static struct usb_driver pud_drv = {
    .name       = DRV_NAME,
    .probe      = pud_probe,
    .disconnect = pud_disconnect,
    .id_table   = pud_ids,
};
module_usb_driver(pud_drv);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("Pico USB display DRM driver");
MODULE_LICENSE("GPL");
