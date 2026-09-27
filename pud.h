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
#include <linux/input.h>

#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_managed.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_probe_helper.h>

#include <drm/drm_format_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_gem_framebuffer_helper.h>

/* Display backends select, fbdev is default */
#define PUD_DISP_BACKEND_FBDEV 0
#define PUD_DISP_BACKEND_DRM 1

#ifndef PUD_DEF_DISP_BACKEND
#define PUD_DEF_DISP_BACKEND PUD_DISP_BACKEND_DRM
#endif

/* Whether to enable input device */
#ifndef PUD_ENABLE_INPUT_SUPPORT
#define PUD_ENABLE_INPUT_SUPPORT 1
#endif

/* EP1 transfer path, selected by the Makefile's PUD_USB_ASYNC: 0 keeps the
 * synchronous usb_sg interface, 1 uses an asynchronous URB.  usb.c holds both
 * implementations; drm.c only builds the SG table the synchronous one needs. */
#ifndef PUD_USB_ASYNC
#define PUD_USB_ASYNC 0
#endif

// TODO: Currently only support less than 40000 bytes transfer
#define USB_TRANS_MAX_SIZE 65535

/* EP1 framing (protocol v2): every transfer is this header followed by the
 * payload it describes, so the rectangle no longer needs its own control
 * request.  Must match the firmware's struct in include/pud.h. */
#define PUD_EP1_HEADER_SIZE 12

struct pud_ep1_header {
	u16 xs;
	u16 ys;
	u16 xe;
	u16 ye;
	u32 size; /* payload bytes that follow */
};

/* Fallback band budget for a device that does not answer PUD_CMD_GET_CAPS:
 * *half* the host-side ceiling, minus the EP1 header and the QOI worst case
 * (3 bytes per pixel plus the 8-byte header and the 8-byte end marker).  Sized
 * for the smaller firmware on purpose: an RP2040 reports frame_max 32768, and
 * half the ceiling lands on exactly the 10913 pixels per band that its
 * capability report gives.  What the device reports always wins when it
 * answers; a 64 KB firmware that stays silent only gets twice as many bands,
 * where a band that does not fit it is dropped without a retry. */
#define PUD_DEFAULT_BAND_PIXELS \
	((USB_TRANS_MAX_SIZE / 2 - PUD_EP1_HEADER_SIZE - 16) / 3)

/* Device capability report (PUD_CMD_GET_CAPS, EP2 IN).  Must match the
 * firmware's struct in the Pico-USB-Display repo (include/pud.h). */
#define PUD_CAPS_MAGIC 0x43445550 /* "PUDC" */
#define PUD_PROTO_VER 2

#define PUD_CMD_GET_SN 0x01
#define PUD_CMD_GET_CAPS 0x02
#define PUD_CMD_SET_PARAM 0x03 /* control OUT (REQ_SET_PARAM), struct pud_params */
#define PUD_CMD_GET_PARAM 0x04 /* EP2 IN, struct pud_param_state */

/*
 * Runtime parameters, protocol v2.  The host writes a mask plus the values it
 * wants and reads back what is in effect; a field this firmware build cannot
 * change (rotation and the decoder are chosen when the firmware is built) comes
 * back in `rejected` instead of being pretended.  Must match the firmware's
 * include/pud.h and the firmware repo's scripts/pud_usb.py byte for byte.
 */
#define PUD_PARAM_BRIGHTNESS 0x00000001 /* u8, 0..100 percent */
#define PUD_PARAM_ROTATION 0x00000002 /* 0..3, TFT_ROTATION numbering */
/* 0x00000004 is retired: it was `fps`, and the device does not pace frames --
 * EP1 flow control makes the host wait instead.  The bit stays unused rather
 * than being renumbered, so an implementation that already knows it is not
 * silently given a new meaning. */
#define PUD_PARAM_DECODER 0x00000008 /* DECODER_TYPE numbering */

struct pud_params {
	u32 mask; /* PUD_PARAM_*: which of the values below this write sets */
	u8 brightness;
	u8 rotation;
	u8 reserved; /* retired `fps`: keeps the struct at 8 bytes, no padding */
	u8 decoder;
};

struct pud_param_state {
	u32 settable; /* PUD_PARAM_* this firmware can change at runtime */
	u32 rejected; /* PUD_PARAM_* the last PUD_CMD_SET_PARAM could not apply */
	u8 brightness; /* values in effect on the device */
	u8 rotation;
	u8 reserved;
	u8 decoder;
};

/* Wire layouts shared with the firmware: no padding is allowed to creep in. */
static_assert(sizeof(struct pud_params) == 8, "pud_params wire size");
static_assert(sizeof(struct pud_param_state) == 12, "pud_param_state wire size");

/* What the device decodes (firmware DECODER_TYPE, reported by PUD_CMD_GET_CAPS).
 * The numbers are a protocol field: do not renumber.  The driver picks its
 * encoder from this, so a firmware built for another decoder does not get QOI
 * pushed at it. */
#define PUD_DECODER_TJPGD 0
#define PUD_DECODER_JPEGDEC 1
#define PUD_DECODER_LZ4 2
#define PUD_DECODER_QOI 3
#define PUD_DECODER_RLE 4

struct pud_caps {
	u32 magic;
	u32 proto_ver;
	u32 frame_max; /* max bytes per EP1 transfer, header included */
	u32 decoder_type; /* PUD_DECODER_* */

	/* Panel parameters, appended after 2.0.  A firmware that predates them
	 * answers with the first PUD_CAPS_V1_SIZE bytes only, so the host has to
	 * check the length instead of assuming this whole struct arrived. */
	u16 xres; /* panel size in the frame it is driven in */
	u16 yres;
	u16 pixelclock_khz; /* bus clock the panel is driven with */
	u8 rotation; /* TFT_ROTATION the firmware applied */
	u8 bpp;
	u8 intf_type;
	u8 tp_polling_period; /* touch poll period, ms (0 when there is no touch) */
	u16 width_mm; /* active area, for the input resolution (0 = unknown) */
	u16 height_mm;
	u16 flags; /* PUD_CAPS_* */
};

/* Capability flags.  Touch is optional on the device side: most board configs
 * in pico-display-lib have no controller for it, so the input device is only
 * registered when the device says it has one. */
#define PUD_CAPS_TOUCH 0x0001

/* Bytes of struct pud_caps that exist since the first version of the command. */
#define PUD_CAPS_V1_SIZE 16

#define PUD_DEFAULT_TIMEOUT USB_CTRL_SET_TIMEOUT

/*
 * EP4 touch report, one per interrupt IN transfer.  The layout is
 * byte-explicit (the firmware's struct pud_touch_report), so neither end
 * depends on alignment or packing:
 *
 *   0  flags      bit0 = pressed
 *   1  x >> 8     panel coordinates in the frame the panel is driven in,
 *   2  x & 0xff   i.e. the firmware has already applied TFT_ROTATION
 *   3  y >> 8     0..xres-1 / 0..yres-1
 *   4  y & 0xff
 *   5  sequence   wraps; a jump means the host missed (coalesced) a report
 *   6  version    PUD_TOUCH_VERSION, 0 on a device that does not report touch
 *   7  reserved   0
 *
 * The device pushes a report per poll while the panel is held plus one on
 * release; an idle panel sends nothing, so a pending URB parks.
 */
#define PUD_TOUCH_VERSION 1
#define PUD_TOUCH_PRESSED 0x01
#define PUD_TOUCH_REPORT_SIZE 8

#define EP0_IN_ADDR (USB_DIR_IN | 0)
#define EP0_OUT_ADDR (USB_DIR_OUT | 0)
#define EP1_OUT_ADDR (USB_DIR_OUT | 1)
#define EP2_IN_ADDR (USB_DIR_IN | 2)
#define EP4_IN_ADDR (USB_DIR_IN | 4)

#define TYPE_VENDOR 0x40

#define REQ_EP0_OUT 0X00
#define REQ_EP0_IN 0X01
/* Reserved since protocol v2: the rectangle travels in the EP1 header and the
 * device stalls this request, on purpose (an old host must fail loudly). */
#define REQ_EP1_OUT 0X02
#define REQ_EP2_IN 0X03
#define REQ_EP4_IN 0x05
/* Runtime parameters ride EP0: a control OUT carrying the same 4-byte command
 * header as the query path, followed by struct pud_params.  EP3 stays unused
 * (the firmware defines the number but has not put it in its descriptor) and is
 * reserved for a future bulk parameter/config channel. */
#define REQ_SET_PARAM 0X06

/* Command header in front of every vendor request (queries and parameters):
 * u16 cmd, u16 size, little-endian. */
#define PUD_REQ_HEADER_SIZE 4

struct pud_display {
	u32 xres;
	u32 yres;
	u32 bpp;
	u32 fps;
	u32 rotate;
	u32 pixelclock_khz;
	/* Active area in mm: the input device hands libinput an axis resolution in
	 * units/mm so that it does not consider the device a buggy absolute one,
	 * and Mutter uses the device size to pick the output it belongs to. */
	u32 width_mm;
	u32 height_mm;
};

struct pud {
	u64 dma_mask;
	struct device *dev;

	/* USB specific data */
	u8 ctrl_buf[32];
	struct usb_device *udev;
	struct usb_interface *intf;
	bool disconnected;
	/* Loaded with input_only=1: only the touch input device is registered, so
	 * no DRM/fbdev node can be held open and rmmod keeps working. */
	bool input_only;
	/* The device reports whether a touch controller is behind EP4 at all; when
	 * it is not, no input device is registered. */
	bool has_touch;

	/* Device limits, from PUD_CMD_GET_CAPS at probe.  frame_max is the largest
	 * single EP1 transfer the device accepts and max_band_pixels the matching
	 * band budget used by pud_fb_dirty(); both fall back to the host-side
	 * defaults when the device does not report anything. */
	u32 frame_max;
	u32 max_band_pixels;
	u32 decoder_type;
	u32 touch_polling_period; /* ms, as reported by the device */

	/* Framebuffer specific data.  The display parameters come from the device
	 * (PUD_CMD_GET_CAPS), so they live per device instead of in a shared
	 * compile-time struct. */
	struct pud_display display_data;
	struct fb_info *info;
	struct pud_display *display;

	/* Encoder data.  encoder_buf always starts with PUD_EP1_HEADER_SIZE bytes
	 * of header; the encoded payload goes right behind it, and one bulk
	 * transfer carries both. */
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
	/* Consecutive failed EP1 transfers and when the last one happened.  A
	 * device that went away, or a host controller that wedged, otherwise turns
	 * every damage rectangle into a transfer that holds a USB worker for the
	 * whole watchdog -- measured as a load average climbing from 7 to 9 with
	 * lsusb hanging. */
	unsigned int flush_fails;
	unsigned long flush_last_fail;
	struct drm_device drm;
	/* The pipeline is built by hand out of these four: the simple-KMS helpers
	 * that used to bundle plane+CRTC+encoder into one struct are deprecated
	 * upstream and gone from kernels after 7.2.  See pud_drm_create_plane(). */
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;

	/* Input device and related.  One interrupt URB stays pending for the
	 * lifetime of the device; the completion handler re-submits it. */
	struct input_dev *indev;
	struct urb *input_urb;
	unsigned char *ep_int_buf;
};

struct fb_info *pud_framebuffer_alloc(struct pud_display *display,
                                      struct device *dev);
void pud_framebuffer_release(struct fb_info *info);
int pud_register_framebuffer(struct fb_info *info);

/* The one panel configuration to fall back on when the device does not report
 * its own (or reports fewer bytes than the parameter block). */
extern const struct pud_display pud_default_display;

/* PUD_CMD_GET_CAPS: returns the number of bytes the device answered with, or a
 * negative errno.  Uses a throwaway heap object for the DMA-able control
 * buffer, because it runs before struct pud exists. */
int pud_query_caps(struct usb_device *udev, struct pud_caps *caps);

/* Fold a capability report into a device: transfer limits, decoder and, when
 * the device sent the parameter block, the panel size/rotation. */
void pud_apply_caps(struct pud *pud, const struct pud_caps *caps, int caps_len);

/* The panel half of that, for callers that need the parameters before the
 * struct pud exists (the DRM mode and the fbdev memory size are built from
 * them).  The caller is expected to have started from pud_default_display. */
void pud_caps_to_display(struct pud_display *disp, const struct pud_caps *caps);
int pud_unregister_framebuffer(struct fb_info *info);

struct drm_device *pud_drm_alloc(struct device *dev,
                                 const struct pud_caps *caps, int caps_len);
void pud_drm_release(struct drm_device *drm);
int pud_drm_register(struct drm_device *drm);
void pud_drm_unregister(struct drm_device *drm);

int pud_input_setup(struct usb_interface *intf, const struct usb_device_id *id);
int pud_input_cleanup(struct usb_interface *intf);

ssize_t pud_flush(struct pud *pud, u16 x, u16 y, u16 xe, u16 ye,
                  const u8 jpeg_data[], size_t data_size);

#endif
