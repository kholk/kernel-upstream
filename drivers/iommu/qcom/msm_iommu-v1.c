/*
 * MSM Secure IOMMU v1/v2 Driver
 * Copyright (C) 2017-2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 *
 * May contain portions of code (c) 2012-2014, The Linux Foundation.
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

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt
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
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>
#include <linux/iopoll.h>
//#include <asm/sizes.h>
#include <linux/dma-iommu.h>
#include <linux/qcom_scm.h>
#include <linux/amba/bus.h>

#include <linux/io-pgtable.h>

#include "msm_iommu_priv.h"

#include "msm_iommu_hw-v1.h"

/* Max ASID width is 8-bit */
#define MAX_ASID	0xff

/* Not on mainline kernels, for now */
//#define QCOM_IOMMU_V1_USE_AARCH64 

#ifdef QCOM_IOMMU_V1_USE_AARCH64
 #define MMU_IAS 36
 #define MMU_OAS 40
 #define QCIOMMU_PGTABLE_OPS	ARM_64_LPAE_S1
#else /* AArch32 LPAE */
 #define MMU_IAS 32
 #define MMU_OAS 40
 #define QCIOMMU_PGTABLE_OPS	ARM_32_LPAE_S1
#endif

#define MMU_SEP (MMU_IAS - 1)

struct msm_iommu_master {
	struct list_head list;
	unsigned int ctx_num;
	struct device *dev;
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
};

static LIST_HEAD(iommu_masters);
static LIST_HEAD(iommu_drvdatas);

static DEFINE_MUTEX(msm_iommu_lock);

struct dump_regs_tbl_entry dump_regs_tbl[MAX_DUMP_REGS];
static struct iommu_ops msm_iommu_ops;

int __enable_clocks(struct msm_iommu_drvdata *drvdata)
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

void __disable_clocks(struct msm_iommu_drvdata *drvdata)
{
//	clk_disable(drvdata->core);
//	clk_disable(drvdata->iface);
}

static void _iommu_lock_acquire(unsigned int need_extra_lock)
{
	mutex_lock(&msm_iommu_lock);
}

static void _iommu_lock_release(unsigned int need_extra_lock)
{
	mutex_unlock(&msm_iommu_lock);
}

struct iommu_access_ops iommu_access_ops_v1 = {
	.iommu_clk_on = __enable_clocks,
	.iommu_clk_off = __disable_clocks,
	.iommu_lock_acquire = _iommu_lock_acquire,
	.iommu_lock_release = _iommu_lock_release,
};

static ATOMIC_NOTIFIER_HEAD(msm_iommu_notifier_list);

void msm_iommu_register_notify(struct notifier_block *nb)
{
	atomic_notifier_chain_register(&msm_iommu_notifier_list, nb);
}
EXPORT_SYMBOL(msm_iommu_register_notify);

static void __sync_tlb(struct msm_iommu_drvdata *iommu_drvdata, int ctx,
		       struct msm_iommu_priv *priv)
{
	void __iomem *base = iommu_drvdata->cb_base;
	unsigned int val;
	unsigned int res;

	SET_TLBSYNC(base, ctx, 0);
	/* No barrier needed due to read dependency */

	res = readl_poll_timeout_atomic(CTX_REG(CB_TLBSTATUS, base, ctx), val,
				(val & CB_TLBSTATUS_SACTIVE) == 0, 0, 1000000);
	if (res)
		BUG();
}

static int __flush_iotlb(struct iommu_domain *domain)
{
	struct msm_iommu_priv *base_priv = to_msm_priv(domain);
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	int ret = 0;

	list_for_each_entry(ctx_drvdata, &base_priv->list_attached, attached_elm) {
		BUG_ON(!ctx_drvdata->pdev || !ctx_drvdata->pdev->dev.parent);

		iommu_drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);
		BUG_ON(!iommu_drvdata);

		ret = __enable_clocks(iommu_drvdata);
		if (ret)
			goto fail;

		SET_TLBIASID(iommu_drvdata->cb_base, ctx_drvdata->num,
			     base_priv->asid);
		__sync_tlb(iommu_drvdata, ctx_drvdata->num, base_priv);
		__disable_clocks(iommu_drvdata);
	}
fail:
	return ret;
}

static void msm_iommu_tlb_sync(void *cookie)
{
	struct iommu_domain *domain = cookie;
	struct msm_iommu_priv *base_priv = to_msm_priv(domain);
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	int ret = 0;

	list_for_each_entry(ctx_drvdata, &base_priv->list_attached, attached_elm) {
		BUG_ON(!ctx_drvdata->pdev || !ctx_drvdata->pdev->dev.parent);

		iommu_drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);
		BUG_ON(!iommu_drvdata);

		ret = __enable_clocks(iommu_drvdata);
		if (ret)
			goto fail;

		__sync_tlb(iommu_drvdata, ctx_drvdata->num, base_priv);

		__disable_clocks(iommu_drvdata);
	}
fail:
	if (ret)
		pr_err("%s: ERROR %d !!\n", __func__, ret);
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
	struct msm_iommu_priv *base_priv = to_msm_priv(domain);
	struct msm_iommu_drvdata *iommu_drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	unsigned long va = iova;
	int ret = 0;

	list_for_each_entry(ctx_drvdata, &base_priv->list_attached, attached_elm) {
		BUG_ON(!ctx_drvdata->pdev || !ctx_drvdata->pdev->dev.parent);

		iommu_drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);
		BUG_ON(!iommu_drvdata);

		ret = __enable_clocks(iommu_drvdata);
		if (ret)
			goto fail;

		va &= ~12UL;
		va |= base_priv->asid;

		if (leaf) {
			SET_TLBIVAL(iommu_drvdata->cb_base,
						ctx_drvdata->num, va);
		} else {
			SET_TLBIVA(iommu_drvdata->cb_base,
						ctx_drvdata->num, va);
		}

		__disable_clocks(iommu_drvdata);
	}
fail:
	if (ret)
		pr_err("%s: ERROR %d !!\n", __func__, ret);
	return;
}

static const struct iommu_gather_ops msm_iommu_gather_ops = {
	.tlb_flush_all	= msm_iommu_tlb_flush_all,
	.tlb_add_flush	= msm_iommu_tlb_flush_range_nosync,
	.tlb_sync	= msm_iommu_tlb_sync,
};

int msm_iommu_sec_program_iommu(struct msm_iommu_drvdata *drvdata,
			struct msm_iommu_ctx_drvdata *ctx_drvdata)
{
	int ret;

	if (drvdata->smmu_local_base) {
		writel_relaxed(0xFFFFFFFF,
			       drvdata->smmu_local_base + SMMU_INTR_SEL_NS);
		mb();
	}

	ret = qcom_scm_restore_sec_cfg(drvdata->sec_id, ctx_drvdata->num);
	if (ret) {
		pr_err("scm call IOMMU_SECURE_CFG failed\n");
		return ret;
	}

	return ret;
}

static void __reset_context(struct msm_iommu_drvdata *iommu_drvdata, int ctx)
{
	void __iomem *base = iommu_drvdata->cb_base;

	/* Don't set ACTLR to zero because if context bank is in
	 * bypass mode (say after iommu_detach), still this ACTLR
	 * value matters for micro-TLB caching.
	 */
	if (iommu_drvdata->model != MMU_500)
		SET_ACTLR(base, ctx, 0);

	SET_FAR(base, ctx, 0);
	SET_FSRRESTORE(base, ctx, 0);
	SET_NMRR(base, ctx, 0);
	SET_PAR(base, ctx, 0);
	SET_PRRR(base, ctx, 0);
	SET_SCTLR(base, ctx, 0);
	SET_TTBCR(base, ctx, 0);
	SET_TTBR0(base, ctx, 0);
	SET_TTBR1(base, ctx, 0);
	mb();
}

static void __program_context(struct msm_iommu_drvdata *iommu_drvdata,
			      struct msm_iommu_ctx_drvdata *ctx_drvdata,
			      struct msm_iommu_priv *priv)
{
	void __iomem *cb_base = iommu_drvdata->cb_base;
	unsigned int ctx = ctx_drvdata->num;
	u64 tcr;
//	u64 ttbr1_conf = 0;
	__reset_context(iommu_drvdata, ctx);

	priv->asid = ctx_drvdata->num;

#ifdef QCOM_IOMMU_V1_USE_AARCH64
	if (qcom_scm_iommu_set_pt_format(iommu_drvdata->sec_id,
				     ctx_drvdata->num, 1)) {
		pr_err("Fatal: Cannot set AArch64 pagetable format!!!\n");
		BUG();
	}
#endif

	tcr = priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr;
/*
	ttbr1_conf = (1 << 23);
	ttbr1_conf |= (7ULL << (15+32));

priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr |= (7ULL << (15+32));
*/
#ifdef QCOM_IOMMU_V1_USE_AARCH64
	SET_TTBCR(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr);

	SET_TCR2(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr >> 32);
#else /* AArch32 LPAE */
	SET_CB_TCR2_SEP(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.tcr >> 32);

	SET_CB_TTBCR_EAE(iommu_drvdata->cb_base, ctx, 1);
#endif

	SET_TTBR0(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.ttbr[0] |
			((u64)priv->asid << CB_TTBR0_ASID_SHIFT));

	SET_TTBR1(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.ttbr[1] |
			((u64)priv->asid << CB_TTBR1_ASID_SHIFT));


	SET_CB_MAIR0(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.mair[0]);

	SET_CB_MAIR1(iommu_drvdata->cb_base, ctx,
			priv->pgtbl_cfg.arm_lpae_s1_cfg.mair[1]);

	/* Ensure that ASID assignment has completed before we use
	 * ASID for TLB invalidation. Here, mb() is required because
	 * both these registers are separated by more than 1KB. */
	mb();

#ifdef QCOM_IOMMU_V1_USE_AARCH64
	/* Disable stall unconditionally for AArch64 addressing */
	SET_CB_SCTLR_CFCFG(cb_base, ctx, 0);
	SET_CB_SCTLR_HUPCF(cb_base, ctx, 1);
#else /* AArch32 LPAE */
//	if (priv->attributes & (1 << DOMAIN_ATTR_CB_STALL_DISABLE)) {
		SET_CB_SCTLR_CFCFG(cb_base, ctx, 0);
		SET_CB_SCTLR_HUPCF(cb_base, ctx, 1);
//	}
#endif

	SET_CB_SCTLR_CFIE(cb_base, ctx, 1);
	SET_CB_SCTLR_CFRE(cb_base, ctx, 1);
	SET_CB_SCTLR_AFE(cb_base, ctx, 1);
	SET_CB_SCTLR_TRE(cb_base, ctx, 1);
	SET_CB_SCTLR_ASIDPNE(cb_base, ctx, 1);
	SET_CB_SCTLR_M(cb_base, ctx, 1);

	mb();
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

pr_err("%s\n", __func__);
	if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

pr_err("allocating domain\n");

	if (type == IOMMU_DOMAIN_DMA) {
		pr_err("DMA DOMAIN!\n");
		ret = iommu_get_dma_cookie(&priv->domain);
		if (ret)
			goto err;
	}

	INIT_LIST_HEAD(&priv->list_attached);
	mutex_init(&priv->init_mutex);
	spin_lock_init(&priv->pgtbl_lock);
pr_err("%s alloc OK\n", __func__);
	return &priv->domain;

err:
pr_err("%s ALLOC FAILURE\n", __func__);
	kfree(priv);
	return NULL;
}

static void msm_iommu_domain_free(struct iommu_domain *domain)
{
	struct msm_iommu_priv *priv = to_msm_priv(domain);

	if (!priv) {
		pr_err("OUCH!\n");
		return;
	}

	iommu_put_dma_cookie(domain);

	free_io_pgtable_ops(priv->pgtbl_ops);
pr_err("domain freed\n");
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
	int ret = 0;
	int is_secure;
	bool secure_ctx;
	unsigned long flags, pgtable_quirks = 0;
pr_err("attach_dev\n");

	if (!fwspec->iommu_priv) {
		pr_err("attach_dev: FWSPEC iommu_priv is missing. Bailing out!\n");
		return -ENODEV;
	}

	priv = to_msm_priv(domain);
	if (!priv || !dev) {
		return -EINVAL;
	}

	mutex_lock(&priv->init_mutex);



	master = fwspec->iommu_priv;// dev->archdata.iommu;

	iommu_drvdata = master->iommu_drvdata;
	ctx_drvdata = master->ctx_drvdata;

	if (!iommu_drvdata || !ctx_drvdata) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!(priv->client_name))
		priv->client_name = dev_name(dev); //master->ctx_drvdata->name; //dev_name(dev);

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

	ret = __enable_clocks(iommu_drvdata);
	if (ret)
		goto unlock;

	/* We can only do this once */
	if (!iommu_drvdata->ctx_attach_count) {
		ret = msm_iommu_sec_program_iommu(iommu_drvdata, ctx_drvdata);
		if (ret)
			goto unlock;
	}

	//pgtable_quirks |= IO_PGTABLE_QUIRK_TLBI_ON_MAP; // Do we need this? 

	pr_err("%s: Assigning pagetable\n", dev_name(&ctx_drvdata->pdev->dev));

	//iommu_drvdata = dev_get_drvdata(ctx_drvdata->pdev->dev.parent);

	/* Make sure the domain is initialized */
	priv->pgtbl_cfg = (struct io_pgtable_cfg) {
		//.quirks		= IO_PGTABLE_QUIRK_NO_DMA,
		.pgsize_bitmap	= msm_iommu_ops.pgsize_bitmap,
		.ias		= MMU_IAS,
		.oas		= MMU_OAS,
		.tlb		= &msm_iommu_gather_ops,
		.iommu_dev	= &ctx_drvdata->pdev->dev,
	};
	domain->geometry.aperture_start = 0;
	domain->geometry.aperture_end = (1ULL << priv->pgtbl_cfg.ias) - 1;
	domain->geometry.force_aperture = true;

	pgtbl_ops = alloc_io_pgtable_ops(QCIOMMU_PGTABLE_OPS,
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

//goto add_domain;

	secure_ctx = !!(ctx_drvdata->secure_context > 0);
	if (secure_ctx) {
		dev_dbg(dev, "Detected secure context.\n");
		goto add_domain;
	}

pr_err("progctx\n");
	__program_context(iommu_drvdata, ctx_drvdata, priv);

	/* Ensure TLB is clear */
	if (iommu_drvdata->model != MMU_500) {
		SET_TLBIASID(iommu_drvdata->cb_base, ctx_drvdata->num,
			     ctx_drvdata->asid);
		__sync_tlb(iommu_drvdata, ctx_drvdata->num, priv);
	}
pr_err("clkdisable\n");
	__disable_clocks(iommu_drvdata);

add_domain:
	if (ctx_drvdata->needs_secure_map)
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

	if (iommu_drvdata->model == MMU_500) {
		SET_TLBIASID(iommu_drvdata->cb_base, ctx_drvdata->num,
			     ctx_drvdata->asid);
		__sync_tlb(iommu_drvdata, ctx_drvdata->num, priv);
	}

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

	if (!priv) {
		pr_err("no priv\n");
		return -EINVAL;
	}

	if (!priv->pgtbl_ops) {
		pr_err("no ops for client %s\n", priv->client_name);
		return -ENODEV;
	}

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
pr_err("iova_to_phys\n");
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

static inline void print_ctx_mem_attr_regs(struct msm_iommu_context_reg regs[])
{
	pr_err("MAIR0   = %08x    MAIR1   = %08x\n",
		 regs[DUMP_REG_MAIR0].val, regs[DUMP_REG_MAIR1].val);
}

void print_ctx_regs(struct msm_iommu_context_reg regs[])
{
	uint32_t fsr = regs[DUMP_REG_FSR].val;
	u64 ttbr;
	enum dump_reg iter;

	pr_err("FAR    = %016llx\n",
		COMBINE_DUMP_REG(
			regs[DUMP_REG_FAR1].val,
			regs[DUMP_REG_FAR0].val));
	pr_err("PAR    = %016llx\n",
		COMBINE_DUMP_REG(
			regs[DUMP_REG_PAR1].val,
			regs[DUMP_REG_PAR0].val));
	pr_err("FSR    = %08x [%s%s%s%s%s%s%s%s%s]\n", fsr,
			(fsr & 0x02) ? "TF " : "",
			(fsr & 0x04) ? "AFF " : "",
			(fsr & 0x08) ? "PF " : "",
			(fsr & 0x10) ? "EF " : "",
			(fsr & 0x20) ? "TLBMCF " : "",
			(fsr & 0x40) ? "TLBLKF " : "",
			(fsr & 0x80) ? "MHF " : "",
			(fsr & 0x40000000) ? "SS " : "",
			(fsr & 0x80000000) ? "MULTI " : "");

	pr_err("FSYNR0 = %08x    FSYNR1 = %08x\n",
		 regs[DUMP_REG_FSYNR0].val, regs[DUMP_REG_FSYNR1].val);

	ttbr = COMBINE_DUMP_REG(regs[DUMP_REG_TTBR0_1].val,
				regs[DUMP_REG_TTBR0_0].val);
	if (regs[DUMP_REG_TTBR0_1].valid)
		pr_err("TTBR0  = %016llx\n", ttbr);
	else
		pr_err("TTBR0  = %016llx (32b)\n", ttbr);

	ttbr = COMBINE_DUMP_REG(regs[DUMP_REG_TTBR1_1].val,
				regs[DUMP_REG_TTBR1_0].val);

	if (regs[DUMP_REG_TTBR1_1].valid)
		pr_err("TTBR1  = %016llx\n", ttbr);
	else
		pr_err("TTBR1  = %016llx (32b)\n", ttbr);

	pr_err("SCTLR  = %08x    ACTLR  = %08x\n",
		 regs[DUMP_REG_SCTLR].val, regs[DUMP_REG_ACTLR].val);
	pr_err("CBAR  = %08x    CBFRSYNRA  = %08x\n",
		regs[DUMP_REG_CBAR_N].val, regs[DUMP_REG_CBFRSYNRA_N].val);
	print_ctx_mem_attr_regs(regs);

	for (iter = DUMP_REG_FIRST; iter < MAX_DUMP_REGS; ++iter)
		if (!regs[iter].valid)
			pr_err("NOTE: Value actually unknown for %s\n",
				dump_regs_tbl[iter].name);
}

static void __print_ctx_regs(struct msm_iommu_drvdata *drvdata, int ctx,
					unsigned int fsr)
{
	void __iomem *base = drvdata->base;
	void __iomem *cb_base = drvdata->cb_base;
	bool is_secure = drvdata->sec_id != -1;

	struct msm_iommu_context_reg regs[MAX_DUMP_REGS];
	unsigned int i;
	memset(regs, 0, sizeof(regs));

	for (i = DUMP_REG_FIRST; i < MAX_DUMP_REGS; ++i) {
		struct msm_iommu_context_reg *r = &regs[i];
		unsigned long regaddr = dump_regs_tbl[i].reg_offset;
		if (is_secure &&
			dump_regs_tbl[i].dump_reg_type != DRT_CTX_REG) {
			r->valid = 0;
			continue;
		}
		r->valid = 1;
		switch (dump_regs_tbl[i].dump_reg_type) {
		case DRT_CTX_REG:
			r->val = GET_CTX_REG(regaddr, cb_base, ctx);
			break;
		case DRT_GLOBAL_REG:
			r->val = GET_GLOBAL_REG(regaddr, base);
			break;
		case DRT_GLOBAL_REG_N:
			r->val = GET_GLOBAL_REG_N(regaddr, ctx, base);
			break;
		default:
			pr_info("Unknown dump_reg_type...\n");
			r->valid = 0;
			break;
		}
	}
	print_ctx_regs(regs);
}

static void print_global_regs(void __iomem *base, unsigned int gfsr)
{
	pr_err("GFAR    = %016llx\n", GET_GFAR(base));

	pr_err("GFSR    = %08x [%s%s%s%s%s%s%s%s%s%s]\n", gfsr,
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

	pr_err("GFSYNR0	= %08x\n", GET_GFSYNR0(base));
	pr_err("GFSYNR1	= %08x\n", GET_GFSYNR1(base));
	pr_err("GFSYNR2	= %08x\n", GET_GFSYNR2(base));
}

irqreturn_t msm_iommu_global_fault_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct msm_iommu_drvdata *drvdata;
	unsigned int gfsr;
	int ret;

	mutex_lock(&msm_iommu_lock);
	BUG_ON(!pdev);

	drvdata = dev_get_drvdata(&pdev->dev);
	BUG_ON(!drvdata);

	if (drvdata->sec_id != -1) {
		pr_err("NON-secure interrupt from secure %s\n", drvdata->name);
		ret = IRQ_HANDLED;
		goto fail;
	}

	ret = __enable_clocks(drvdata);
	if (ret) {
		ret = IRQ_NONE;
		goto fail;
	}

	gfsr = GET_GFSR(drvdata->base);
	if (gfsr) {
		pr_err("Unexpected %s global fault !!\n", drvdata->name);
		print_global_regs(drvdata->base, gfsr);
		SET_GFSR(drvdata->base, gfsr);
		ret = IRQ_HANDLED;
	} else
		ret = IRQ_NONE;

	__disable_clocks(drvdata);
fail:
	mutex_unlock(&msm_iommu_lock);

	return ret;
}

irqreturn_t msm_iommu_fault_handler_v2(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct msm_iommu_drvdata *drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	unsigned int fsr;
	int ret;
	u64 faulty_iova = 0;

	mutex_lock(&msm_iommu_lock);

	BUG_ON(!pdev);

	drvdata = dev_get_drvdata(pdev->dev.parent);
	BUG_ON(!drvdata);

	ctx_drvdata = dev_get_drvdata(&pdev->dev);
	BUG_ON(!ctx_drvdata);

	ret = __enable_clocks(drvdata);
	if (ret) {
		ret = IRQ_NONE;
		goto fail;
	}

	fsr = GET_FSR(drvdata->cb_base, ctx_drvdata->num);
	if (fsr) {
		if (!ctx_drvdata->attached_domain) {
			pr_err("Bad domain in interrupt handler\n");
			ret = -ENOSYS;
		} else {
			faulty_iova =
				GET_FAR(drvdata->cb_base, ctx_drvdata->num);
			ret = report_iommu_fault(ctx_drvdata->attached_domain,
				&ctx_drvdata->pdev->dev,
				faulty_iova, 0);

		}
		if (ret == -ENOSYS) {
			pr_err("Unexpected IOMMU page fault!\n");
			pr_err("name = %s\n", drvdata->name);
			pr_err("context = %s (%d)\n", ctx_drvdata->name,
							ctx_drvdata->num);
			pr_err("Interesting registers:\n");
			__print_ctx_regs(drvdata,
					ctx_drvdata->num, fsr);
		}

		if (ret != -EBUSY)
			SET_FSR(drvdata->cb_base, ctx_drvdata->num, fsr);
		ret = IRQ_HANDLED;
	} else
		ret = IRQ_NONE;

	__disable_clocks(drvdata);
fail:
	mutex_unlock(&msm_iommu_lock);

	return ret;
}

static void msm_iommu_build_dump_regs_table(void)
{
	DUMP_REG_INIT(DUMP_REG_FAR0,	CB_FAR,       1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_FAR1,	CB_FAR + 4,   1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_PAR0,	CB_PAR,       1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_PAR1,	CB_PAR + 4,   1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_FSR,	CB_FSR,       1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_FSYNR0,	CB_FSYNR0,    1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_FSYNR1,	CB_FSYNR1,    1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_TTBR0_0,	CB_TTBR0,     1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_TTBR0_1,	CB_TTBR0 + 4, 0, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_TTBR1_0,	CB_TTBR1,     1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_TTBR1_1,	CB_TTBR1 + 4, 0, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_SCTLR,	CB_SCTLR,     1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_ACTLR,	CB_ACTLR,     1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_PRRR,	CB_PRRR,      1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_NMRR,	CB_NMRR,      1, DRT_CTX_REG);
	DUMP_REG_INIT(DUMP_REG_CBAR_N,	CBAR,         1, DRT_GLOBAL_REG_N);
	DUMP_REG_INIT(DUMP_REG_CBFRSYNRA_N, CBFRSYNRA, 1, DRT_GLOBAL_REG_N);
}

static int msm_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
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

#if 0
static int msm_iommu_dma_supported(struct iommu_domain *domain,
				  struct device *dev, u64 mask)
{
	return ((1ULL << 32) - 1) < mask ? 0 : 1;
}
#endif
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
	//.pgsize_bitmap	= SZ_4K | SZ_64K | SZ_1M | SZ_16M,
	.of_xlate = msm_iommu_of_xlate,
};

int msm_iommu_init(struct msm_iommu_drvdata *drvdata)
{
	struct device *dev = drvdata->dev;
	int ret;

	iommu_device_set_ops(&drvdata->iommu, &msm_iommu_ops);
	iommu_device_set_fwnode(&drvdata->iommu, &dev->of_node->fwnode);

	ret = iommu_device_register(&drvdata->iommu);
	if (ret) {
		dev_err(dev, "Cannot register MSM IOMMU device\n");
		return ret;
	};

	if (!iommu_present(&platform_bus_type))
		bus_set_iommu(&platform_bus_type, &msm_iommu_ops);

#ifdef CONFIG_ARM_AMBA
	if (!iommu_present(&amba_bustype))
		bus_set_iommu(&amba_bustype, &msm_iommu_ops);
#endif

#if 0 //def CONFIG_PCI
	if (!iommu_present(&pci_bus_type))
		bus_set_iommu(&pci_bus_type, &msm_iommu_ops);
#endif

	msm_iommu_build_dump_regs_table();

	//list_add(*drvdata, &iommu_drvdatas);

	return 0;
}
