/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: CK Hu <ck.hu@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "mtk_drm_crtc.h"
#include "mtk_drm_ddp_comp.h"
#include "mtk_drm_drv.h"
#include "mtk_drm_fb.h"
#include "mtk_drm_gem.h"
#include "mtk_drm_plane.h"

static const u32 formats[] = {
#ifdef MTK_DRM_VIRTUAL_SUPPORT
	DRM_FORMAT_RGB888,
#endif
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
#ifdef MTK_DRM_VIRTUAL_SUPPORT
	DRM_FORMAT_ABGR2101010,
#endif
	DRM_FORMAT_RGB565,
};

static const struct drm_plane_funcs mtk_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int mtk_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_crtc_state *crtc_state;
	struct drm_rect clip = { 0, };

	if (!fb)
		return 0;

	if (!mtk_fb_get_gem_obj(fb)) {
		DRM_DEBUG_KMS("buffer is null\n");
		return -EFAULT;
	}

	if (!state->crtc)
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	clip.x2 = crtc_state->mode.hdisplay;
	clip.y2 = crtc_state->mode.vdisplay;

#ifdef MTK_DRM_VIRTUAL_SUPPORT
	return 0;
#else
	return drm_plane_helper_check_state(state, &clip,
					    DRM_PLANE_HELPER_NO_SCALING,
					    DRM_PLANE_HELPER_NO_SCALING,
					    true, true);
#endif
}

static void mtk_plane_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = plane->state->crtc;
	struct drm_framebuffer *fb = plane->state->fb;
	struct drm_gem_object *gem;
	struct mtk_drm_gem_obj *mtk_gem;
	unsigned int pitch, format;
	dma_addr_t addr;
	struct mtk_plane_pending_state pending;

	if (!crtc || WARN_ON(!fb))
		return;

	gem = mtk_fb_get_gem_obj(fb);
	mtk_gem = to_mtk_gem_obj(gem);
	addr = mtk_gem->dma_addr;
	pitch = fb->pitches[0];
	format = fb->pixel_format;

#ifndef MTK_DRM_VIRTUAL_SUPPORT
	addr += (plane->state->src.x1 >> 16) * drm_format_plane_cpp(format, 0);
	addr += (plane->state->src.y1 >> 16) * pitch;
#endif

	pending.enable = true;
	pending.pitch = pitch;
	pending.format = format;
	pending.addr = addr;

#ifndef MTK_DRM_VIRTUAL_SUPPORT
	pending.x = plane->state->dst.x1;
	pending.y = plane->state->dst.y1;
	pending.width = drm_rect_width(&plane->state->dst);
	pending.height = drm_rect_height(&plane->state->dst);
#endif

	mtk_drm_crtc_plane_update(crtc, plane, &pending);
}

static void mtk_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct mtk_plane_pending_state pending;

	pending.enable = false;

	/* Fetch CRTC from old plane state when disabling. */
	mtk_drm_crtc_plane_update(old_state->crtc, plane, &pending);
}

static const struct drm_plane_helper_funcs mtk_plane_helper_funcs = {
	.atomic_check = mtk_plane_atomic_check,
	.atomic_update = mtk_plane_atomic_update,
	.atomic_disable = mtk_plane_atomic_disable,
};

int mtk_plane_init(struct drm_device *dev, struct drm_plane *plane,
		   unsigned long possible_crtcs, enum drm_plane_type type)
{
	int err;

	err = drm_universal_plane_init(dev, plane, possible_crtcs,
				       &mtk_plane_funcs, formats,
				       ARRAY_SIZE(formats), type);
	if (err) {
		DRM_ERROR("failed to initialize plane\n");
		return err;
	}

	drm_plane_helper_add(plane, &mtk_plane_helper_funcs);

	return 0;
}
