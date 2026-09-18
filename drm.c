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
#include <linux/vmalloc.h>
#include <video/mipi_display.h>

#include "pud.h"
#include "encoder.h"

#define DRV_NAME "pud-drm"

static inline struct pud *drm_to_pud(struct drm_device *drm)
{
    return container_of(drm, struct pud, drm);
}

static enum drm_mode_status pud_drm_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
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

static int pud_buf_copy(void *dst, struct iosys_map *src, struct drm_framebuffer *fb,
                        struct drm_rect *clip, bool swap)
{
    struct pud *pud = drm_to_pud(fb->dev);
    // struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
    struct iosys_map dst_map = IOSYS_MAP_INIT_VADDR(dst);
    int ret;

    ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
    if (ret)
        return ret;

    switch (fb->format->format) {
    case DRM_FORMAT_RGB565:
        /* Already in the panel's native format: copy the block straight
         * across. Compositors (and the 16bpp fbdev emulation) hand us RGB565
         * buffers, so this path must not error out.
         */
        drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
        break;
    // case DRM_FORMAT_RGB888:
    //     drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
    //     break;
    case DRM_FORMAT_XRGB8888:
        switch (pud->pixel_format) {
        case DRM_FORMAT_RGB565:
            drm_fb_xrgb8888_to_rgb565(&dst_map, NULL, src, fb, clip, swap);
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

/*
 * Frame encoding / refresh policy.
 *
 * Frames are encoded with RGB565 QOI (lossless, very fast to encode and
 * decode) instead of JPEG. The firmware's QOI decoder handles arbitrary
 * partial windows correctly (it decodes pixel by pixel, unlike the JPEGDEC
 * path whose MCU/crop logic mis-clips a sub-image decoded at x != 0 and could
 * wedge the display under load). So the damage rectangle is sent as-is:
 * small, lossless and fast.
 *
 * A rectangle whose QOI stream might not fit the USB transfer limit is split
 * into horizontal bands and each band is encoded and flushed on its own. The
 * QOI worst case is 3 bytes per pixel, so a band of at most
 * pud->max_band_pixels pixels always fits.  That budget comes from the device
 * (PUD_CMD_GET_CAPS, see pud_read_caps()) because it depends on how much RAM
 * the firmware has: an RP2040 accepts half-size transfers, an RP2350 the full
 * 64 KB.  It falls back to PUD_DEFAULT_BAND_PIXELS when the device does not
 * report anything.
 */
static void pud_fb_dirty(struct iosys_map *src, struct drm_framebuffer *fb,
                         struct drm_rect *rect)
{
    struct pud *pud = drm_to_pud(fb->dev);
    struct drm_rect band;
    unsigned int rows, y;
    bool swap = false;
    ssize_t qoi_length;
    int ret;

    if (rect->x2 <= rect->x1 || rect->y2 <= rect->y1)
        return;

    rows = pud->max_band_pixels / (rect->x2 - rect->x1);
    if (rows < 1)
        rows = 1;

    for (y = rect->y1; y < rect->y2; y += rows) {
        unsigned int width, height;

        band.x1 = rect->x1;
        band.x2 = rect->x2;
        band.y1 = y;
        band.y2 = min(y + rows, (unsigned int)rect->y2);

        width = band.x2 - band.x1;
        height = band.y2 - band.y1;

        ret = pud_buf_copy(pud->tx_buf, src, fb, &band, swap);
        if (ret)
            return;

        qoi_length = 0;
        ret = qoi_encode_rgb565((uint8_t *)pud->tx_buf, width, height,
                                pud->encoder_buf_size, pud->encoder_buf,
                                &qoi_length);
        if (ret) {
            drm_err_once(fb->dev, "QOI encode failed: %d\n", ret);
            pud->needs_full_refresh = true;
            return;
        }

        ret = pud_flush(pud, band.x1, band.y1, band.x2 - 1, band.y2 - 1,
                        pud->encoder_buf, qoi_length);
        if (ret < 0) {
            /* The frame did not reach the display: those pixels would stay
             * stale until the application happens to redraw them. Ask for a
             * full repaint on the next update instead. */
            drm_dbg(fb->dev, "flush failed: %d\n", ret);
            pud->needs_full_refresh = true;
            return;
        }
    }
}

static void pud_drm_pipe_update(struct drm_simple_display_pipe *pipe,
                                struct drm_plane_state *old_state)
{
    struct drm_plane_state *state = pipe->plane.state;
    struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(state);
    struct drm_framebuffer *fb = state->fb;
    struct drm_rect rect;
    struct pud *pud;
    int idx;

    if (!pipe->crtc.state->active)
        return;

    if (WARN_ON(!fb))
        return;

    pud = drm_to_pud(fb->dev);

    if (!drm_dev_enter(fb->dev, &idx))
        return;

    if (drm_atomic_helper_damage_merged(old_state, state, &rect) ||
        pud->needs_full_refresh) {
        if (pud->needs_full_refresh) {
            /* A previous flush was lost; repaint everything so no stale
             * pixels survive. */
            pud->needs_full_refresh = false;
            drm_rect_init(&rect, 0, 0, fb->width, fb->height);
        }
        drm_dbg(fb->dev, "Flushing [FB:%d] " DRM_RECT_FMT "\n", fb->base.id, DRM_RECT_ARG(&rect));
        pud_fb_dirty(&shadow_plane_state->data[0], fb, &rect);
    }

    drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs pud_display_pipe_funcs = {
    .mode_valid = pud_drm_pipe_mode_valid,
    .enable = pud_drm_pipe_enable,
    .disable = pud_drm_pipe_disable,
    .update = pud_drm_pipe_update,
    DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
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
    DRM_FORMAT_RGB565,      /* device pixel format */
    DRM_FORMAT_XRGB8888,    /* DRM driver framebuffer format */
};

/*
 * The mode is built at probe time from the panel parameters the device reports
 * (PUD_CMD_GET_CAPS), so it is no longer a compile-time constant: the same
 * driver follows whatever panel the firmware drives.
 */
static void pud_mode_init(struct drm_display_mode *mode,
                          const struct pud_display *disp)
{
    *mode = (struct drm_display_mode){
        DRM_MODE_INIT(60, disp->xres, disp->yres,
                      disp->width_mm ?: 85, disp->height_mm ?: 55)
    };
}

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

static int pud_drm_dev_init_with_formats(struct pud *pud,
                const struct drm_simple_display_pipe_funcs *funcs,
                const uint32_t *formats, unsigned int formats_count,
                const struct drm_display_mode *mode, size_t tx_buf_size)
{
    static const uint64_t modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		DRM_FORMAT_MOD_INVALID
	};
    struct drm_device *drm = &pud->drm;
    struct page **pages;
    unsigned int i, num_pages;
    size_t enc_size;
    void *ptr;
    int rc;

    pr_info("%s\n", __func__);

    rc = drm_mode_config_init(drm);
    if (rc) {
        pr_err("failed to init mode config\n");
        return rc;
    }

    pud->tx_buf = vmalloc(tx_buf_size);
    if (!pud->tx_buf)
        return -ENOMEM;

    /* The encoder output buffer is DMA-allocated so its pages are within the
     * USB controller's DMA range: the bulk SG transfer can map them directly
     * (no swiotlb bounce). dma_alloc_coherent() returns a vmap address for
     * the CPU encoder to write to; the underlying pages are obtained with
     * vmalloc_to_page() for the SG list.
     *
     * It must hold the worst-case QOI stream for a full frame
     * (8 + pixels*3 + 8), since the QOI encoder rejects undersized buffers.
     */
    enc_size = rgb565_qoi_max_compressed_size((size_t)mode->hdisplay *
                                              mode->vdisplay);
    if (!enc_size)
        return -EINVAL;
    pud->encoder_buf = dma_alloc_coherent(drm->dev, enc_size,
                                          &pud->encoder_dma, GFP_KERNEL);
    if (!pud->encoder_buf)
        return -ENOMEM;
    pud->encoder_buf_size = enc_size;

    num_pages = DIV_ROUND_UP(enc_size, PAGE_SIZE);
    pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
    if (!pages)
        return -ENOMEM;

    for (i = 0, ptr = pud->encoder_buf; i < num_pages; i++, ptr += PAGE_SIZE)
        pages[i] = vmalloc_to_page(ptr);

    rc = sg_alloc_table_from_pages(&pud->bulk_sgt, pages, num_pages,
                            0, enc_size, GFP_KERNEL);
    kfree(pages);
    if (rc)
        return rc;

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

    rc = drm_simple_display_pipe_init(drm, &pud->pipe, funcs, formats, formats_count, modifiers, &pud->connector);
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
                        ARRAY_SIZE(pud_drm_formats), mode, bufsize);
}

static void pud_drm_release_buffers(struct pud *pud)
{
    sg_free_table(&pud->bulk_sgt);
    if (pud->encoder_buf) {
        dma_free_coherent(pud->drm.dev, pud->encoder_buf_size,
                          pud->encoder_buf, pud->encoder_dma);
        pud->encoder_buf = NULL;
        pud->encoder_buf_size = 0;
    }
    if (pud->tx_buf) {
        vfree(pud->tx_buf);
        pud->tx_buf = NULL;
    }
}

struct drm_device *pud_drm_alloc(struct device *dev,
                                 const struct pud_caps *caps, int caps_len)
{
    struct drm_display_mode mode;
    struct pud *pud;
    struct drm_device *drm;
    int rc;

    pr_info("%s\n", __func__);
    pud = devm_drm_dev_alloc(dev, &pud_drm_driver,
                    struct pud, drm);
    if (IS_ERR(pud)) {
        pr_err("failed to allocate drm\n");
        return ERR_PTR(-ENOMEM);
    }
    drm = &pud->drm;

    /* Panel parameters before anything is sized from them: the mode below and,
     * inside pud_drm_dev_init(), the encoder buffer and the plane limits. */
    pud->display = &pud->display_data;
    pud->display_data = pud_default_display;
    if (caps_len >= (int)sizeof(*caps))
        pud_caps_to_display(&pud->display_data, caps);
    pud_mode_init(&mode, &pud->display_data);

    /* The streaming dma_mask is 64-bit so dma-buf buffers imported from the
     * compositor (often above 4GB) map directly without swiotlb bounce, while
     * the coherent mask stays 32-bit so our own encoder buffer (sent over the
     * USB bulk endpoint) stays in a DMA range any USB controller can reach.
     */
    pud->dma_mask = DMA_BIT_MASK(64);
    dev->dma_mask = &pud->dma_mask;
    dev->coherent_dma_mask = DMA_BIT_MASK(32);

    rc = pud_drm_dev_init(pud, &pud_display_pipe_funcs, &mode);
    if (rc) {
        pr_err("failed to init drm dev\n");
        pud_drm_release_buffers(pud);
        return ERR_PTR(-ENOMEM);
    }

    return drm;
}

void pud_drm_release(struct drm_device *drm)
{
    struct pud *pud = drm_to_pud(drm);

    pr_info("%s\n", __func__);
    pud_drm_release_buffers(pud);
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

    pud_drm_release_buffers(pud);
}
