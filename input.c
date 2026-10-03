// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2025 embeddedboys, Ltd.
 *
 * Author: Zheng Hua <hua.zheng@embeddedboys.com>
 */

#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/input/mt.h>

#include "pud.h"

/* Touch uses direct MT-B reports; pointer maps ABS_X/Y across the desktop.
 * Both need physical axis resolution for userspace display matching.
 */
static char *report_mode = "touch";
module_param(report_mode, charp, 0444);
MODULE_PARM_DESC(report_mode, "input device type: touch (default) or pointer");

static bool pud_report_touch(void)
{
	return strcmp(report_mode, "pointer") != 0;
}

static void pud_report_sample(struct pud *pud, const u8 *report)
{
	struct input_dev *indev = pud->indev;
	bool pressed;
	u16 x, y;

	pressed = !!(report[0] & PUD_TOUCH_PRESSED);
	x = (u16)(report[1] << 8 | report[2]);
	y = (u16)(report[3] << 8 | report[4]);

	if (pud_report_touch()) {
		input_mt_slot(indev, 0);
		input_mt_report_slot_state(indev, MT_TOOL_FINGER,
		                           pressed);
		if (pressed) {
			input_report_abs(indev, ABS_MT_POSITION_X, x);
			input_report_abs(indev, ABS_MT_POSITION_Y, y);
		}
		input_report_abs(indev, ABS_X, x);
		input_report_abs(indev, ABS_Y, y);
		input_report_key(indev, BTN_TOUCH, pressed);
	} else {
		input_report_key(indev, BTN_LEFT, pressed);
		input_report_abs(indev, ABS_X, x);
		input_report_abs(indev, ABS_Y, y);
	}
	input_sync(indev);
}

/* Keep one interrupt URB pending. Only cancellation/unload ends the stream. */
static void pud_tp_urb_callback(struct urb *urb)
{
	struct pud *pud = urb->context;
	const u8 *report = pud->ep_int_buf;

	if (urb->status) {
		switch (urb->status) {
		case -ENOENT: /* killed on unload */
		case -ECONNRESET:
		case -ESHUTDOWN:
			return;
		default:
			/* Idle panels park the URB for as long as nobody
			 * touches them, so a status here is a real hiccup, but
			 * still not a reason to stop. */
			dev_dbg(pud->dev, "touch urb status %d\n", urb->status);
			break;
		}
	} else if (urb->actual_length < PUD_TOUCH_REPORT_SIZE) {
		dev_warn_ratelimited(pud->dev,
		                     "short touch report (%u bytes)\n",
		                     urb->actual_length);
	} else if (report[6] != PUD_TOUCH_VERSION) {
		/* version 0 is a device that never reports anything, which is
		 * fine: it just means touch is not implemented there */
		if (report[6])
			dev_warn_once(pud->dev,
			              "touch report version %u, expected %u\n",
			              report[6], PUD_TOUCH_VERSION);
	} else {
		pud_report_sample(pud, report);
	}

	if (!pud->disconnected)
		usb_submit_urb(urb, GFP_ATOMIC);
}

static int pud_input_configure_axes(struct pud *pud, struct input_dev *input_dev)
{
	unsigned int xmax, ymax;
	int rc;

	/* Coordinates are panel coordinates in the frame the panel is driven in:
	 * the firmware applies TFT_ROTATION and clamps before reporting, so
	 * there is nothing to swap or scale on this side. */
	xmax = pud->display ? pud->display->xres - 1 : 479;
	ymax = pud->display ? pud->display->yres - 1 : 319;

	input_set_abs_params(input_dev, ABS_X, 0, xmax, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, ymax, 0, 0);

	/*
	 * libinput considers an absolute device without a valid resolution a
	 * kernel bug, and Mutter compares the size that resolution implies with
	 * the outputs when it decides where a touchscreen belongs.  The device
	 * reports its active area; without it there is nothing sensible to set.
	 */
	if (pud->display && pud->display->width_mm && pud->display->height_mm) {
		input_abs_set_res(input_dev, ABS_X,
		                  DIV_ROUND_CLOSEST(xmax + 1,
		                                    pud->display->width_mm));
		input_abs_set_res(input_dev, ABS_Y,
		                  DIV_ROUND_CLOSEST(ymax + 1,
		                                    pud->display->height_mm));
	} else {
		dev_warn(
		        pud->dev,
		        "panel size unknown, leaving the input resolution at 0\n");
	}

	if (pud_report_touch()) {
		input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, xmax, 0,
		                     0);
		input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, ymax, 0,
		                     0);
		if (pud->display && pud->display->width_mm &&
		    pud->display->height_mm) {
			input_abs_set_res(
			        input_dev, ABS_MT_POSITION_X,
			        DIV_ROUND_CLOSEST(xmax + 1,
			                          pud->display->width_mm));
			input_abs_set_res(
			        input_dev, ABS_MT_POSITION_Y,
			        DIV_ROUND_CLOSEST(ymax + 1,
			                          pud->display->height_mm));
		}

		input_set_capability(input_dev, EV_KEY, BTN_TOUCH);
		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

		rc = input_mt_init_slots(
		        input_dev, 1, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
		if (rc) {
			dev_err(pud->dev, "failed to init MT slots: %d\n",
			        rc);
			return rc;
		}
	} else {
		input_set_capability(input_dev, EV_KEY, BTN_LEFT);
		input_set_capability(input_dev, EV_KEY, BTN_RIGHT);
	}

	return 0;
}

int pud_input_setup(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *endpoint_desc = NULL;
	struct usb_host_interface *interface;
	struct input_dev *input_dev;
	struct pud *pud;
	int pipe, maxp;
	int i, rc;

	pud = usb_get_intfdata(intf);
	if (!pud)
		return -EINVAL;

	interface = intf->cur_altsetting;

	for (i = 0; i < interface->desc.bNumEndpoints; i++) {
		if (usb_endpoint_is_int_in(&interface->endpoint[i].desc)) {
			endpoint_desc = &interface->endpoint[i].desc;
			break;
		}
	}

	if (!endpoint_desc) {
		dev_err(&intf->dev, "device has no interrupt IN endpoint\n");
		return -ENODEV;
	}

	pud->input_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!pud->input_urb)
		return -ENOMEM;

	pipe = usb_rcvintpipe(udev, endpoint_desc->bEndpointAddress);
	maxp = usb_maxpacket(udev, pipe);
	if (maxp < PUD_TOUCH_REPORT_SIZE)
		maxp = PUD_TOUCH_REPORT_SIZE;

	pud->ep_int_buf = kzalloc(maxp, GFP_KERNEL);
	if (!pud->ep_int_buf) {
		rc = -ENOMEM;
		goto free_urb;
	}

	usb_fill_int_urb(pud->input_urb, udev, pipe, pud->ep_int_buf, maxp,
	                 pud_tp_urb_callback, pud, endpoint_desc->bInterval);

	input_dev = devm_input_allocate_device(&intf->dev);
	if (!input_dev) {
		rc = -ENOMEM;
		goto free_buf;
	}

	input_dev->dev.parent = &intf->dev;
	input_dev->name = "pud touch panel";
	/* The USB ids, so userspace can identify the device -- and so the
	 * per-device settings path Mutter builds from vendor:product (used to
	 * pick the output a touchscreen belongs to) is stable instead of
	 * 0000:0000. */
	input_dev->id.bustype = BUS_USB;
	input_dev->id.vendor = le16_to_cpu(udev->descriptor.idVendor);
	input_dev->id.product = le16_to_cpu(udev->descriptor.idProduct);
	input_dev->id.version = le16_to_cpu(udev->descriptor.bcdDevice);

	rc = pud_input_configure_axes(pud, input_dev);
	if (rc)
		goto free_buf;

	input_set_drvdata(input_dev, pud);

	rc = input_register_device(input_dev);
	if (rc) {
		dev_err(&intf->dev, "failed to register input device: %d\n",
		        rc);
		goto free_buf;
	}

	pud->indev = input_dev;

	/* the device pushes by itself, so this is the only submit we make */
	rc = usb_submit_urb(pud->input_urb, GFP_KERNEL);
	if (rc) {
		dev_err(&intf->dev, "failed to submit touch urb: %d\n", rc);
		pud->indev = NULL;
		goto free_buf;
	}

	return 0;

free_buf:
	kfree(pud->ep_int_buf);
	pud->ep_int_buf = NULL;
free_urb:
	usb_free_urb(pud->input_urb);
	pud->input_urb = NULL;
	return rc;
}

int pud_input_cleanup(struct usb_interface *intf)
{
	struct pud *pud = usb_get_intfdata(intf);

	if (!pud)
		return 0;

	/* The completion handler re-submits the URB, so it has to be dead before
	 * the URB and its buffer go away.  usb_kill_urb() waits for a callback
	 * that is already running; the -ENOENT arm above then keeps that one
	 * from re-submitting. */
	if (pud->input_urb) {
		usb_kill_urb(pud->input_urb);
		usb_free_urb(pud->input_urb);
		pud->input_urb = NULL;
	}

	kfree(pud->ep_int_buf);
	pud->ep_int_buf = NULL;

	/* pud->indev comes from devm_input_allocate_device(), and devres
	 * unregisters it when the interface goes away -- unregistering it here
	 * as well would be a double unregister. */
	pud->indev = NULL;

	return 0;
}
