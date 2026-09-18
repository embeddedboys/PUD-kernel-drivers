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
        pud_DEFAULT_TIMEOUT
    );

    pipe = (addr & USB_DIR_OUT) ? usb_sndbulkpipe(udev, addr) : \
                                  usb_rcvbulkpipe(udev, addr);

    rc = usb_bulk_msg(
        udev,
        pipe,
        (void *)data,
        len,
        &actual_length,
        pud_DEFAULT_TIMEOUT
    );

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

struct req_ep1_out {
    u16 xs;
    u16 ys;
    u16 xe;
    u16 ye;
    u32 size;
};

static inline void pud_feed_ctrl_buf(void *buf, u16 xs, u16 ys, u16 xe, u16 ye,
                                     u32 size)
{
    struct req_ep1_out *req = buf;

    req->xs = xs;
    req->ys = ys;
    req->xe = xe;
    req->ye = ye;
    req->size = size;
}

ssize_t pud_flush(struct pud *pud, u16 x, u16 y, u16 xe, u16 ye,
                  const u8 jpeg_data[], size_t data_size)
{
    struct usb_device *udev = pud->udev;
    struct pud_usb_bulk_context ctx;
    int rc;

    /* data_size must be even for RP2350 */
    if (data_size % 2)
        data_size += 1;

    if (data_size > pud->encoder_buf_size)
        data_size = pud->encoder_buf_size;

    pud_feed_ctrl_buf(pud->ctrl_buf, x, y, xe, ye, data_size);

    // request setup
    rc = usb_control_msg(
        udev,
        usb_sndctrlpipe(udev, EP0_OUT_ADDR),
        REQ_EP1_OUT,
        TYPE_VENDOR | USB_DIR_OUT,
        0, 0,
        pud->ctrl_buf,
        sizeof(pud->ctrl_buf),
        pud_DEFAULT_TIMEOUT
    );

    /* Without a successful window request the device would still use the
     * previous rectangle, so do not send the payload at all. */
    if (rc < 0)
        return rc;

    /* Move the frame into the DMA buffer before the SG bulk send */
    if (jpeg_data != pud->encoder_buf)
        memcpy(pud->encoder_buf, jpeg_data, data_size);

    rc = usb_sg_init(&ctx.sgr, pud->udev, usb_sndbulkpipe(udev, EP1_OUT_ADDR), 0,
                    pud->bulk_sgt.sgl, pud->bulk_sgt.nents, data_size, GFP_KERNEL);

    /* timeout routine in case the device stops reading */
    timer_setup_on_stack(&ctx.timer, pud_usb_bulk_timeout, 0);
    mod_timer(&ctx.timer, jiffies + msecs_to_jiffies(3000));

    usb_sg_wait(&ctx.sgr);

    if (!timer_delete_sync(&ctx.timer))
        rc = -ETIMEDOUT;
    else if (ctx.sgr.status < 0)
        rc = ctx.sgr.status;
    else if (ctx.sgr.bytes != data_size)
        rc = -EIO;
    else
        rc = ctx.sgr.bytes;

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

/* Ask the device how large a single transfer may be (PUD_CMD_GET_CAPS).  The
 * answer sizes the bands pud_fb_dirty() splits a rectangle into, which is what
 * keeps one encoded band inside the device's frame slot.  A device without the
 * command replies with whatever was left in its buffer, so the magic decides;
 * on any mismatch the conservative host-side default stands. */
static void pud_read_caps(struct pud *pud)
{
    struct pud_caps *caps = (struct pud_caps *)pud->ctrl_buf;
    unsigned int frame_max;
    int n;

    BUILD_BUG_ON(sizeof(struct pud_caps) > sizeof(pud->ctrl_buf));

    /* Defaults in case the device stays silent. */
    pud->frame_max = USB_TRANS_MAX_SIZE;
    pud->max_band_pixels = PUD_DEFAULT_BAND_PIXELS;

    n = pud_transfer(pud, PUD_CMD_GET_CAPS, REQ_EP2_IN, EP2_IN_ADDR,
                     pud->ctrl_buf, sizeof(*caps));
    if (n != sizeof(*caps) || caps->magic != PUD_CAPS_MAGIC) {
        dev_warn(pud->dev,
                 "no capability report (%d bytes, magic %#x); keeping %u pixels per band\n",
                 n, caps->magic, pud->max_band_pixels);
        return;
    }

    frame_max = min_t(u32, caps->frame_max, USB_TRANS_MAX_SIZE);
    if (frame_max <= 16) {
        dev_warn(pud->dev, "device reports an unusable frame_max (%u)\n",
                 caps->frame_max);
        return;
    }

    pud->frame_max = frame_max;
    pud->max_band_pixels = (frame_max - 16) / 3;
    pud->decoder_type = caps->decoder_type;
    dev_info(pud->dev,
             "caps: proto %u, frame_max %u, decoder %u -> %u pixels per band\n",
             caps->proto_ver, caps->frame_max, caps->decoder_type,
             pud->max_band_pixels);
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

struct pud_display default_display = {
    .xres   = 480,
    .yres   = 320,
    .bpp    = 16,
    .rotate = 0,
    .fps    = 24,
};

static int __maybe_unused pud_fb_steup(struct usb_interface *intf,
                    const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct device *dev = &intf->dev;
    // struct usb_endpoint_descriptor *endpoint_desc;
    // struct usb_host_interface *interface;
    struct fb_info *info;
    struct pud *pud;
    int rc;

    printk("\n\n%s\n", __func__);

    // interface = intf->cur_altsetting;
    // endpoint_desc = &interface->endpoint[0].desc;
    // printk("num of eps : %d\n", interface->desc.bNumEndpoints);

    // printk("bLength : 0x%02x", endpoint_desc->bLength);
    // printk("bDescriptorType : 0x%02x", endpoint_desc->bDescriptorType);
    // printk("bEndpointAddress : 0x%02x\n", endpoint_desc->bEndpointAddress);
    // printk("bmAttributes : 0x%02x", endpoint_desc->bmAttributes);
    // printk("wMaxPacketSize : 0x%04x", endpoint_desc->wMaxPacketSize);
    // printk("bInterval : 0x%02x\n", endpoint_desc->bInterval);

    info = pud_framebuffer_alloc(&default_display, dev);
    if (!info)
        return -ENOMEM;

    pud = info->par;
    pud->display = &default_display;
    pud->udev = udev;
    pud->dev = dev;
    pud->info = info;

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
                    const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct device *dev = &intf->dev;
    // struct usb_endpoint_descriptor *endpoint_desc;
    // struct usb_host_interface *interface;
    struct drm_device *drm;
    struct pud *pud;
    int rc;

    printk("\n\n%s\n", __func__);

    drm = pud_drm_alloc(dev);
    if (IS_ERR(drm))
        return PTR_ERR(drm);

    pud = container_of(drm, struct pud, drm);
    pud->display = &default_display;
    pud->udev = udev;
    pud->dev = dev;

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

static int pud_probe(struct usb_interface *intf,
                    const struct usb_device_id *id)
{
    u8 *serial;
    int rc;

    /* usb_bulk_msg() needs a DMA-mappable buffer, not a stack one */
    serial = kzalloc(8, GFP_KERNEL);
    if (!serial)
        return -ENOMEM;

#if pud_DEF_DISP_BACKEND == pud_DISP_BACKEND_FBDEV
    rc = pud_fb_steup(intf, id);
#else
    rc = pud_drm_setup(intf, id);
#endif
    if (rc)
        goto out_free_serial;

    pud_read_unique_id(intf, serial, 8);

    pr_info("sn : 0x%02x%02x%02x%02x%02x%02x%02x%02x\n", serial[0], serial[1],
                                                        serial[2], serial[3],
                                                        serial[4], serial[5],
                                                        serial[6], serial[7]);

    /* Device limits (transfer size / band budget) before anything flushes. */
    pud_read_caps(usb_get_intfdata(intf));

#if pud_ENABLE_INPUT_SUPPORT
    pud_input_setup(intf, id);
#endif

    rc = 0;
out_free_serial:
    kfree(serial);
    return rc;
}

static void pud_disconnect(struct usb_interface *intf)
{
    struct pud *pud = usb_get_intfdata(intf);

    pud->disconnected = true;

#if pud_DEF_DISP_BACKEND == pud_DISP_BACKEND_FBDEV
    pud_fb_cleanup(intf);
#else
    pud_drm_cleanup(intf);
#endif

#if pud_ENABLE_INPUT_SUPPORT
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
