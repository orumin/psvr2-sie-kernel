/*
 * Copyright (c) 2015 MediaTek Inc.
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
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_drm_crtc.h"
#include "mtk_drm_ddp_comp.h"

#ifdef MTK_DRM_VIRTUAL_SUPPORT
#define cmdq_pkt_write(arg...)  {}
#define cmdq_pkt_write_mask(arg...) {}
#endif

#define DISP_REG_OVL_INTEN			0x0004
#define OVL_FME_CPL_INT					BIT(1)
#define DISP_REG_OVL_INTSTA			0x0008
#define DISP_REG_OVL_EN				0x000c
#define DISP_REG_OVL_RST			0x0014
#define DISP_REG_OVL_ROI_SIZE			0x0020
#define DISP_REG_OVL_ROI_BGCLR			0x0028
#define DISP_REG_OVL_SRC_CON			0x002c
#define DISP_REG_OVL_CON(n)			(0x0030 + 0x20 * (n))
#define DISP_REG_OVL_SRC_SIZE(n)		(0x0038 + 0x20 * (n))
#define DISP_REG_OVL_OFFSET(n)			(0x003c + 0x20 * (n))
#define DISP_REG_OVL_PITCH(n)			(0x0044 + 0x20 * (n))
#define DISP_REG_OVL_RDMA_CTRL(n)		(0x00c0 + 0x20 * (n))
#define DISP_REG_OVL_RDMA_GMC(n)		(0x00c8 + 0x20 * (n))
#define DISP_REG_OVL_ADDR(n)			(0x0f40 + 0x20 * (n))

#define	OVL_RDMA_MEM_GMC	0x40402020

#define OVL_CON_BYTE_SWAP	BIT(24)
#define OVL_CON_CLRFMT_RGB565	(0 << 12)
#define OVL_CON_CLRFMT_RGB888	(1 << 12)
#define OVL_CON_CLRFMT_RGBA8888	(2 << 12)
#define OVL_CON_CLRFMT_ARGB8888	(3 << 12)
#define	OVL_CON_AEN		BIT(8)
#define	OVL_CON_ALPHA		0xff

/**
 * struct mtk_disp_ovl - DISP_OVL driver structure
 * @ddp_comp - structure containing type enum and hardware resources
 * @crtc - associated crtc to report vblank events to
 */
struct mtk_disp_ovl {
	struct mtk_ddp_comp		ddp_comp;
	struct drm_crtc			*crtc;
};

static irqreturn_t mtk_disp_ovl_irq_handler(int irq, void *dev_id)
{
	struct mtk_disp_ovl *priv = dev_id;
	struct mtk_ddp_comp *ovl = &priv->ddp_comp;

	/* Clear frame completion interrupt */
	writel(0x0, ovl->regs + DISP_REG_OVL_INTSTA);

	if (!priv->crtc)
		return IRQ_NONE;

	mtk_crtc_vblank_irq(priv->crtc);

	return IRQ_HANDLED;
}

static void mtk_ovl_enable_vblank(struct mtk_ddp_comp *comp,
				  struct drm_crtc *crtc,
				  struct cmdq_pkt *handle)
{
	struct mtk_disp_ovl *priv = container_of(comp, struct mtk_disp_ovl,
						 ddp_comp);

	priv->crtc = crtc;
	writel_relaxed(OVL_FME_CPL_INT, comp->regs + DISP_REG_OVL_INTEN);
}

static void mtk_ovl_disable_vblank(struct mtk_ddp_comp *comp,
				   struct cmdq_pkt *handle)
{
	struct mtk_disp_ovl *priv = container_of(comp, struct mtk_disp_ovl,
						 ddp_comp);

	priv->crtc = NULL;
	writel_relaxed(0x0, comp->regs + DISP_REG_OVL_INTEN);
}

static void mtk_ovl_start(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle)
{
	cmdq_pkt_write(handle, 0x1, comp->cmdq_base, DISP_REG_OVL_EN);
}

static void mtk_ovl_stop(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle)
{
	cmdq_pkt_write(handle, 0x0, comp->cmdq_base, DISP_REG_OVL_EN);
}

static void mtk_ovl_config(struct mtk_ddp_comp *comp, unsigned int w,
			   unsigned int h, unsigned int vrefresh,
			   unsigned int bpc, struct cmdq_pkt *handle)
{
	cmdq_pkt_write(handle, h << 16 | w, comp->cmdq_base,
		       DISP_REG_OVL_ROI_SIZE);
	cmdq_pkt_write(handle, 0x0, comp->cmdq_base, DISP_REG_OVL_ROI_BGCLR);
	cmdq_pkt_write(handle, 0x1, comp->cmdq_base, DISP_REG_OVL_RST);
	cmdq_pkt_write(handle, 0x0, comp->cmdq_base, DISP_REG_OVL_RST);
}

static void mtk_ovl_layer_on(struct mtk_ddp_comp *comp, unsigned int idx,
			     struct cmdq_pkt *handle)
{
	cmdq_pkt_write(handle, 0x1, comp->cmdq_base,
		       DISP_REG_OVL_RDMA_CTRL(idx));
	cmdq_pkt_write(handle, OVL_RDMA_MEM_GMC,
		       comp->cmdq_base, DISP_REG_OVL_RDMA_GMC(idx));
	cmdq_pkt_write_mask(handle, BIT(idx), comp->cmdq_base,
			    DISP_REG_OVL_SRC_CON, BIT(idx));
}

static void mtk_ovl_layer_off(struct mtk_ddp_comp *comp, unsigned int idx,
			      struct cmdq_pkt *handle)
{
	cmdq_pkt_write_mask(handle, 0, comp->cmdq_base,
			    DISP_REG_OVL_SRC_CON, BIT(idx));
	cmdq_pkt_write(handle, 0, comp->cmdq_base,
		       DISP_REG_OVL_RDMA_CTRL(idx));
}

#ifndef MTK_DRM_VIRTUAL_SUPPORT
static unsigned int ovl_fmt_convert(unsigned int fmt)
{
	switch (fmt) {
	default:
	case DRM_FORMAT_RGB565:
		return OVL_CON_CLRFMT_RGB565;
	case DRM_FORMAT_BGR565:
		return OVL_CON_CLRFMT_RGB565 | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGB888:
		return OVL_CON_CLRFMT_RGB888;
	case DRM_FORMAT_BGR888:
		return OVL_CON_CLRFMT_RGB888 | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		return OVL_CON_CLRFMT_ARGB8888;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return OVL_CON_CLRFMT_ARGB8888 | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return OVL_CON_CLRFMT_RGBA8888;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return OVL_CON_CLRFMT_RGBA8888 | OVL_CON_BYTE_SWAP;
	}
}
#endif

static void mtk_ovl_layer_config(struct mtk_ddp_comp *comp, unsigned int idx,
				 struct mtk_plane_pending_state *pending,
				 struct cmdq_pkt *handle)
{
#ifndef MTK_DRM_VIRTUAL_SUPPORT
	unsigned int addr = pending->addr;
	unsigned int pitch = pending->pitch & 0xffff;
	unsigned int fmt = pending->format;
	unsigned int offset = (pending->y << 16) | pending->x;
	unsigned int src_size = (pending->height << 16) | pending->width;
	unsigned int con;

	if (!pending->enable)
		mtk_ovl_layer_off(comp, idx, handle);

	con = ovl_fmt_convert(fmt);
	if (idx != 0)
		con |= OVL_CON_AEN | OVL_CON_ALPHA;

	cmdq_pkt_write(handle, con, comp->cmdq_base,
		       DISP_REG_OVL_CON(idx));
	cmdq_pkt_write(handle, pitch, comp->cmdq_base,
		       DISP_REG_OVL_PITCH(idx));
	cmdq_pkt_write(handle, src_size, comp->cmdq_base,
		       DISP_REG_OVL_SRC_SIZE(idx));
	cmdq_pkt_write(handle, offset, comp->cmdq_base,
		       DISP_REG_OVL_OFFSET(idx));
	cmdq_pkt_write(handle, addr, comp->cmdq_base,
		       DISP_REG_OVL_ADDR(idx));

	if (pending->enable)
		mtk_ovl_layer_on(comp, idx, handle);
#endif
}

static const struct mtk_ddp_comp_funcs mtk_disp_ovl_funcs = {
	.config = mtk_ovl_config,
	.start = mtk_ovl_start,
	.stop = mtk_ovl_stop,
	.enable_vblank = mtk_ovl_enable_vblank,
	.disable_vblank = mtk_ovl_disable_vblank,
	.layer_on = mtk_ovl_layer_on,
	.layer_off = mtk_ovl_layer_off,
	.layer_config = mtk_ovl_layer_config,
};

static int mtk_disp_ovl_bind(struct device *dev, struct device *master,
			     void *data)
{
	struct mtk_disp_ovl *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	int ret;

	ret = mtk_ddp_comp_register(drm_dev, &priv->ddp_comp);
	if (ret < 0) {
		dev_err(dev, "Failed to register component %s: %d\n",
			dev->of_node->full_name, ret);
		return ret;
	}

	return 0;
}

static void mtk_disp_ovl_unbind(struct device *dev, struct device *master,
				void *data)
{
	struct mtk_disp_ovl *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;

	mtk_ddp_comp_unregister(drm_dev, &priv->ddp_comp);
}

static const struct component_ops mtk_disp_ovl_component_ops = {
	.bind	= mtk_disp_ovl_bind,
	.unbind = mtk_disp_ovl_unbind,
};

static int mtk_disp_ovl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_ovl *priv;
	int comp_id;
	int irq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, mtk_disp_ovl_irq_handler,
			       IRQF_TRIGGER_NONE, dev_name(dev), priv);
	if (ret < 0) {
		dev_err(dev, "Failed to request irq %d: %d\n", irq, ret);
		return ret;
	}

	comp_id = mtk_ddp_comp_get_id(dev->of_node, MTK_DISP_OVL);
	if (comp_id < 0) {
		dev_err(dev, "Failed to identify by alias: %d\n", comp_id);
		return comp_id;
	}

	ret = mtk_ddp_comp_init(dev, dev->of_node, &priv->ddp_comp, comp_id,
				&mtk_disp_ovl_funcs);
	if (ret) {
		dev_err(dev, "Failed to initialize component: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	ret = component_add(dev, &mtk_disp_ovl_component_ops);
	if (ret)
		dev_err(dev, "Failed to add component: %d\n", ret);

	return ret;
}

static int mtk_disp_ovl_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_ovl_component_ops);

	return 0;
}

static const struct of_device_id mtk_disp_ovl_driver_dt_match[] = {
	{ .compatible = "mediatek,mt8173-disp-ovl", },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_disp_ovl_driver_dt_match);

struct platform_driver mtk_disp_ovl_driver = {
	.probe		= mtk_disp_ovl_probe,
	.remove		= mtk_disp_ovl_remove,
	.driver		= {
		.name	= "mediatek-disp-ovl",
		.owner	= THIS_MODULE,
		.of_match_table = mtk_disp_ovl_driver_dt_match,
	},
};
