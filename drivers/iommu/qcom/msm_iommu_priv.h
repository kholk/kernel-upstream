/*
 * Copyright (C) 2017-2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 *
 * May contain portions of code (c) 2013-2014, The Linux Foundation.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef MSM_IOMMU_PRIV_H
#define MSM_IOMMU_PRIV_H

#include <linux/pm_domain.h>

/* SEC definitions */
#define MAXIMUM_VIRT_SIZE	(300 * SZ_1M)

enum model_id {
	QSMMU_V2 = 1,
	MMU_500,
	MAX_MODEL,
};

/**
 * struct msm_iommu_priv - Container for page table attributes and other
 * private iommu domain information attributes.
 */
struct msm_iommu_priv {
	struct list_head list_attached;
	struct iommu_domain domain;
	const char *client_name;
	struct io_pgtable_cfg pgtbl_cfg;
	struct io_pgtable_ops *pgtbl_ops;
	spinlock_t pgtbl_lock;
	struct mutex init_mutex;
	u32 asid;
};

static inline struct msm_iommu_priv *to_msm_priv(struct iommu_domain *dom)
{
	return container_of(dom, struct msm_iommu_priv, domain);
}

#define MMU_POWER_DOMAINS_CNT	2

/**
 * struct msm_iommu_drvdata - A single IOMMU hardware instance
 * @base:	IOMMU config port base address (VA)
 * @cb_base:	Context bank base address
 * @ncb		The number of contexts on this IOMMU
 * @core:	The bus clock for this IOMMU hardware instance
 * @iface:	The clock for the IOMMU bus interconnect
 * @name:	Human-readable name of this IOMMU device
 * @sec_id:	TZ Secure ID for this IOMMU hardware
 * @dev:	Struct device this hardware instance is tied to
 * @pds:	Power domains for the IOMMU hardware instance
 * @mmu_fmt:	Format of the IOMMU page table for this instance
 * @list:	List head to link all iommus together
 * @ctx_attach_count: Count of how many context are attached.
 * @model:	Model of this IOMMU
 * @iommu:	Core IOMMU device handle
 * @glb_lock:	Locking relative to the entire instance
 *
 * A msm_iommu_drvdata holds the global driver data about a single piece
 * of an IOMMU hardware instance.
 */
struct msm_iommu_drvdata {
	void __iomem *base;
	void __iomem *cb_base;
	int ncb;
	struct clk *core;
	struct clk *iface;
	const char *name;
	int sec_id;
	struct device *dev;
	struct device *pds[MMU_POWER_DOMAINS_CNT];
	enum io_pgtable_fmt mmu_fmt;
	struct list_head list;
	unsigned int ctx_attach_count;
	unsigned int model;
	struct iommu_device iommu;
	struct mutex glb_lock;
};

/**
 * struct msm_iommu_ctx_drvdata - an IOMMU context bank instance
 * @num:		Hardware context number of this context
 * @pdev:		Platform device associated wit this HW instance
 * @attached_elm:	List element for domains to track which devices are
 *			attached to them
 * @attached_domain	Domain currently attached to this context (if any)
 * @ctx_lock		Lock for the specific context bank instance
 * @name		Human-readable name of this context device
 * @secure_context	true if this is a secure context programmed by
			the secure environment (TZ), false otherwise
 * @asid		ASID used with this context.
 * @attach_count	Number of time this context has been attached.
 *
 * A msm_iommu_ctx_drvdata holds the driver data for a single context bank
 * within each IOMMU hardware instance
 */
struct msm_iommu_ctx_drvdata {
	int num;
	struct platform_device *pdev;
	struct list_head attached_elm;
	struct iommu_domain *attached_domain;
	struct mutex ctx_lock;
	const char *name;
	bool secure_context;
	int asid;
	int attach_count;
};

#endif
