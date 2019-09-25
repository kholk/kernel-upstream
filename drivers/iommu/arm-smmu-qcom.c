// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

#include <linux/qcom_scm.h>

#include "arm-smmu.h"

struct qcom_smmu {
	struct arm_smmu_device smmu;
};

static int qcom_sdm845_smmu500_reset(struct arm_smmu_device *smmu)
{
	int ret;

	arm_mmu500_reset(smmu);

	/*
	 * To address performance degradation in non-real time clients,
	 * such as USB and UFS, turn off wait-for-safe on sdm845 based boards,
	 * such as MTP and db845, whose firmwares implement secure monitor
	 * call handlers to turn on/off the wait-for-safe logic.
	 */
	ret = qcom_scm_qsmmu500_wait_safe_toggle(0);
	if (ret)
		dev_warn(smmu->dev, "Failed to turn off SAFE logic\n");

	return ret;
}

static int qcom_smmuv2_reset(struct arm_smmu_device *smmu)
{
	int ret;

	arm_mmu500_reset(smmu);

	/*
	 * To address performance degradation in non-real time clients,
	 * such as USB and UFS, turn off wait-for-safe on sdm845 based boards,
	 * such as MTP and db845, whose firmwares implement secure monitor
	 * call handlers to turn on/off the wait-for-safe logic.
	 */
	ret = qcom_scm_qsmmu500_wait_safe_toggle(0);
	if (ret)
		dev_warn(smmu->dev, "Failed to turn off SAFE logic\n");

	return ret;
}

static const struct arm_smmu_impl qcom_smmu_impl = {
	.reset = qcom_sdm845_smmu500_reset,
};

static const struct arm_smmu_impl qcom_smmuv2_impl = {
	.reset = qcom_smmuv2_reset,
};

struct arm_smmu_device *qcom_smmu_impl_init(struct arm_smmu_device *smmu)
{
	struct qcom_smmu *qsmmu;

	qsmmu = devm_kzalloc(smmu->dev, sizeof(*qsmmu), GFP_KERNEL);
	if (!qsmmu)
		return ERR_PTR(-ENOMEM);

	qsmmu->smmu = *smmu;

	if (of_device_is_compatible(smmu->dev->of_node, "qcom,sdm845-smmu-500"))
		qsmmu->smmu.impl = &qcom_smmu_impl;

	if (of_device_is_compatible(smmu->dev->of_node, "qcom,msm8956-smmu"))
		qsmmu->smmu.impl = &qcom_smmuv2_impl;

	devm_kfree(smmu->dev, smmu);

	return &qsmmu->smmu;
}
