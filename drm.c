// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2025 embeddedboys, Ltd.
 *
 * Author: Zheng Hua <hua.zheng@embeddedboys.com>
 */

#define pr_fmt(fmt) "pud-drm: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <video/mipi_display.h>

#include "pud.h"
#include "encoder.h"

#define DRV_NAME "pud-drm"

static inline struct pud *drm_to_pud(struct drm_device *drm)
{
	return container_of(drm, struct pud, drm);
}

static enum drm_mode_status
pud_drm_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			const struct drm_display_mode *mode)
{
	struct pud *pud = drm_to_pud(pipe->crtc.dev);
	int rc;
	rc = drm_crtc_helper_mode_valid_fixed(&pipe->crtc, mode, &pud->mode);
	pr_info("%s, rc: %d\n", __func__, rc);
	return rc;
}

static void pud_drm_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	pr_info("%s\n", __func__);
}

static void pud_drm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	pr_info("%s\n", __func__);
}

static int pud_buf_copy(void *dst, struct iosys_map *src,
			struct drm_framebuffer *fb, struct drm_rect *clip,
			bool swap)
{
	struct pud *pud = drm_to_pud(fb->dev);
	// struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct iosys_map dst_map = IOSYS_MAP_INIT_VADDR(dst);
	int ret;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	switch (fb->format->format) {
	// case DRM_FORMAT_RGB565:
	//     if (swap)
	//         drm_fb_swab(&dst_map, NULL, src, fb, clip, !gem->import_attach);
	//     else
	//         drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
	//     break;
	// case DRM_FORMAT_RGB888:
	//     drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
	//     break;
	case DRM_FORMAT_XRGB8888:
		switch (pud->pixel_format) {
		case DRM_FORMAT_RGB565:
			drm_fb_xrgb8888_to_rgb565(&dst_map, NULL, src, fb, clip,
						  swap);
			break;
			// case DRM_FORMAT_RGB888:
			//     drm_fb_xrgb8888_to_rgb888(&dst_map, NULL, src, fb, clip);
			//     break;
		}
		break;
	default:
		drm_err_once(fb->dev, "Format is not supported: %p4cc\n",
			     &fb->format->format);
		ret = -EINVAL;
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	return ret;
}

static void pud_fb_dirty(struct iosys_map *src, struct drm_framebuffer *fb,
			 struct drm_rect *rect)
{
	struct pud *pud = drm_to_pud(fb->dev);
	unsigned int height = rect->y2 - rect->y1;
	unsigned int width = rect->x2 - rect->x1;
	ssize_t jpeg_length = 0;
	bool swap = false;
	int ret = 0;
	void *tr;

	tr = pud->tx_buf;
	ret = pud_buf_copy(tr, src, fb, rect, swap);

	jpeg_encode_rgb565(tr, width, height, width * height * sizeof(u16),
			   pud->encoder_buf, &jpeg_length,
			   pud->encoder_quality);

	// pr_info("%s, w: %d, h: %d, len : %ld\n", __func__, width, height, jpeg_length);
	if (jpeg_length > USB_TRANS_MAX_SIZE)
		jpeg_length = USB_TRANS_MAX_SIZE - 1;

	pud_flush(pud, rect->x1, rect->y1, pud->encoder_buf, jpeg_length);
}

static void pud_drm_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(state);
	struct drm_framebuffer *fb = state->fb;
	struct drm_rect rect;
	int idx;

	if (!pipe->crtc.state->active)
		return;

	if (WARN_ON(!fb))
		return;

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect)) {
		drm_dbg(fb->dev, "Flushing [FB:%d] " DRM_RECT_FMT "\n",
			fb->base.id, DRM_RECT_ARG(&rect));
		pud_fb_dirty(&shadow_plane_state->data[0], fb, &rect);
	}

	drm_dev_exit(idx);
}

static int pud_drm_pipe_begin_fb_access(struct drm_simple_display_pipe *pipe,
					struct drm_plane_state *plane_state)
{
	return drm_gem_begin_shadow_fb_access(&pipe->plane, plane_state);
}

static void pud_drm_pipe_end_fb_access(struct drm_simple_display_pipe *pipe,
				       struct drm_plane_state *plane_state)
{
	drm_gem_end_shadow_fb_access(&pipe->plane, plane_state);
}

static void pud_drm_pipe_reset_plane(struct drm_simple_display_pipe *pipe)
{
	drm_gem_reset_shadow_plane(&pipe->plane);
}

static struct drm_plane_state *
pud_drm_pipe_duplicate_plane_state(struct drm_simple_display_pipe *pipe)
{
	return drm_gem_duplicate_shadow_plane_state(&pipe->plane);
}

static void
pud_drm_pipe_destroy_plane_state(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *plane_state)
{
	drm_gem_destroy_shadow_plane_state(&pipe->plane, plane_state);
}

static const struct drm_simple_display_pipe_funcs pud_display_pipe_funcs = {
	.mode_valid = pud_drm_pipe_mode_valid,
	.enable = pud_drm_pipe_enable,
	.disable = pud_drm_pipe_disable,
	.update = pud_drm_pipe_update,
	.begin_fb_access = pud_drm_pipe_begin_fb_access,
	.end_fb_access = pud_drm_pipe_end_fb_access,
	.reset_plane = pud_drm_pipe_reset_plane,
	.duplicate_plane_state = pud_drm_pipe_duplicate_plane_state,
	.destroy_plane_state = pud_drm_pipe_destroy_plane_state,
};

static int pud_connector_get_modes(struct drm_connector *connector)
{
	struct pud *pud = drm_to_pud(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &pud->mode);
}

static const struct drm_connector_helper_funcs pud_connector_hfuncs = {
	.get_modes = pud_connector_get_modes,
};

static const struct drm_connector_funcs pud_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs pud_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const uint32_t pud_drm_formats[] = {
	DRM_FORMAT_RGB565, /* device pixel format */
	DRM_FORMAT_XRGB8888, /* DRM driver framebuffer format */
};

static const struct drm_display_mode pud_disp_mode = {
	DRM_MODE_INIT(60, 480, 320, 85, 55),
};

DEFINE_DRM_GEM_DMA_FOPS(pud_drm_fops);

static const struct drm_driver pud_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &pud_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.name = "pud-drm",
	.desc = "pud DRM driver",
	.date = "20250119",
	.major = 1,
	.minor = 0,
};

static int pud_drm_dev_init_with_formats(
	struct pud *pud, const struct drm_simple_display_pipe_funcs *funcs,
	const uint32_t *formats, unsigned int formats_count,
	const struct drm_display_mode *mode, size_t tx_buf_size)
{
	static const uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR,
					      DRM_FORMAT_MOD_INVALID };
	struct drm_device *drm = &pud->drm;
	struct page **pages;
	unsigned int i, num_pages;
	void *ptr;
	int rc;

	pr_info("%s\n", __func__);

	rc = drm_mode_config_init(drm);
	if (rc) {
		pr_err("failed to init mode config\n");
		return rc;
	}

	pud->tx_buf = devm_kmalloc(drm->dev, tx_buf_size, GFP_KERNEL);
	if (!pud->tx_buf)
		return -ENOMEM;

	// pud->encoder_buf = devm_kmalloc(drm->dev, tx_buf_size, GFP_KERNEL);
	// if (!pud->encoder_buf)
	//     return -ENOMEM;

	pud->encoder_buf = vmalloc_32(tx_buf_size);
	if (!pud->encoder_buf)
		return -ENOMEM;

	num_pages = DIV_ROUND_UP(tx_buf_size, PAGE_SIZE);
	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	DRM_DEBUG_KMS("%s, %d\n", __func__, num_pages);

	for (i = 0, ptr = pud->encoder_buf; i < num_pages;
	     i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);

	rc = sg_alloc_table_from_pages(&pud->bulk_sgt, pages, num_pages, 0,
				       tx_buf_size, GFP_KERNEL);

	kfree(pages);

	/* TODO: use debugfs to set params */
	// pud->encoder_quality = JPEGE_Q_BEST;
	// pud->encoder_quality = JPEGE_Q_HIGH;
	// pud->encoder_quality = JPEGE_Q_MED;
	pud->encoder_quality = JPEGE_Q_LOW;

	drm_mode_copy(&pud->mode, mode);
	pr_info("mode: %ux%u\n", pud->mode.hdisplay, pud->mode.vdisplay);

	drm_connector_helper_add(&pud->connector, &pud_connector_hfuncs);
	rc = drm_connector_init(drm, &pud->connector, &pud_connector_funcs,
				DRM_MODE_CONNECTOR_USB);
	if (rc) {
		pr_err("failed to init connector\n");
		return rc;
	}

	rc = drm_simple_display_pipe_init(drm, &pud->pipe, funcs, formats,
					  formats_count, modifiers,
					  &pud->connector);
	if (rc) {
		pr_err("failed to init pipe\n");
		return rc;
	}

	drm_plane_enable_fb_damage_clips(&pud->pipe.plane);

	drm->mode_config.funcs = &pud_drm_mode_config_funcs;
	drm->mode_config.min_width = pud->mode.hdisplay;
	drm->mode_config.max_width = pud->mode.hdisplay;
	drm->mode_config.min_height = pud->mode.vdisplay;
	drm->mode_config.max_height = pud->mode.vdisplay;
	pud->pixel_format = formats[0];

	DRM_DEBUG_KMS("mode: %ux%u", pud->mode.hdisplay, pud->mode.vdisplay);

	return 0;
}

static int pud_drm_dev_init(struct pud *pud,
			    const struct drm_simple_display_pipe_funcs *funcs,
			    const struct drm_display_mode *mode)
{
	ssize_t bufsize = mode->vdisplay * mode->hdisplay * sizeof(u16);

	pud->drm.mode_config.preferred_depth = 16;

	pr_info("%s\n", __func__);

	return pud_drm_dev_init_with_formats(pud, funcs, pud_drm_formats,
					     ARRAY_SIZE(pud_drm_formats), mode,
					     bufsize);
}

struct drm_device *pud_drm_alloc(struct device *dev)
{
	struct pud *pud;
	struct drm_device *drm;
	int rc;

	pr_info("%s\n", __func__);
	pud = devm_drm_dev_alloc(dev, &pud_drm_driver, struct pud, drm);
	if (IS_ERR(pud)) {
		pr_err("failed to allocate drm\n");
		return ERR_PTR(-ENOMEM);
	}
	drm = &pud->drm;

	pud->dma_mask = DMA_BIT_MASK(32);
	dev->dma_mask = &pud->dma_mask;
	dev->coherent_dma_mask = pud->dma_mask;

	rc = pud_drm_dev_init(pud, &pud_display_pipe_funcs, &pud_disp_mode);
	if (rc) {
		pr_err("failed to init drm dev\n");
		return ERR_PTR(-ENOMEM);
	}

	return drm;
}

void pud_drm_release(struct drm_device *drm)
{
	pr_info("%s\n", __func__);
}

int pud_drm_register(struct drm_device *drm)
{
	int rc;

	pr_info("%s\n", __func__);

	drm_mode_config_reset(drm);

	rc = drm_dev_register(drm, 0);
	if (rc) {
		pr_err("failed to register drm dev\n");
		return -1;
	};

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}

void pud_drm_unregister(struct drm_device *drm)
{
	struct pud *pud = drm_to_pud(drm);

	pr_info("%s\n", __func__);
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	sg_free_table(&pud->bulk_sgt);
	vfree(pud->encoder_buf);
	pud->encoder_buf = NULL;
}
