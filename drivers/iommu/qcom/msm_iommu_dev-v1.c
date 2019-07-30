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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>

#include <asm/cacheflush.h>

#include <linux/io-pgtable.h>
#include "msm_iommu_hw-v1.h"
#include "msm_iommu_priv.h"
#include <linux/qcom_scm.h>

char *power_domain_names[MMU_POWER_DOMAINS_CNT+1] =
	{
		"cx", "mx", NULL
	};

static const struct of_device_id msm_iommu_ctx_match_table[];
static struct iommu_access_ops *iommu_access_ops;

static DEFINE_MUTEX(iommu_list_lock);
static LIST_HEAD(iommu_list);

extern struct iommu_access_ops iommu_access_ops_v1;

static int msm_iommu_parse_dt(struct platform_device *pdev,
			      struct msm_iommu_drvdata *drvdata)
{
	struct device_node *child;
	int ret;

	drvdata->dev = &pdev->dev;

	for_each_available_child_of_node(pdev->dev.of_node, child)
		drvdata->ncb++;

	ret = of_property_read_string(pdev->dev.of_node, "label",
				      &drvdata->name);
	if (ret)
		goto fail;

	drvdata->sec_id = -1;
	of_property_read_u32(pdev->dev.of_node, "qcom,iommu-secure-id",
				     &drvdata->sec_id);

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
	if (ret) {
		dev_err(dev, "cannot set iommu max mapped size (%d)\n", ret);
	}

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

irqreturn_t msm_iommu_secure_fault_handler_v2(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct msm_iommu_drvdata *drvdata;
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	struct msm_scm_fault_regs_dump *regs;
	int ret = IRQ_HANDLED;

	iommu_access_ops->iommu_lock_acquire(0);

	BUG_ON(!pdev);

	drvdata = dev_get_drvdata(pdev->dev.parent);
	BUG_ON(!drvdata);

	ctx_drvdata = dev_get_drvdata(&pdev->dev);
	BUG_ON(!ctx_drvdata);

	regs = kzalloc(sizeof(*regs), GFP_ATOMIC);
	if (!regs) {
		pr_err("%s: Couldn't allocate memory\n", __func__);
		goto lock_release;
	}

	if (!drvdata->ctx_attach_count) {
		pr_err("Unexpected IOMMU page fault from secure context bank!\n");
		pr_err("name = %s\n", drvdata->name);
		pr_err("Power is OFF. Unable to read page fault information\n");
		/*
		 * We cannot determine which context bank caused the issue so
		 * we just return handled here to ensure IRQ handler code is
		 * happy
		 */
		goto free_regs;
	}

free_regs:
	kfree(regs);
lock_release:
	iommu_access_ops->iommu_lock_release(0);
	return ret;
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

static int msm_iommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct msm_iommu_drvdata *drvdata;
	struct resource *res;
	resource_size_t ioaddr;
	int ret;
	int global_cfg_irq, global_client_irq;
	u32 temp;
	unsigned long rate;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iommu_base");
	drvdata->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(drvdata->base))
		return PTR_ERR(drvdata->base);

	drvdata->glb_base = drvdata->base;
	drvdata->phys_base = ioaddr = res->start;

	/* Optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "smmu_local_base");
	if (res) {
		drvdata->smmu_local_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(drvdata->smmu_local_base) &&
		    PTR_ERR(drvdata->smmu_local_base) != -EPROBE_DEFER)
			drvdata->smmu_local_base = NULL;
	}

	if (of_device_is_compatible(np, "qcom,msm-mmu-500"))
		drvdata->model = MMU_500;

	drvdata->iface = devm_clk_get(dev, "iface_clk");
	if (IS_ERR(drvdata->iface))
		return PTR_ERR(drvdata->iface);

	ret = clk_prepare(drvdata->iface);
	if (ret)
		return ret;

	drvdata->core = devm_clk_get(dev, "core_clk");
	if (IS_ERR(drvdata->core)) {
		clk_unprepare(drvdata->iface);
		return PTR_ERR(drvdata->core);
	}
/*
	ret = msm_iommu_pds_attach(dev, drvdata->power_domains,
				   power_domain_names);
	if (ret) {
		pr_err("Cannot attach PDs\n");
		return ret;
	}
*/
	ret = clk_prepare(drvdata->core);
	if (ret)
		return ret;

	if (!of_property_read_u32(np, "qcom,cb-base-offset", &temp))
		drvdata->cb_base = drvdata->base + temp;
	else
		drvdata->cb_base = drvdata->base + 0x8000;

	rate = clk_get_rate(drvdata->core);
	if (!rate) {
		rate = clk_round_rate(drvdata->core, 1000);
		clk_set_rate(drvdata->core, rate);
	}

	dev_dbg(&pdev->dev, "iface: %lu, core: %lu\n",
		 clk_get_rate(drvdata->iface), clk_get_rate(drvdata->core));

	ret = msm_iommu_parse_dt(pdev, drvdata);
	if (ret)
		return ret;

	if (drvdata->sec_id != -1) {
		ret = msm_iommu_sec_ptbl_init(dev);
		if (ret)
			return ret;
	}

	dev_info(dev, "device %s (model: %d) with %d ctx banks\n",
		 drvdata->name, drvdata->model, drvdata->ncb);

	platform_set_drvdata(pdev, drvdata);

	global_cfg_irq = platform_get_irq_byname(pdev, "global_cfg_NS_irq");
	if (global_cfg_irq < 0 && global_cfg_irq == -EPROBE_DEFER)
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

	global_client_irq =
			platform_get_irq_byname(pdev, "global_client_NS_irq");
	if (global_client_irq < 0 && global_client_irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;

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

	INIT_LIST_HEAD(&drvdata->masters);

	idr_init(&drvdata->asid_idr);

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
				     "msm-iommu.%pa", &ioaddr);
	if (ret) {
		dev_err(dev, "Cannot add msm-iommu.%pa to sysfs\n", &ioaddr);
		return ret;
	}

	return msm_iommu_init(drvdata);
}

static int msm_iommu_remove(struct platform_device *pdev)
{
	struct msm_iommu_drvdata *drv;

	drv = platform_get_drvdata(pdev);
	if (!drv)
		return -EINVAL;

	idr_destroy(&drv->asid_idr);
	__disable_clocks(drv);
	clk_unprepare(drv->iface);
	clk_unprepare(drv->core);

	mutex_lock(&iommu_list_lock);
	list_del(&drv->list);
	mutex_unlock(&iommu_list_lock);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static int msm_iommu_ctx_parse_dt(struct platform_device *pdev,
				  struct msm_iommu_ctx_drvdata *ctx_drvdata)
{
	struct resource rp;
	int irq = 0, ret = 0;
	struct msm_iommu_drvdata *drvdata;
	u32 nsid;
	u32 n_sid_mask;
	u32 reg;
	unsigned long cb_offset;

	drvdata = dev_get_drvdata(pdev->dev.parent);
	if (!drvdata)
		return -EPROBE_DEFER;

	if (of_property_read_u32_index(pdev->dev.of_node, "reg", 0, &reg))
		return -ENODEV;

	ctx_drvdata->secure_context = of_property_read_bool(pdev->dev.of_node,
				"qcom,secure-context");
	ctx_drvdata->needs_secure_map = of_property_read_bool(pdev->dev.of_node,
				"qcom,require-tz-mapping");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0 && irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (irq > 0) {
		if (drvdata->sec_id == -1)
			ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					msm_iommu_fault_handler_v2,
					IRQF_ONESHOT | IRQF_SHARED,
					"msm_iommu_nonsecure_irq", pdev);
		else
			ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					msm_iommu_secure_fault_handler_v2,
					IRQF_ONESHOT | IRQF_SHARED,
					"msm_iommu_secure_irq", pdev);
		if (ret) {
			pr_err("Request IRQ %d failed with ret=%d\n",
				irq, ret);
			goto out;
		}
	}

	ret = of_address_to_resource(pdev->dev.parent->of_node, 0, &rp);
	if (ret)
		goto out;

	/* Calculate the context bank number using the base addresses.
	 * Typically CB0 base address is 0x8000 pages away if the number
	 * of CBs are <=8. So, assume the offset 0x8000 until mentioned
	 * explicitely.
	 */
	cb_offset = drvdata->cb_base - drvdata->base;
	ctx_drvdata->num = (reg - rp.start - cb_offset);
	if (ctx_drvdata->num > 0)
		ctx_drvdata->num /= 0x1000;

	if (of_property_read_string(pdev->dev.of_node, "label",
				    &ctx_drvdata->name))
		ctx_drvdata->name = dev_name(&pdev->dev);

	if (!of_get_property(pdev->dev.of_node, "qcom,iommu-ctx-sids", &nsid)) {
		ret = -EINVAL;
		goto out;
	}

	if (nsid >= sizeof(ctx_drvdata->sids)) {
		ret = -EINVAL;
		goto out;
	}

	if (of_property_read_u32_array(pdev->dev.of_node, "qcom,iommu-ctx-sids",
				       ctx_drvdata->sids,
				       nsid / sizeof(*ctx_drvdata->sids))) {
		ret = -EINVAL;
		goto out;
	}

	ctx_drvdata->nsid = nsid;
	ctx_drvdata->asid = -1;

	if (!of_get_property(pdev->dev.of_node, "qcom,iommu-sid-mask",
			     &n_sid_mask)) {
		memset(ctx_drvdata->sid_mask, 0, sizeof(*ctx_drvdata->sid_mask));
		goto out;
	}

	if (n_sid_mask != nsid) {
		ret = -EINVAL;
		goto out;
	}

	if (of_property_read_u32_array(pdev->dev.of_node, "qcom,iommu-sid-mask",
				ctx_drvdata->sid_mask,
				n_sid_mask / sizeof(*ctx_drvdata->sid_mask))) {
		ret = -EINVAL;
		goto out;
	}
	ctx_drvdata->n_sid_mask = n_sid_mask;

out:
	return ret;
}

static int msm_iommu_ctx_probe(struct platform_device *pdev)
{
	struct msm_iommu_ctx_drvdata *ctx_drvdata;
	int ret;

	if (!pdev->dev.parent)
		return -EINVAL;

	ctx_drvdata = devm_kzalloc(&pdev->dev, sizeof(*ctx_drvdata),
				   GFP_KERNEL);
	if (!ctx_drvdata)
		return -ENOMEM;

	ctx_drvdata->pdev = pdev;
	INIT_LIST_HEAD(&ctx_drvdata->attached_elm);

	ret = msm_iommu_ctx_parse_dt(pdev, ctx_drvdata);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, ctx_drvdata);

	dev_info(&pdev->dev, "context %s using bank %d\n",
		 ctx_drvdata->name, ctx_drvdata->num);

	return 0;
}

static int msm_iommu_ctx_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id msm_iommu_match_table[] = {
	{ .compatible = "qcom,msm-smmu-v1", },
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

static const struct of_device_id msm_iommu_ctx_match_table[] = {
	{ .compatible = "qcom,msm-smmu-v1-ctx", },
	{ .compatible = "qcom,msm-smmu-v2-ctx", },
	{}
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
	int ret;

	iommu_access_ops = &iommu_access_ops_v1;

	ret = platform_driver_register(&msm_iommu_driver);
	if (ret) {
		pr_err("Failed to register IOMMU driver\n");
		return ret;
	}

	ret = platform_driver_register(&msm_iommu_ctx_driver);
	if (ret) {
		pr_err("Failed to register IOMMU context driver\n");
		platform_driver_unregister(&msm_iommu_driver);
		return ret;
	}

	return 0;
}
device_initcall(msm_iommu_driver_init);

static void __exit msm_iommu_driver_exit(void)
{
	platform_driver_unregister(&msm_iommu_ctx_driver);
	platform_driver_unregister(&msm_iommu_driver);
}
module_exit(msm_iommu_driver_exit);

MODULE_LICENSE("GPL v2");
