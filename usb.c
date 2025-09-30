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
#include <linux/fb.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/timer.h>

#include "udd.h"

// A jpeg image of a panda, binary data
#include "panda.h"

#include "encoder.h"
#include "rgb565.h"

#define DRV_NAME "udd"

struct udd_usb_bulk_context {
    struct timer_list timer;
    struct usb_sg_request sgr;
};

static int udd_transfer(struct udd *udd, u8 cmd, u8 request,
                        u8 addr, u8 *data, size_t len)
{
    struct usb_device *udev = udd->udev;
    int rc, actual_length;
    int pipe;

    udd->ctrl_buf[0] = cmd;
    udd->ctrl_buf[1] = len & 0xff;
    udd->ctrl_buf[2] = len >> 8;

    rc = usb_control_msg(
        udev,
        usb_sndctrlpipe(udev, EP0_OUT_ADDR),
        request,
        TYPE_VENDOR | USB_DIR_OUT,
        0, 0,
        udd->ctrl_buf,
        sizeof(udd->ctrl_buf),
        UDD_DEFAULT_TIMEOUT
    );

    pipe = (addr & USB_DIR_OUT) ? usb_sndbulkpipe(udev, addr) : \
                                  usb_rcvbulkpipe(udev, addr);

    rc = usb_bulk_msg(
        udev,
        pipe,
        (void *)data,
        len,
        &actual_length,
        UDD_DEFAULT_TIMEOUT
    );

    return actual_length;
}

static void udd_usb_bulk_timeout(struct timer_list *t)
{
    struct udd_usb_bulk_context *ctx = from_timer(ctx, t, timer);

    usb_sg_cancel(&ctx->sgr);
}

ssize_t udd_flush(struct udd *udd, u16 x, u16 y, const u8 jpeg_data[], size_t data_size)
{
    struct usb_device *udev = udd->udev;
    struct udd_usb_bulk_context ctx;
    int rc, actual_length;

    /* data_size must be even for RP2350 */
    if (data_size % 2)
        data_size += 1;

    udd->ctrl_buf[0] = (x & 0xff);
    udd->ctrl_buf[1] = (x >> 8);
    udd->ctrl_buf[2] = (y & 0xff);
    udd->ctrl_buf[3] = (y >> 8);
    udd->ctrl_buf[4] = (data_size & 0xff);
    udd->ctrl_buf[5] = (data_size >> 8);

    // request setup
    rc = usb_control_msg(
        udev,
        usb_sndctrlpipe(udev, EP0_OUT_ADDR),
        REQ_EP1_OUT,
        TYPE_VENDOR | USB_DIR_OUT,
        0, 0,
        udd->ctrl_buf,
        sizeof(udd->ctrl_buf),
        UDD_DEFAULT_TIMEOUT
    );

    // rc = usb_bulk_msg(
    //     udev,
    //     usb_sndbulkpipe(udev, EP1_OUT_ADDR),
    //     (void *)jpeg_data,
    //     data_size,
    //     &actual_length,
    //     UDD_DEFAULT_TIMEOUT
    // );

    // return actual_length;

    rc = usb_sg_init(&ctx.sgr, udd->udev, usb_sndbulkpipe(udev, EP1_OUT_ADDR), 0,
                    udd->bulk_sgt.sgl, udd->bulk_sgt.nents, data_size, GFP_KERNEL);

    /* TODO: add timeout process routine */
    timer_setup_on_stack(&ctx.timer, udd_usb_bulk_timeout, 0);
    mod_timer(&ctx.timer, jiffies + msecs_to_jiffies(3000));

    usb_sg_wait(&ctx.sgr);

    if (!timer_delete_sync(&ctx.timer))
        rc = -ETIMEDOUT;
    else if (ctx.sgr.status < 0)
        rc = ctx.sgr.status;
    else if (ctx.sgr.bytes != data_size)
        rc = -EIO;

    destroy_timer_on_stack(&ctx.timer);

    return rc;
}

static int udd_read_unique_id(struct usb_interface *intf, u8 serial[], size_t len)
{
    struct udd *udd = usb_get_intfdata(intf);
    int ret;

    if (len > 8) {
        pr_info("serial length should less than 8!\n");
        return -EINVAL;
    }

    // /* Dummy read, device need to prepares data */
    // ret = udd_transfer(udd, 0x01, REQ_EP2_IN, EP2_IN_ADDR, serial, len);

    ret = udd_transfer(udd, 0x01, REQ_EP2_IN, EP2_IN_ADDR, serial, len);

    return ret;
}

static int udd_bmp_blit(struct udd *udd, uint8_t *bmp, size_t len)
{
    u8 *jpeg_data;
    ssize_t jpeg_length = 0, actual_length = 0;

    jpeg_data = jpeg_encode_bmp(bmp, len, &jpeg_length);
    actual_length = udd_flush(udd, 0, 0, jpeg_data, jpeg_length);

    kfree(jpeg_data);

    if (actual_length != jpeg_length) {
        dev_warn(udd->dev, "Failed to blit bmp data");
        return -1;
    }

    return 0;
}

struct udd_display default_display = {
    .xres   = 480,
    .yres   = 320,
    .bpp    = 16,
    .rotate = 0,
    .fps    = 24,
};

static int __maybe_unused udd_fb_steup(struct usb_interface *intf,
                    const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct device *dev = &intf->dev;
    // struct usb_endpoint_descriptor *endpoint_desc;
    // struct usb_host_interface *interface;
    struct fb_info *info;
    struct udd *udd;
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

    info = udd_framebuffer_alloc(&default_display, dev);
    if (!info)
        return -ENOMEM;

    udd = info->par;
    udd->udev = udev;
    udd->dev = dev;
    udd->info = info;

    udd->encoder_buf = devm_kmalloc(udd->dev, info->var.xres * info->var.yres * 2, GFP_KERNEL);
    if (!udd->encoder_buf)
        return -ENOMEM;

    udd->encoder_quality = JPEGE_Q_LOW;

    dev_set_drvdata(dev, udd);

    udd_bmp_blit(udd, rgb565, ARRAY_SIZE(rgb565));

    rc = udd_register_framebuffer(info);
    if (rc) {
        dev_err(udd->dev, "failed to register framebuffer");
        return rc;
    }

    pr_info("%d KB video memory\n", info->fix.smem_len >> 10);

    return 0;
}

static void __maybe_unused udd_fb_cleanup(struct usb_interface *intf)
{
    struct udd *udd = dev_get_drvdata(&intf->dev);
    printk("%s\n", __func__);

    udd_unregister_framebuffer(udd->info);
    udd_framebuffer_release(udd->info);
}

static int __maybe_unused udd_drm_setup(struct usb_interface *intf,
                    const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct device *dev = &intf->dev;
    // struct usb_endpoint_descriptor *endpoint_desc;
    // struct usb_host_interface *interface;
    struct drm_device *drm;
    struct udd *udd;
    int rc;

    printk("\n\n%s\n", __func__);

    drm = udd_drm_alloc(dev);
    if (!drm)
        return -ENOMEM;

    udd = container_of(drm, struct udd, drm);
    udd->udev = udev;
    udd->dev = dev;

    usb_set_intfdata(intf, udd);
    udd_bmp_blit(udd, rgb565, ARRAY_SIZE(rgb565));

    rc = udd_drm_register(drm);
    if (rc)
        goto err_free_drm;

    return 0;
err_free_drm:
    udd_drm_release(drm);
    return -1;
}

static void __maybe_unused udd_drm_cleanup(struct usb_interface *intf)
{
    struct udd *udd = usb_get_intfdata(intf);
    struct drm_device *drm = &udd->drm;

    pr_info("%s\n", __func__);
    udd_drm_unregister(drm);
}

static int udd_probe(struct usb_interface *intf,
                    const struct usb_device_id *id)
{
    u8 serial[8];

#if UDD_DEF_DISP_BACKEND == UDD_DISP_BACKEND_FBDEV
    udd_fb_steup(intf, id);
#else
    udd_drm_setup(intf, id);
#endif

    udd_read_unique_id(intf, serial, ARRAY_SIZE(serial));

    pr_info("sn : 0x%02x%02x%02x%02x%02x%02x%02x%02x\n", serial[0], serial[1],
                                                        serial[2], serial[3],
                                                        serial[4], serial[5],
                                                        serial[6], serial[7]);

#if UDD_ENABLE_INPUT_SUPPORT
    udd_input_setup(intf, id);
#endif

    return 0;
}

static void udd_disconnect(struct usb_interface *intf)
{
    struct udd *udd = usb_get_intfdata(intf);

    udd->disconnected = true;

#if UDD_DEF_DISP_BACKEND == UDD_DISP_BACKEND_FBDEV
    udd_fb_cleanup(intf);
#else
    udd_drm_cleanup(intf);
#endif

#if UDD_ENABLE_INPUT_SUPPORT
    udd_input_cleanup(intf);
#endif
}

static struct usb_device_id udd_ids[] = {
    { USB_DEVICE(0x2E8A, 0x0001) },
    { /* KEEP THIS */ }
};
MODULE_DEVICE_TABLE(usb, udd_ids);

static struct usb_driver udd_drv = {
    .name       = DRV_NAME,
    .probe      = udd_probe,
    .disconnect = udd_disconnect,
    .id_table   = udd_ids,
};
module_usb_driver(udd_drv);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("USB display device driver");
MODULE_LICENSE("GPL");
