// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2025 embeddedboys, Ltd.
 *
 * Author: Zheng Hua <hua.zheng@embeddedboys.com>
 */

#define DRV_NAME "pud-fb"
#define pr_fmt(fmt) DRV_NAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/fb.h>

#include "pud.h"
#include "encoder.h"

static ssize_t pud_fb_read(struct fb_info *info, char __user *buf, size_t count,
                           loff_t *ppos)
{
	pr_info("%s\n", __func__);
	return fb_sys_read(info, buf, count, ppos);
}

static ssize_t pud_fb_write(struct fb_info *info, const char __user *buf,
                            size_t count, loff_t *ppos)
{
	ssize_t ret = 0;
	pr_info("%s: count=%zd, ppos=%llu\n", __func__, count, *ppos);
	ret = fb_sys_write(info, buf, count, ppos);
	schedule_delayed_work(&info->deferred_work, info->fbdefio->delay);
	return ret;
}

static void pud_fb_fillrect(struct fb_info *info,
                            const struct fb_fillrect *rect)
{
	pr_info("%s\n", __func__);
	sys_fillrect(info, rect);
}

static void pud_fb_copyarea(struct fb_info *info,
                            const struct fb_copyarea *area)
{
	pr_info("%s\n", __func__);
	sys_copyarea(info, area);
}

static void pud_fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	pr_info("%s\n", __func__);
	/* Image blits are not implemented by the legacy backend. */
}

static int pud_fb_setcolreg(unsigned int regno, unsigned int red,
                            unsigned int green, unsigned int blue,
                            unsigned int transp, struct fb_info *info)
{
	/* The legacy backend has no programmable palette. */
	return 0;
}

static int pud_fb_blank(int blank, struct fb_info *info)
{
	int ret = -EINVAL;

	switch (blank) {
	case FB_BLANK_POWERDOWN:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_NORMAL:
		pr_info("%s, blank\n", __func__);
		break;
	case FB_BLANK_UNBLANK:
		pr_info("%s, unblank\n", __func__);
		break;
	}
	return ret;
}

static void pud_fb_deferred_io(struct fb_info *info,
                               struct list_head *pagereflist)
{
	size_t jpeg_length = 0;
	int rc;
	struct pud *pud = info->par;

	rc = jpeg_encode_rgb565(info->screen_buffer, info->var.xres,
	                       info->var.yres,
	                       pud_payload_capacity(pud),
	                       pud->encoder_buf + PUD_EP1_HEADER_SIZE,
	                       &jpeg_length, pud->encoder_quality);
	if (rc) {
		dev_err_ratelimited(pud->dev, "JPEG encode failed: %d\n", rc);
		return;
	}

	/* Full-screen JPEG cannot be split or truncated to fit the transfer. */
	if (ALIGN(jpeg_length, 2) + PUD_EP1_HEADER_SIZE > pud->frame_max) {
		dev_err_once(
		        pud->dev,
		        "full-screen JPEG is %zu bytes, device accepts %u; not sending\n",
		        jpeg_length, pud->frame_max);
		return;
	}

	pud_flush(pud, 0, 0, info->var.xres - 1, info->var.yres - 1,
	          pud->encoder_buf + PUD_EP1_HEADER_SIZE, jpeg_length);
}

struct fb_info *pud_framebuffer_alloc(struct pud_display *display,
                                      struct device *dev)
{
	struct fb_deferred_io *fbdefio;
	struct fb_ops *fbops;
	struct fb_info *info;
	int width, height, bpp, rotate;
	u8 *vmem = NULL;
	int vmem_size;

	pr_info("%s\n", __func__);

	width = display->xres;
	height = display->yres;
	bpp = display->bpp;
	rotate = display->rotate;

	vmem_size = (width * height * bpp) / BITS_PER_BYTE;
	pr_info("vmem_size: %d\n", vmem_size);
	vmem = vmalloc(vmem_size);
	if (!vmem) {
		pr_err("failed to allocate vmem\n");
		return NULL;
	}

	fbops = kzalloc(sizeof(*fbops), GFP_KERNEL);
	if (!fbops) {
		pr_err("failed to allocate fbops\n");
		goto err_free_vmem;
	}

	fbdefio = kzalloc(sizeof(*fbdefio), GFP_KERNEL);
	if (!fbdefio) {
		pr_err("failed to allocate fbdefio\n");
		goto err_free_fbops;
	}

	info = framebuffer_alloc(sizeof(struct pud), dev);
	if (!info) {
		pr_err("failed to allocate info\n");
		goto err_free_fbdefio;
	}

	info->screen_buffer = vmem;
	info->fbops = fbops;
	info->fbdefio = fbdefio;

	fbops->owner = THIS_MODULE;
	fbops->fb_read = pud_fb_read;
	fbops->fb_write = pud_fb_write;
	fbops->fb_fillrect = pud_fb_fillrect;
	fbops->fb_copyarea = pud_fb_copyarea;
	fbops->fb_imageblit = pud_fb_imageblit;
	fbops->fb_setcolreg = pud_fb_setcolreg;
	fbops->fb_blank = pud_fb_blank;

// TODO: Find out which version requires mmap to be implemented.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	fbops->fb_mmap = fb_deferred_io_mmap;
#endif

	snprintf(info->fix.id, sizeof(info->fix.id), "%s", DRV_NAME);
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.xpanstep = 0;
	info->fix.ypanstep = 0;
	info->fix.ywrapstep = 0;
	info->fix.line_length = width * bpp / BITS_PER_BYTE;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.smem_len = vmem_size;

	info->var.rotate = rotate;
	info->var.xres = width;
	info->var.yres = height;
	info->var.xres_virtual = info->var.xres;
	info->var.yres_virtual = info->var.yres;
	info->var.bits_per_pixel = bpp;
	info->var.nonstd = 1;
	info->var.grayscale = 0;

	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.transp.offset = 0;
	info->var.transp.length = 0;

	info->flags = FBINFO_VIRTFB;

	fbdefio->delay = HZ / display->fps;
	fbdefio->sort_pagereflist = true;
	fbdefio->deferred_io = pud_fb_deferred_io;
	fb_deferred_io_init(info);

	return info;

err_free_fbdefio:
	kfree(fbdefio);
err_free_fbops:
	kfree(fbops);
err_free_vmem:
	vfree(vmem);
	return NULL;
}

void pud_framebuffer_release(struct fb_info *info)
{
	fb_deferred_io_cleanup(info);
	kfree(info->fbdefio);
	kfree(info->fbops);
	vfree(info->screen_buffer);
	framebuffer_release(info);
}

int pud_register_framebuffer(struct fb_info *info)
{
	int rc;
	rc = register_framebuffer(info);
	return rc;
}

int pud_unregister_framebuffer(struct fb_info *info)
{
	unregister_framebuffer(info);
	return 0;
}
