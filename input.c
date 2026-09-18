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

/*
 * How the panel presents itself to userspace.  Both modes are absolute devices
 * (libinput's absolute types are multi-touch screens, single-touch screens and
 * tablets); they differ in what a desktop does with them:
 *
 *   touch   (default) INPUT_PROP_DIRECT + MT-B + BTN_TOUCH: a touchscreen, so
 *           touching the panel acts where you touch -- as long as the
 *           compositor maps it to this output (GNOME has no way to choose that
 *           for an external touchscreen, it matches on the device size, which
 *           is why the panel size is reported).
 *   pointer ABS_X/Y + BTN_LEFT without INPUT_PROP_DIRECT: an absolute pointer
 *           (the QEMU usb-tablet kind), mapped across the whole desktop.  It
 *           always works, at the price of the panel being a position pad
 *           rather than "touch what you see".
 *
 * MT-B is used for the touch mode because that is what libinput's touchscreen
 * gesture/tracking code expects; a single finger just occupies slot 0.
 */
static char *report_mode = "touch";
module_param(report_mode, charp, 0444);
MODULE_PARM_DESC(report_mode, "input device type: touch (default) or pointer");

static bool pud_report_touch(void)
{
	return strcmp(report_mode, "pointer") != 0;
}

/*
 * EP4 is an interrupt IN endpoint the device pushes touch reports on: one per
 * poll while the panel is held, one when it is released, and nothing at all
 * while it is idle.  The host therefore keeps exactly one URB pending and
 * re-submits it from the completion handler -- there is no control request to
 * ask for a sample, and no error may end the stream, or input would stay dead
 * until the next replug.
 */
static void pud_tp_urb_callback(struct urb *urb)
{
	struct pud *pud = urb->context;
	struct input_dev *indev = pud->indev;
	const u8 *report = pud->ep_int_buf;
	bool pressed;
	u16 x, y;

	if (urb->status) {
		switch (urb->status) {
		case -ENOENT:		/* killed on unload */
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
		dev_warn_ratelimited(pud->dev, "short touch report (%u bytes)\n",
				     urb->actual_length);
	} else if (report[6] != PUD_TOUCH_VERSION) {
		/* version 0 is a device that never reports anything, which is
		 * fine: it just means touch is not implemented there */
		if (report[6])
			dev_warn_once(pud->dev,
				      "touch report version %u, expected %u\n",
				      report[6], PUD_TOUCH_VERSION);
	} else {
		pressed = !!(report[0] & PUD_TOUCH_PRESSED);
		x = (u16)(report[1] << 8 | report[2]);
		y = (u16)(report[3] << 8 | report[4]);

		if (pud_report_touch()) {
			input_mt_slot(indev, 0);
			input_mt_report_slot_state(indev, MT_TOOL_FINGER, pressed);
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

	if (!pud->disconnected)
		usb_submit_urb(urb, GFP_ATOMIC);
}

int pud_input_setup(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *endpoint_desc = NULL;
	struct usb_host_interface *interface;
	struct input_dev *input_dev;
	struct pud *pud;
	unsigned int xmax, ymax;
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
	input_dev->id.bustype = BUS_USB;

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
				  DIV_ROUND_CLOSEST(xmax + 1, pud->display->width_mm));
		input_abs_set_res(input_dev, ABS_Y,
				  DIV_ROUND_CLOSEST(ymax + 1, pud->display->height_mm));
	} else {
		dev_warn(&intf->dev, "panel size unknown, leaving the input resolution at 0\n");
	}

	if (pud_report_touch()) {
		input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, xmax, 0, 0);
		input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, ymax, 0, 0);
		if (pud->display && pud->display->width_mm && pud->display->height_mm) {
			input_abs_set_res(input_dev, ABS_MT_POSITION_X,
					  DIV_ROUND_CLOSEST(xmax + 1, pud->display->width_mm));
			input_abs_set_res(input_dev, ABS_MT_POSITION_Y,
					  DIV_ROUND_CLOSEST(ymax + 1, pud->display->height_mm));
		}

		input_set_capability(input_dev, EV_KEY, BTN_TOUCH);
		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

		rc = input_mt_init_slots(input_dev, 1,
					 INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
		if (rc) {
			dev_err(&intf->dev, "failed to init MT slots: %d\n", rc);
			goto free_buf;
		}
	} else {
		input_set_capability(input_dev, EV_KEY, BTN_LEFT);
		input_set_capability(input_dev, EV_KEY, BTN_RIGHT);
	}

	input_set_drvdata(input_dev, pud);

	rc = input_register_device(input_dev);
	if (rc) {
		dev_err(&intf->dev, "failed to register input device: %d\n", rc);
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
