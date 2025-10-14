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
#include <linux/input/touchscreen.h>

#include "pud.h"

static void pud_tp_work(struct work_struct *work)
{
	struct pud *pud = container_of(work, struct pud, work);
	struct urb *urb = pud->input_urb;
	// u8 control_buffer[4] = {0};
	int ret;

	usb_control_msg(pud->udev, usb_sndctrlpipe(pud->udev, EP0_OUT_ADDR),
			REQ_EP4_IN, TYPE_VENDOR | USB_DIR_OUT, 0, 0, NULL, 0,
			pud_DEFAULT_TIMEOUT);

	ret = usb_submit_urb(urb, GFP_KERNEL);
}

static void pud_tp_urb_callback(struct urb *urb)
{
	struct pud *pud = urb->context;
	struct input_dev *indev = pud->indev;
	bool pressed;
	u16 x, y;

	if (urb->status) {
		printk("%s, error status, %d\n", __func__, urb->status);
		return;
	}

	pressed = pud->ep_int_buf[0];
	x = pud->ep_int_buf[1] << 8 | pud->ep_int_buf[2];
	y = pud->ep_int_buf[3] << 8 | pud->ep_int_buf[4];

	input_report_key(indev, BTN_TOUCH, pressed);
	input_report_key(indev, BTN_LEFT, pressed);
	input_report_abs(indev, ABS_X, x);
	input_report_abs(indev, ABS_Y, y);
	// touchscreen_report_pos(indev, &pud->props, x, y, 0);

	input_sync(indev);

	schedule_work(&pud->work);
}

static int pud_tp_indev_open(struct input_dev *dev)
{
	return 0;
}

static void pud_tp_indev_close(struct input_dev *dev)
{
}

int pud_input_setup(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *endpoint_desc;
	struct usb_host_interface *interface;
	struct input_dev *input_dev;
	struct pud *pud;
	int pipe, maxp;
	int i, rc;

	pud = usb_get_intfdata(intf);

	interface = intf->cur_altsetting;
	// printk("num of eps : %d\n", interface->desc.bNumEndpoints);

	for (i = 0; i < interface->desc.bNumEndpoints; i++) {
		if (usb_endpoint_is_int_in(&interface->endpoint[i].desc))
			endpoint_desc = &interface->endpoint[i].desc;
	}

	if (!endpoint_desc) {
		printk("%s, device doesn't have a int endpoint for polling.\n",
		       __func__);
		return -ENODEV;
	}

	pud->input_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!pud->input_urb)
		return -ENOMEM;

	pipe = usb_rcvintpipe(udev, endpoint_desc->bEndpointAddress);
	maxp = usb_maxpacket(pud->udev, pipe);

	pud->ep_int_buf = kmalloc(maxp, GFP_KERNEL);
	if (!pud->ep_int_buf)
		goto free_urb;

	usb_fill_int_urb(pud->input_urb, udev, pipe, pud->ep_int_buf, maxp,
			 pud_tp_urb_callback, pud, endpoint_desc->bInterval);

	INIT_WORK(&pud->work, pud_tp_work);

	input_dev = devm_input_allocate_device(&intf->dev);
	if (!input_dev) {
		printk("Failed to allocate input dev.\n");
		return -ENOMEM;
	}

	input_dev->dev.parent = &intf->dev;
	input_dev->name = "pud touch panel";
	input_dev->id.bustype = BUS_USB;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_dev->keybit[BIT_WORD(BTN_LEFT)] = BIT_MASK(BTN_LEFT);

	/* TODO: query from device via pud protocal */
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, 480, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, 320, 0, 0);

	/* TODO: multitouch support  */
	input_mt_init_slots(input_dev, 1,
			    INPUT_MT_DIRECT | INPUT_MT_TRACK |
				    INPUT_MT_DROP_UNUSED);

	input_set_drvdata(input_dev, pud);

	input_dev->open = pud_tp_indev_open;
	input_dev->close = pud_tp_indev_close;

	pud->indev = input_dev;

	rc = input_register_device(input_dev);
	if (rc) {
		printk("Failed to register input dev.\n");
		goto free_buf;
	}

	schedule_work(&pud->work);
	return 0;

free_buf:
	kfree(pud->ep_int_buf);
free_urb:
	usb_free_urb(pud->input_urb);
	return -1;
}

int pud_input_cleanup(struct usb_interface *intf)
{
	struct pud *pud = dev_get_drvdata(&intf->dev);

	printk("%s\n", __func__);
	input_unregister_device(pud->indev);

	usb_kill_urb(pud->input_urb);
	usb_free_urb(pud->input_urb);

	kfree(pud->ep_int_buf);

	return 0;
}
