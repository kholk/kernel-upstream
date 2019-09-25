// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM Secure IOMMUv2 and Secure MMU-500 Driver
 * Copyright (C) 2017-2019, AngeloGioacchino Del Regno <kholk11@gmail.com>
 *
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/clk.h>
#include <linux/scatterlist.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>
#include <linux/iopoll.h>
#include <linux/dma-iommu.h>
#include <linux/qcom_scm.h>
#include <linux/amba/bus.h>

#include <linux/io-pgtable.h>

#include "msm_iommu_priv.h"

#include "../arm-smmu.h"

#define QCOM_DUMMY_VAL -1

struct msm_iommu_master {
	struct list_head list;
	unsigned int ctx_num;
	struct device *dev;
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
};

static DEFINE_MUTEX(iommu_list_lock);
static LIST_HEAD(iommu_list);
static LIST_HEAD(iommu_masters);
static struct iommu_ops msm_iommu_ops;

char *power_domain_names[MMU_POWER_DOMAINS_CNT+1] =
	{
		"cx", "mx", NULL
	};

static int __enable_clocks(struct msm_iommu_drvdata *drvdata)
{
	int ret;

	ret = clk_enable(drvdata->iface);
	if (ret)
		return ret;

	ret = clk_enable(drvdata->core);
	if (ret)
		goto err;

	return 0;

err:
	clk_disable(drvdata->iface);
	return ret;
}

static void __disable_clocks(struct msm_iommu_drvdata *drvdata)
{
//	clk_disable(drvdata->core);
//	clk_disable(drvdata->iface);
}

static inline void
iommu_writel(struct msm_iommu_drvdata *drvdata, unsigned reg, u32 val)
{
	writel_relaxed(val, drvdata->base + reg);
}

static inline u32
iommu_readl(struct msm_iommu_drvdata *drvdata, unsigned reg)
{
	return readl_relaxed(drvdata->base + reg);
}

static inline void
iommu_cb_writel(struct msm_iommu_drvdata *drvdata, unsigned reg,
		int ctx, u32 val)
{
	writel_relaxed(val, drvdata->cb_base + reg + (ctx << 12));
}

static inline void
iommu_cb_writeq(struct msm_iommu_drvdata *drvdata, unsigned reg,
		int ctx, u64 val)
{
	writeq_relaxed(val, drvdata->cb_base + reg + (ctx << 12));
}

static inline u32
iommu_cb_readl(struct msm_iommu_drvdata *drvdata, unsigned reg, int ctx)
{
	return readl_relaxed(drvdata->cb_base + reg + (ctx << 12));
}

static inline u64
iommu_cb_readq(struct msm_iommu_drvdata *drvdata, unsigned reg, int ctx)
{
	return readq_relaxed(drvdata->cb_base + reg + (ctx << 12));
}

static void __sync_tlb(struct msm_iommu_drvdata *drvdata, int ctx)
{
	unsigned int val, res;

	iommu_cb_writel(drvdata, ARM_SMMU_CB_TLBSYNC, ctx, QCOM_DUMMY_VAL);
	/* No barrier needed due to read dependency */

	res = readl_poll_timeout_atomic(
			drvdata->cb_base + ARM_SMMU_CB_TLBSTATUS + (ctx << 12),
			 val, (val & sTLBGSTATUS_GSACTIVE) == 0, 0, 1000000);
	if (res)
		dev_warn(drvdata->dev,
			 "Timeout waiting for TLB SYNC on IOMMU context.\n");
}

static void msm_iommu_tlb_inv_context_s1(
		struct msm_iommu_drvdata *iommu_drvdata, int ctx,
		unsigned int asid)
{
	/*
	 * The TLBI write may be relaxed, so ensure that PTEs cleared by the
	 * current CPU are visible beforehand.
	 */
	wmb();
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_S1_TLBIASID, ctx, asid);
	__sync_tlb(iommu_drvdata, ctx);
}

static int __flush_iotlb(struct iommu_domain *domain)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	int ret = 0;

	list_for_each_entry(ctx_drvdata, &priv->list_attached, attached_elm) {
		BUG_ON(!ctx_drvdata->pdev || !ctx_drvdata->pdev->dev.parent);

		iommu_drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);
		BUG_ON(!iommu_drvdata);

		ret = __enable_clocks(iommu_drvdata);
		if (ret)
			goto fail;

		msm_iommu_tlb_inv_context_s1(iommu_drvdata, ctx_drvdata->num,
					     priv->asid);

		__disable_clocks(iommu_drvdata);
	}
fail:
	return ret;
}

static void msm_iommu_tlb_sync(void *cookie)
{
	struct iommu_domain *domain = cookie;
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	int ret = 0;

	list_for_each_entry(ctx_drvdata, &priv->list_attached, attached_elm) {
		iommu_drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);
		BUG_ON(!iommu_drvdata);

		ret = __enable_clocks(iommu_drvdata);
		if (ret)
			goto fail;

		__sync_tlb(iommu_drvdata, ctx_drvdata->num);

		__disable_clocks(iommu_drvdata);
	}
fail:
	if (ret)
		dev_err(iommu_drvdata->dev, "Cannot sync TLB: %d \n", ret);
	return;
}

static void msm_iommu_tlb_flush_all(void *cookie)
{
	struct iommu_domain *domain = cookie;

	__flush_iotlb(domain);
}

static void msm_iommu_tlb_flush_range_nosync(unsigned long iova, size_t size,
					size_t granule, bool leaf, void *cookie)
{
	struct iommu_domain *domain = cookie;
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	struct msm_iommu_drvdata *drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	unsigned long va = iova;
	int reg, ret = 0;

	reg = leaf ? ARM_SMMU_CB_S1_TLBIVAL : ARM_SMMU_CB_S1_TLBIVA;

	list_for_each_entry(ctx_drvdata, &priv->list_attached, attached_elm) {
		BUG_ON(!ctx_drvdata->pdev || !ctx_drvdata->pdev->dev.parent);

		drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);
		BUG_ON(!drvdata);

		ret = __enable_clocks(drvdata);
		if (ret)
			goto fail;

		if (drvdata->mmu_fmt == ARM_64_LPAE_S1) {
			va >>= 12;
			va |= (u64)priv->asid << 48;
		} else {
			va = (va >> 12) << 12;
			va |= priv->asid;
		}

		iommu_cb_writel(drvdata, reg, ctx_drvdata->num, va);

		__disable_clocks(drvdata);
	}
fail:
	if (ret)
		dev_err(drvdata->dev, "Cannot flush TLB: %d \n", ret);
	return;
}

static const struct iommu_gather_ops msm_iommu_gather_ops = {
	.tlb_flush_all	= msm_iommu_tlb_flush_all,
	.tlb_add_flush	= msm_iommu_tlb_flush_range_nosync,
	.tlb_sync	= msm_iommu_tlb_sync,
};

static void __reset_context(struct msm_iommu_drvdata *iommu_drvdata, int ctx)
{
	/* Don't set ACTLR to zero because if context bank is in
	 * bypass mode (say after iommu_detach), still this ACTLR
	 * value matters for micro-TLB caching.
	 */
	if (iommu_drvdata->model != MMU_500)
		iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_ACTLR, ctx, 0);

	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_FAR, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_FSR, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_S1_MAIR1, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_PAR, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_S1_MAIR0, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_SCTLR, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TCR2, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TCR, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TTBR0, ctx, 0);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TTBR1, ctx, 0);

	/* Should we TLBSYNC there instead? */
	mb();
}


static void __program_context(struct msm_iommu_drvdata *iommu_drvdata,
			      struct msm_iommu_ctx_drvdata *ctx_drvdata,
			      struct msm_iommu_priv *priv)
{
	unsigned int ctx = ctx_drvdata->num;
	u32 reg, tcr[2];
	u64 ttbr[2];

	__reset_context(iommu_drvdata, ctx);

	priv->asid = ctx_drvdata->num;

	tcr[0] = priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr;
	tcr[1] = priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr >> 32;
	tcr[1] |= FIELD_PREP(TCR2_SEP, TCR2_SEP_UPSTREAM);

	if (iommu_drvdata->mmu_fmt == ARM_64_LPAE_S1) {
		/* If this fails, we will surely end up in a DISASTER */
		if (qcom_scm_iommu_set_pt_format(iommu_drvdata->sec_id,
						 ctx_drvdata->num, 1)) {
			dev_warn(iommu_drvdata->dev,
				 "FATAL: Cannot set AArch64 pt format\n");
			BUG();
		}

		tcr[1] |= TCR2_AS;
	}

	ttbr[0] = priv->pgtbl_cfg.arm_lpae_s1_cfg.ttbr[0];
	ttbr[0] |= FIELD_PREP(TTBRn_ASID, priv->asid);
	ttbr[1] = priv->pgtbl_cfg.arm_lpae_s1_cfg.ttbr[1];
	ttbr[1] |= FIELD_PREP(TTBRn_ASID, priv->asid);

	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TCR2, ctx, tcr[1]);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TCR, ctx, tcr[0]);

	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TTBR0, ctx, ttbr[0]);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_TTBR1, ctx, ttbr[1]);

	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_S1_MAIR0, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.mair[0]);
	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_S1_MAIR1, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.mair[1]);

	/* Ensure that ASID assignment has completed before we use
	 * ASID for TLB invalidation. Here, mb() is required because
	 * both these registers are separated by more than 1KB. */
	mb();

	reg = SCTLR_CFIE | SCTLR_CFRE | SCTLR_AFE | SCTLR_TRE;
	reg |= SCTLR_S1_ASIDPNE | SCTLR_HUPCF | SCTLR_M;

	iommu_cb_writel(iommu_drvdata, ARM_SMMU_CB_SCTLR, ctx, reg);
}

static struct msm_iommu_master *msm_iommu_find_master(struct device *dev)
{
	struct msm_iommu_master *master;
	bool found = false;

	list_for_each_entry(master, &iommu_masters, list) {
		if (master && master->dev == dev) {
			found = true;
			break;
		}
	}

	if (found) {
		dev_dbg(dev, "found master %s with ctx:%d\n",
			dev_name(master->dev),
			master->ctx_num);
		return master;
	}

	return ERR_PTR(-ENODEV);
}

static struct iommu_domain *msm_iommu_domain_alloc(unsigned type)
{
	struct msm_iommu_priv *priv;
	int ret;

	if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;


	if (type == IOMMU_DOMAIN_DMA) {
		ret = iommu_get_dma_cookie(&priv->domain);
		if (ret)
			goto err;
	}

	INIT_LIST_HEAD(&priv->list_attached);
	mutex_init(&priv->init_mutex);
	spin_lock_init(&priv->pgtbl_lock);

	return &priv->domain;

err:
	kfree(priv);
	return NULL;
}

static void msm_iommu_domain_free(struct iommu_domain *domain)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);

	if (!priv) {
		return;
	}

	iommu_put_dma_cookie(domain);
	free_io_pgtable_ops(priv->pgtbl_ops);

	kfree(priv);
}

static int msm_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct msm_iommu_priv *priv;
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	struct msm_iommu_ctx_drvdata *tmp_drvdata;
	struct msm_iommu_master *master;
	struct io_pgtable_ops *pgtbl_ops;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	unsigned long ias, oas, flags;
	int ret = 0;
	int is_secure;
	bool secure_ctx;

	if (!fwspec->iommu_priv)
		return -ENODEV;

	priv = to_msm_priv(domain);
	if (!priv || !dev) {
		return -EINVAL;
	}

	mutex_lock(&priv->init_mutex);

	master = fwspec->iommu_priv;

	iommu_drvdata = master->iommu_drvdata;
	ctx_drvdata = master->ctx_drvdata;

	if (!iommu_drvdata || !ctx_drvdata) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!(priv->client_name))
		priv->client_name = dev_name(dev);

	++ctx_drvdata->attach_count;

	if (ctx_drvdata->attach_count > 1)
		goto unlock;

	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	if (!list_empty(&ctx_drvdata->attached_elm)) {
		ret = -EBUSY;
		spin_unlock_irqrestore(&priv->pgtbl_lock, flags);
		goto unlock;
	}

	list_for_each_entry(tmp_drvdata, &priv->list_attached, attached_elm)
		if (tmp_drvdata == ctx_drvdata) {
			ret = -EBUSY;
			spin_unlock_irqrestore(&priv->pgtbl_lock, flags);
			goto unlock;
		}

	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	is_secure = iommu_drvdata->sec_id != -1;

	if (iommu_drvdata->mmu_fmt == ARM_64_LPAE_S1) {
		/* 64-bits addressing: 48-bits IPA and VA */
		ias = oas = 48;
	} else {
		/* 32-bits LPAE addressing: 32-bits VA, 40-bits IPA */
		ias = 32;
		oas = 40;
	}

	ret = __enable_clocks(iommu_drvdata);
	if (ret)
		goto unlock;

	/* We can only do this once */
	if (!iommu_drvdata->ctx_attach_count) {
		ret = qcom_scm_restore_sec_cfg(iommu_drvdata->sec_id,
					       ctx_drvdata->num);
		if (ret)
			goto unlock;
	}

	/* Make sure the domain is initialized */
	priv->pgtbl_cfg = (struct io_pgtable_cfg) {
		.pgsize_bitmap	= msm_iommu_ops.pgsize_bitmap,
		.ias		= ias,
		.oas		= oas,
		.tlb		= &msm_iommu_gather_ops,
		.iommu_dev	= &ctx_drvdata->pdev->dev,
	};
	domain->geometry.aperture_start = SZ_16M;
	domain->geometry.aperture_end = (1ULL << priv->pgtbl_cfg.ias) - 1;
	domain->geometry.force_aperture = true;

	pgtbl_ops = alloc_io_pgtable_ops(iommu_drvdata->mmu_fmt,
			&priv->pgtbl_cfg, domain);
	if (!pgtbl_ops) {
		dev_err(dev, "failed to allocate pagetable ops\n");
		ret = -ENOMEM;
		goto unlock;
	}

	domain->pgsize_bitmap = priv->pgtbl_cfg.pgsize_bitmap;

	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	priv->pgtbl_ops = pgtbl_ops;
	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	secure_ctx = !!(ctx_drvdata->secure_context > 0);
	if (secure_ctx) {
		dev_dbg(dev, "Detected secure context.\n");
		goto add_domain;
	}

	__program_context(iommu_drvdata, ctx_drvdata, priv);

	/* Ensure TLB is clear */
	if (iommu_drvdata->model != MMU_500)
		msm_iommu_tlb_inv_context_s1(iommu_drvdata, ctx_drvdata->num,
					     ctx_drvdata->asid);

	__disable_clocks(iommu_drvdata);

add_domain:
	if (secure_ctx)
		dev_err(dev, "Attaching secure domain %s (%d)\n",
			ctx_drvdata->name, ctx_drvdata->num);
	else
		dev_err(dev, "Attaching unsecured domain %s (%d)\n",
			ctx_drvdata->name, ctx_drvdata->num);

	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	list_add(&(ctx_drvdata->attached_elm), &priv->list_attached);
	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	ctx_drvdata->attached_domain = domain;
	++iommu_drvdata->ctx_attach_count;

unlock:
	mutex_unlock(&priv->init_mutex);

	return ret;
}

static void msm_iommu_detach_dev(struct iommu_domain *domain,
				 struct device *dev)
{
	struct msm_iommu_priv *priv;
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	struct msm_iommu_master *master;
	int ret;
	unsigned long flags;

	if (!dev)
		return;

	priv = to_msm_priv(domain);
	if (!priv)
		return;

	mutex_lock(&priv->init_mutex);

	master = msm_iommu_find_master(dev);
	if (IS_ERR(master)) {
		ret = PTR_ERR(master);
		goto unlock;
	}

	iommu_drvdata = master->iommu_drvdata;
	ctx_drvdata = master->ctx_drvdata;

	if (!iommu_drvdata || !ctx_drvdata)
		goto unlock;

	if (!ctx_drvdata->attached_domain)
		goto unlock;

	--ctx_drvdata->attach_count;
	BUG_ON(ctx_drvdata->attach_count < 0);

	if (ctx_drvdata->attach_count > 0)
		goto unlock;

	ret = __enable_clocks(iommu_drvdata);
	if (ret)
		goto unlock;

	if (iommu_drvdata->model == MMU_500)
		msm_iommu_tlb_inv_context_s1(iommu_drvdata, ctx_drvdata->num,
					     ctx_drvdata->asid);

	ctx_drvdata->asid = -1;

	__reset_context(iommu_drvdata, ctx_drvdata->num);

	__disable_clocks(iommu_drvdata);


	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	list_del_init(&ctx_drvdata->attached_elm);
	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	ctx_drvdata->attached_domain = NULL;
	BUG_ON(iommu_drvdata->ctx_attach_count == 0);
	--iommu_drvdata->ctx_attach_count;
unlock:
	mutex_unlock(&priv->init_mutex);
}

static int msm_iommu_map(struct iommu_domain *domain, unsigned long va,
			 phys_addr_t pa, size_t len, int prot)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	int ret = 0;
	unsigned long flags;

	if (!priv)
		return -EINVAL;

	if (!priv->pgtbl_ops)
		return -ENODEV;

	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	ret = priv->pgtbl_ops->map(priv->pgtbl_ops, va, pa, len, prot);
	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	return ret;
}

static size_t msm_iommu_unmap(struct iommu_domain *domain, unsigned long va,
			      size_t len)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	int ret = -ENODEV;
	unsigned long flags;

	priv = to_msm_priv(domain);
	if (!priv)
		return -EINVAL;

	if (!priv->pgtbl_ops)
		return 0;

	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	ret = priv->pgtbl_ops->unmap(priv->pgtbl_ops, va, len);
	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	return ret;
}

static void msm_iommu_iotlb_sync(struct iommu_domain *domain)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	struct io_pgtable *pgtable = container_of(priv->pgtbl_ops,
						  struct io_pgtable, ops);
	if (!priv->pgtbl_ops)
		return;

	msm_iommu_tlb_sync(pgtable->cookie);
}

static phys_addr_t msm_iommu_iova_to_phys(struct iommu_domain *domain,
					  dma_addr_t va)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);
	phys_addr_t ret = 0;
	unsigned long flags;

	/*
	 * NOTE: The iova_to_phys for secure mapping ONLY is
	 *       NEVER supported. Though, this should not give
	 *       us any problem here, since we always support
	 *       also the insecure pagetable mapping. Always.
	 */
	if (!priv)
		return 0;

	spin_lock_irqsave(&priv->pgtbl_lock, flags);
	ret = priv->pgtbl_ops->iova_to_phys(priv->pgtbl_ops, va);
	spin_unlock_irqrestore(&priv->pgtbl_lock, flags);

	return ret;
}

static int msm_iommu_add_device(struct device *dev)
{
	struct iommu_group *group;
	struct msm_iommu_drvdata *drvdata;
	struct msm_iommu_master *master = NULL;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct device_link *link;

	if (dev->archdata.iommu == NULL)
		return -ENODEV;

	master = dev->archdata.iommu;
	drvdata = master->iommu_drvdata;
	fwspec->iommu_priv = master;

	/*
	 * Establish the link between iommu and master, so that the
	 * iommu gets runtime enabled/disabled as per the master's
	 * needs.
	 */
	link = device_link_add(dev, drvdata->dev, DL_FLAG_PM_RUNTIME);
	if (!link) {
		dev_err(master->dev,
			"Unable to create device link between %s and %s\n",
			dev_name(master->dev), dev_name(dev));
		return -ENODEV;
	}

	group = iommu_group_get_for_dev(dev);
	if (IS_ERR(group))
		return PTR_ERR(group);

	iommu_group_put(group);
	iommu_device_link(&drvdata->iommu, dev);

	return 0;
}

static void msm_iommu_remove_device(struct device *dev)
{
	struct msm_iommu_master *master = dev->archdata.iommu;

	iommu_group_remove_device(dev);

	if (master)
		iommu_device_unlink(&master->iommu_drvdata->iommu, dev);
}

static void msm_iommu_release_group_iommudata(void *data)
{
	/* As of now, we don't need to do anything here. */
	return;
}

static struct iommu_group *msm_iommu_device_group(struct device *dev)
{
	struct msm_iommu_master *master;
	struct iommu_group *group;

	group = generic_device_group(dev);
	if (IS_ERR(group))
		return group;

	master = msm_iommu_find_master(dev);
	if (IS_ERR(master)) {
		iommu_group_put(group);
		return ERR_CAST(master);
	}

	iommu_group_set_iommudata(group, &master->ctx_drvdata,
					msm_iommu_release_group_iommudata);

	return group;
}

static irqreturn_t msm_iommu_global_fault_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct msm_iommu_drvdata *drvdata;
	u32 gfsr;
	int ret;

	drvdata = dev_get_drvdata(&pdev->dev);
	mutex_lock(&drvdata->glb_lock);

	if (drvdata->sec_id != -1) {
		dev_err(&pdev->dev,
			"NON-secure interrupt from secure %s\n",
			drvdata->name);
		ret = IRQ_HANDLED;
		goto fail;
	}

	ret = __enable_clocks(drvdata);
	if (ret) {
		ret = IRQ_NONE;
		goto fail;
	}

	gfsr = iommu_readl(drvdata, ARM_SMMU_GR0_sGFSR);
	if (gfsr) {
		dev_err_ratelimited(&pdev->dev,
			"Unexpected %s global fault !!\n",
			drvdata->name);
		dev_err_ratelimited(&pdev->dev,
			"GFSR    = %08x [%s%s%s%s%s%s%s%s%s%s]\n",
			gfsr,
			(gfsr & 0x01) ? "ICF " : "",
			(gfsr & 0x02) ? "USF " : "",
			(gfsr & 0x04) ? "SMCF " : "",
			(gfsr & 0x08) ? "UCBF " : "",
			(gfsr & 0x10) ? "UCIF " : "",
			(gfsr & 0x20) ? "CAF " : "",
			(gfsr & 0x40) ? "EF " : "",
			(gfsr & 0x80) ? "PF " : "",
			(gfsr & 0x40000000) ? "SS " : "",
			(gfsr & 0x80000000) ? "MULTI " : "");

		dev_err_ratelimited(&pdev->dev, "GFSYNR0 = %08x\n",
				iommu_readl(drvdata, ARM_SMMU_GR0_sGFSYNR0));
		dev_err_ratelimited(&pdev->dev, "GFSYNR1 = %08x\n",
				iommu_readl(drvdata, ARM_SMMU_GR0_sGFSYNR1));
		dev_err_ratelimited(&pdev->dev, "GFSYNR2 = %08x\n",
				iommu_readl(drvdata, ARM_SMMU_GR0_sGFSYNR2));
		iommu_writel(drvdata, ARM_SMMU_GR0_sGFSR, gfsr);
		ret = IRQ_HANDLED;
	} else
		ret = IRQ_NONE;

	__disable_clocks(drvdata);
fail:
	mutex_unlock(&drvdata->glb_lock);

	return ret;
}

static irqreturn_t msm_iommu_fault_handler_v2(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct msm_iommu_drvdata *drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	u64 faulty_iova = 0;
	u32 fsr, fsynr;
	int ctx, ret;

	drvdata = dev_get_drvdata(pdev->dev.parent);
	ctx_drvdata = dev_get_drvdata(&pdev->dev);

	mutex_lock(&ctx_drvdata->ctx_lock);
	ctx = ctx_drvdata->num;

	ret = __enable_clocks(drvdata);
	if (ret) {
		ret = IRQ_NONE;
		goto fail;
	}

	fsr = iommu_cb_readl(drvdata, ARM_SMMU_CB_FSR, ctx);
	if (!(fsr & FSR_FAULT)) {
		ret = IRQ_NONE;
		goto out;
	}

	faulty_iova = iommu_cb_readq(drvdata, ARM_SMMU_CB_FAR, ctx);

	if (!ctx_drvdata->attached_domain) {
		dev_err(&pdev->dev, "Bad domain in interrupt handler\n");
		ret = -ENOSYS;
	} else {
		ret = report_iommu_fault(ctx_drvdata->attached_domain,
			&ctx_drvdata->pdev->dev,
			faulty_iova, 0);
	}
	if (ret == -ENOSYS) {
		fsynr = iommu_cb_readl(drvdata, ARM_SMMU_CB_FSYNR0, ctx);

		dev_err_ratelimited(&pdev->dev,
				    "Unexpected IOMMU page fault!\n");
		dev_err_ratelimited(&pdev->dev,
				    "name = %s\n", drvdata->name);
		dev_err_ratelimited(&pdev->dev,
				    "context = %s (%d)\n", ctx_drvdata->name,
				    ctx_drvdata->num);
		dev_err_ratelimited(&pdev->dev,
				    "fsr=0x%x, iova=0x%08llx, fsynr=0x%x,"
				    " cb=%d\n",
				    fsr, faulty_iova, fsynr, ctx);
	}

	if (ret != -EBUSY)
		iommu_cb_writel(drvdata, ARM_SMMU_CB_FSR,
				ctx_drvdata->num, fsr);
	ret = IRQ_HANDLED;

out:
	__disable_clocks(drvdata);
fail:
	mutex_unlock(&ctx_drvdata->ctx_lock);

	return ret;
}

static int msm_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	//struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	struct platform_device *pdev, *ctx_pdev;
	struct msm_iommu_master *master;
	struct device_node *child;
	unsigned int asid = args->args[0];
	bool found = false;
	u32 val;
	int ret;

	if (args->args_count > 2)
		return -EINVAL;

	dev_dbg(dev, "getting pdev for %s\n", args->np->name);

	pdev = of_find_device_by_node(args->np);
	if (!pdev) {
		dev_dbg(dev, "iommu pdev not found\n");
		return -ENODEV;
	}

	iommu_drvdata = platform_get_drvdata(pdev);
	if (!iommu_drvdata)
		return -ENODEV;

	for_each_child_of_node(args->np, child) {
		ctx_pdev = of_find_device_by_node(child);
		if (!ctx_pdev)
			return -ENODEV;

		ctx_drvdata = platform_get_drvdata(ctx_pdev);

		ret = of_property_read_u32(child, "qcom,ctx-num", &val);
		if (ret)
			return ret;

		if (val == asid) {
			found = true;
			break;
		}
	}

	if (!found)
		return -ENODEV;

	dev_err(dev, "found ctx data for %s (num:%d)\n",
		ctx_drvdata->name, ctx_drvdata->num);

	master = devm_kzalloc(iommu_drvdata->dev, sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	INIT_LIST_HEAD(&master->list);
	master->ctx_num = args->args[0];
	master->dev = dev;
	master->iommu_drvdata = iommu_drvdata;
	master->ctx_drvdata = ctx_drvdata;

	dev_err(dev, "adding master for device %s\n", dev_name(dev));

	list_add_tail(&master->list, &iommu_masters);
	dev->archdata.iommu = master;
	//fwspec->iommu_priv = master;

	return iommu_fwspec_add_ids(dev, &ctx_drvdata->num, 1);
}

static bool msm_iommu_capable(enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	case IOMMU_CAP_NOEXEC:
		return true;
	default:
		return false;
	}
}

static struct iommu_ops msm_iommu_ops = {
	.capable = msm_iommu_capable,
	.domain_alloc = msm_iommu_domain_alloc,
	.domain_free = msm_iommu_domain_free,
	.attach_dev = msm_iommu_attach_dev,
	.detach_dev = msm_iommu_detach_dev,
	.map = msm_iommu_map,
	.unmap = msm_iommu_unmap,
	.flush_iotlb_all = msm_iommu_iotlb_sync,
	.iotlb_sync	= msm_iommu_iotlb_sync,
	.iova_to_phys = msm_iommu_iova_to_phys,
	.add_device = msm_iommu_add_device,
	.remove_device = msm_iommu_remove_device,
	.device_group = msm_iommu_device_group,
	.pgsize_bitmap = (SZ_4K | SZ_64K | SZ_2M | SZ_32M | SZ_1G),
	.of_xlate = msm_iommu_of_xlate,
};

static int msm_iommu_parse_dt(struct platform_device *pdev,
			      struct msm_iommu_drvdata *drvdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	u32 temp;
	int ret;

	if (of_device_is_compatible(np, "qcom,msm-mmu-500"))
		drvdata->model = MMU_500;
	else
		drvdata->model = QSMMU_V2;

	if (!of_property_read_u32(np, "qcom,cb-base-offset", &temp))
		drvdata->cb_base = drvdata->base + temp;
	else
		drvdata->cb_base = drvdata->base + 0x8000;

	ret = of_property_read_string(np, "label", &drvdata->name);
	if (ret)
		goto fail;

	/* If present, force switch to AArch64 addressing */
	if (of_property_read_bool(np, "qcom,use-aarch64-addressing"))
		drvdata->mmu_fmt = ARM_64_LPAE_S1;
	else
		drvdata->mmu_fmt = ARM_32_LPAE_S1;

	for_each_available_child_of_node(np, child)
		drvdata->ncb++;

	drvdata->sec_id = -1;
	of_property_read_u32(np, "qcom,iommu-secure-id", &drvdata->sec_id);

	mutex_lock(&iommu_list_lock);
	list_add(&drvdata->list, &iommu_list);
	mutex_unlock(&iommu_list_lock);

	return 0;

fail:
	return ret;
}

static int msm_iommu_sec_ptbl_init(struct device *dev)
{
	size_t psize = 0;
	unsigned int spare = 0;
	void *cpu_addr;
	dma_addr_t paddr;
	unsigned long attrs;
	static bool allocated = false;
	int ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	if (allocated)
		return 0;

	/* Optional: Not all TZ versions will accept this.
	 * If we fail, just go on without really caring about it
	 */
	ret = qcom_scm_iommu_set_cp_pool_size(spare, MAXIMUM_VIRT_SIZE);
	if (ret)
		dev_dbg(dev, "cannot set iommu max mapped size (%d)\n", ret);

	ret = qcom_scm_iommu_secure_ptbl_size(spare, &psize);
	if (ret) {
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",
			ret);
		return ret;
	}

	dev_info(dev, "iommu sec: pgtable size: %zu\n", psize);

	attrs = DMA_ATTR_NO_KERNEL_MAPPING;

	cpu_addr = dma_alloc_attrs(dev, psize, &paddr, GFP_KERNEL, attrs);
	if (!cpu_addr) {
		dev_err(dev, "failed to allocate %zu bytes for pgtable\n",
			psize);
		return -ENOMEM;
	}

	ret = qcom_scm_iommu_secure_ptbl_init(paddr, psize, spare);
	if (ret) {
		dev_err(dev, "failed to init iommu pgtable (%d)\n", ret);
		goto free_mem;
	}
	allocated = true;

	return 0;

free_mem:
	dma_free_attrs(dev, psize, cpu_addr, paddr,
		DMA_ATTR_NO_KERNEL_MAPPING);
	return ret;
}

static irqreturn_t msm_iommu_secure_fault_handler_v2(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct msm_iommu_drvdata *drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;

	drvdata = dev_get_drvdata(pdev->dev.parent);
	ctx_drvdata = dev_get_drvdata(&pdev->dev);

	mutex_lock(&ctx_drvdata->ctx_lock);

	if (!drvdata->ctx_attach_count) {
		dev_err_ratelimited(&pdev->dev,
				    "Unexpected IOMMU page fault from secure"
				    "unattached context bank %s!\n",
				    drvdata->name);
		/*
		 * We cannot determine which context bank caused the issue so
		 * we just return handled here to ensure IRQ handler code is
		 * happy
		 */
	}

	mutex_unlock(&ctx_drvdata->ctx_lock);
	return IRQ_HANDLED;
}

static int msm_iommu_pds_attach(struct device *dev, struct device **devs,
				char **pd_names)
{
	size_t num_pds = 0;
	int ret;
	int i;

	if (!pd_names)
		return 0;

	while (pd_names[num_pds])
		num_pds++;

	for (i = 0; i < num_pds; i++) {
		devs[i] = dev_pm_domain_attach_by_name(dev, pd_names[i]);
		if (IS_ERR(devs[i])) {
			ret = PTR_ERR(devs[i]);
			goto unroll_attach;
		}
	}

	return num_pds;

unroll_attach:
	for (i--; i >= 0; i--)
		dev_pm_domain_detach(devs[i], false);

	return ret;
};

static void msm_iommu_pds_detach(struct device **pds, size_t pd_count)
{
	int i;

	for (i = 0; i < pd_count; i++)
		dev_pm_domain_detach(pds[i], false);
}

static const struct of_device_id msm_iommu_ctx_match_table[] = {
	{ .compatible = "qcom,msm-mmu-500-ctx", },
	{ .compatible = "qcom,msm-smmu-v2-ctx", },
	{}
};

static int msm_iommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct msm_iommu_drvdata *drvdata;
	struct resource *res;
	resource_size_t ioaddr;
	int global_cfg_irq, global_client_irq;
	unsigned long rate;
	int ret;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iommu_base");
	drvdata->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(drvdata->base))
		return PTR_ERR(drvdata->base);

	drvdata->dev = dev;
	ioaddr = res->start;

	drvdata->iface = devm_clk_get(dev, "iface_clk");
	if (IS_ERR(drvdata->iface))
		return PTR_ERR(drvdata->iface);

	drvdata->core = devm_clk_get(dev, "core_clk");
	if (IS_ERR(drvdata->core))
		return PTR_ERR(drvdata->core);

	ret = msm_iommu_parse_dt(pdev, drvdata);
	if (ret)
		return ret;

/*
	ret = msm_iommu_pds_attach(dev, drvdata->pds,
				   power_domain_names);
	if (ret) {
		pr_err("Cannot attach PDs\n");
		return ret;
	}
*/
	ret = clk_prepare(drvdata->iface);
	if (ret)
		return ret;

	ret = clk_prepare(drvdata->core);
	if (ret)
		return ret;

	rate = clk_get_rate(drvdata->core);
	if (!rate) {
		rate = clk_round_rate(drvdata->core, 1000);
		clk_set_rate(drvdata->core, rate);
	}

	dev_dbg(&pdev->dev, "iface: %lu, core: %lu\n",
		 clk_get_rate(drvdata->iface), clk_get_rate(drvdata->core));

	if (drvdata->sec_id != -1) {
		ret = msm_iommu_sec_ptbl_init(dev);
		if (ret)
			return ret;
	}
	mutex_init(&drvdata->glb_lock);

	dev_info(dev, "device %s (model: %d) with %d ctx banks\n",
		 drvdata->name, drvdata->model, drvdata->ncb);

	platform_set_drvdata(pdev, drvdata);

	global_cfg_irq = platform_get_irq_byname(pdev, "global_cfg_NS_irq");
	if (global_cfg_irq < 0 && global_cfg_irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	global_client_irq =
			platform_get_irq_byname(pdev, "global_client_NS_irq");
	if (global_client_irq < 0 && global_client_irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (global_cfg_irq > 0) {
		ret = devm_request_threaded_irq(dev, global_cfg_irq,
						NULL,
						msm_iommu_global_fault_handler,
						IRQF_ONESHOT | IRQF_SHARED,
						"msm_iommu_global_cfg_irq",
						pdev);
		if (ret < 0)
			dev_err(dev, "Request Global CFG IRQ %d failed with ret=%d\n",
				global_cfg_irq, ret);
	}

	if (global_client_irq > 0) {
		ret = devm_request_threaded_irq(dev, global_client_irq,
						NULL,
						msm_iommu_global_fault_handler,
						IRQF_ONESHOT | IRQF_SHARED,
						"msm_iommu_global_client_irq",
						pdev);
		if (ret < 0)
			dev_err(dev, "Request Global Client IRQ %d failed with ret=%d\n",
				global_client_irq, ret);
	}

	ret = of_platform_populate(np, msm_iommu_ctx_match_table, NULL, dev);
	if (ret) {
		dev_err(dev, "Failed to create iommu context device\n");
		return ret;
	}

	ret = __enable_clocks(drvdata);
	if (ret) {
		dev_err(dev, "Failed to enable clocks\n");
		return ret;
	}

	ret = iommu_device_sysfs_add(&drvdata->iommu, dev, NULL,
				     dev_name(dev));
	if (ret) {
		dev_err(dev, "Cannot add msm iommu to sysfs\n");
		goto fail;
	}

	iommu_device_set_ops(&drvdata->iommu, &msm_iommu_ops);
	iommu_device_set_fwnode(&drvdata->iommu, &dev->of_node->fwnode);

	ret = iommu_device_register(&drvdata->iommu);
	if (ret) {
		dev_err(dev, "Cannot register MSM IOMMU device\n");
		goto fail;
	};

	if (!iommu_present(&platform_bus_type))
		bus_set_iommu(&platform_bus_type, &msm_iommu_ops);

	return 0;
fail:
	__disable_clocks(drvdata);
	return ret;
}

static int msm_iommu_remove(struct platform_device *pdev)
{
	struct msm_iommu_drvdata *drv;

	drv = platform_get_drvdata(pdev);
	if (!drv)
		return -EINVAL;

	__disable_clocks(drv);
	clk_unprepare(drv->iface);
	clk_unprepare(drv->core);

	mutex_lock(&iommu_list_lock);
	list_del(&drv->list);
	mutex_unlock(&iommu_list_lock);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static int msm_iommu_ctx_probe(struct platform_device *pdev)
{
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	struct msm_iommu_drvdata *drvdata;
	struct resource rp;
	struct device *dev = &pdev->dev;
	unsigned long cb_offset;
	int irq = 0, ret = 0;
	u32 reg;

	ctx_drvdata = devm_kzalloc(&pdev->dev, sizeof(*ctx_drvdata),
				   GFP_KERNEL);
	if (!ctx_drvdata)
		return -ENOMEM;

	ctx_drvdata->pdev = pdev;

	drvdata = dev_get_drvdata(pdev->dev.parent);
	if (!drvdata)
		return -EPROBE_DEFER;

	if (of_property_read_u32_index(pdev->dev.of_node, "reg", 0, &reg))
		return -ENODEV;

	INIT_LIST_HEAD(&ctx_drvdata->attached_elm);
	mutex_init(&ctx_drvdata->ctx_lock);

	ctx_drvdata->secure_context = of_property_read_bool(dev->of_node,
				"qcom,secure-context");

	irq = platform_get_irq(pdev, 0);
	if (irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (irq > 0) {
		if (drvdata->sec_id == -1)
			ret = devm_request_threaded_irq(dev, irq, NULL,
					msm_iommu_fault_handler_v2,
					IRQF_ONESHOT | IRQF_SHARED,
					"msm_iommu_nonsecure_irq", pdev);
		else
			ret = devm_request_threaded_irq(dev, irq, NULL,
					msm_iommu_secure_fault_handler_v2,
					IRQF_ONESHOT | IRQF_SHARED,
					"msm_iommu_secure_irq", pdev);
		if (ret) {
			dev_err(dev, "Request IRQ %d failed with ret=%d\n",
				irq, ret);
			return ret;
		}
	}

	ret = of_address_to_resource(dev->parent->of_node, 0, &rp);
	if (ret)
		return ret;

	/* Calculate the context bank number using the base addresses.
	 * Typically CB0 base address is 0x8000 pages away if the number
	 * of CBs are <=8. So, assume the offset 0x8000 until mentioned
	 * explicitely.
	 */
	cb_offset = drvdata->cb_base - drvdata->base;
	ctx_drvdata->num = (reg - rp.start - cb_offset);
	if (ctx_drvdata->num > 0)
		ctx_drvdata->num /= 0x1000;

	if (of_property_read_string(dev->of_node, "label",
				    &ctx_drvdata->name))
		ctx_drvdata->name = dev_name(&pdev->dev);

	ctx_drvdata->asid = -1;

	platform_set_drvdata(pdev, ctx_drvdata);

	dev_dbg(&pdev->dev, "context %s using bank %d\n",
		 ctx_drvdata->name, ctx_drvdata->num);

	return 0;
}

static int msm_iommu_ctx_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id msm_iommu_match_table[] = {
	{ .compatible = "qcom,msm-mmu-500", },
	{ .compatible = "qcom,msm-smmu-v2", },
	{}
};

static struct platform_driver msm_iommu_driver = {
	.driver = {
		.name = "msm_iommu",
		.of_match_table = msm_iommu_match_table,
	},
	.probe = msm_iommu_probe,
	.remove = msm_iommu_remove,
};

static struct platform_driver msm_iommu_ctx_driver = {
	.driver = {
		.name = "msm_iommu_ctx",
		.of_match_table = msm_iommu_ctx_match_table,
	},
	.probe = msm_iommu_ctx_probe,
	.remove = msm_iommu_ctx_remove,
};

static int __init msm_iommu_driver_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&msm_iommu_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&msm_iommu_ctx_driver);
	if (ret)
		platform_driver_unregister(&msm_iommu_driver);

	return ret;
}
device_initcall(msm_iommu_driver_init);

static void __exit msm_iommu_driver_exit(void)
{
	platform_driver_unregister(&msm_iommu_ctx_driver);
	platform_driver_unregister(&msm_iommu_driver);
}
module_exit(msm_iommu_driver_exit);
