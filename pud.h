// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2025 embeddedboys, Ltd.
 *
 * Author: Zheng Hua <hua.zheng@embeddedboys.com>
 */

#ifndef __pud_H
#define __pud_H

#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/input/touchscreen.h>

#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_managed.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_probe_helper.h>

#include <drm/drm_format_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>

/* Display backends select, fbdev is default */
#define pud_DISP_BACKEND_FBDEV 0
#define pud_DISP_BACKEND_DRM   1

#ifndef pud_DEF_DISP_BACKEND
    #define pud_DEF_DISP_BACKEND pud_DISP_BACKEND_DRM
#endif

/* Whether to enable input device */
#ifndef pud_ENABLE_INPUT_SUPPORT
    #define pud_ENABLE_INPUT_SUPPORT    1
#endif

// TODO: Currently only support less than 40000 bytes transfer
#define USB_TRANS_MAX_SIZE  65535

/* Conservative band budget: one transfer of the host-side ceiling, QOI worst
 * case (3 bytes per pixel plus the 8-byte header and 8-byte end marker).  Used
 * until the device reports its own limit through PUD_CMD_GET_CAPS; an RP2040
 * firmware accepts half-size transfers, so the reported value must win. */
#define PUD_DEFAULT_BAND_PIXELS ((USB_TRANS_MAX_SIZE - 16) / 3)

/* Device capability report (PUD_CMD_GET_CAPS, EP2 IN).  Must match the
 * firmware's struct in the Pico-USB-Display repo (include/pud.h). */
#define PUD_CAPS_MAGIC 0x43445550 /* "PUDC" */
#define PUD_PROTO_VER  1

#define PUD_CMD_GET_SN   0x01
#define PUD_CMD_GET_CAPS 0x02

struct pud_caps {
    u32 magic;
    u32 proto_ver;
    u32 frame_max;      /* max bytes the device accepts in one EP1 transfer */
    u32 decoder_type;   /* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI */
};

#define pud_DEFAULT_TIMEOUT USB_CTRL_SET_TIMEOUT

#define EP0_IN_ADDR  (USB_DIR_IN  | 0)
#define EP0_OUT_ADDR (USB_DIR_OUT | 0)
#define EP1_OUT_ADDR (USB_DIR_OUT | 1)
#define EP2_IN_ADDR  (USB_DIR_IN  | 2)
#define EP4_IN_ADDR  (USB_DIR_IN  | 4)

#define TYPE_VENDOR 0x40

#define REQ_EP0_OUT  0X00
#define REQ_EP0_IN   0X01
#define REQ_EP1_OUT  0X02
#define REQ_EP2_IN   0X03
#define REQ_EP4_IN   0x05

struct pud_display {
    u32     xres;
    u32     yres;
    u32     bpp;
    u32     fps;
    u32     rotate;
};

struct pud {
    u64 dma_mask;
    struct device          *dev;

    /* USB specific data */
    u8 ctrl_buf[16];
    struct usb_device      *udev;
    struct usb_interface   *intf;
    bool                    disconnected;

    /* Device limits, from PUD_CMD_GET_CAPS at probe.  frame_max is the largest
     * single EP1 transfer the device accepts and max_band_pixels the matching
     * band budget used by pud_fb_dirty(); both fall back to the host-side
     * defaults when the device does not report anything. */
    u32 frame_max;
    u32 max_band_pixels;
    u32 decoder_type;

    /* Framebuffer specific data */
    struct fb_info        *info;
    struct pud_display    *display;

    /* Encoder data */
    u8 *encoder_buf;
    dma_addr_t encoder_dma;
    size_t encoder_buf_size;
    u8 encoder_quality;

    /* DRM specific data */
    u16 *tx_buf;
    struct sg_table bulk_sgt;
    u32 pixel_format;
    /* Set when a flush fails: that damage rectangle is lost, so the next
     * update repaints the whole screen to clear any stale region. */
    bool needs_full_refresh;
    struct drm_device drm;
    struct drm_simple_display_pipe pipe;
    struct drm_connector connector;
    struct drm_display_mode mode;

    /* Input device and related */
    struct input_dev *indev;
    struct urb *input_urb;
    unsigned char *ep_int_buf;
    struct work_struct work;
    struct touchscreen_properties props;
};

struct fb_info *pud_framebuffer_alloc(struct pud_display *display,
                                      struct device *dev);
void pud_framebuffer_release(struct fb_info *info);
int pud_register_framebuffer(struct fb_info *info);
int pud_unregister_framebuffer(struct fb_info *info);

struct drm_device *pud_drm_alloc(struct device *dev);
void pud_drm_release(struct drm_device *drm);
int pud_drm_register(struct drm_device *drm);
void pud_drm_unregister(struct drm_device *drm);

int pud_input_setup(struct usb_interface *intf, const struct usb_device_id *id);
int pud_input_cleanup(struct usb_interface *intf);

ssize_t pud_flush(struct pud *pud, u16 x, u16 y, u16 xe, u16 ye,
                  const u8 jpeg_data[], size_t data_size);

#endif
