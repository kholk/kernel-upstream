// SPDX-License-Identifier: GPL-2.0-only

#include <linux/extcon-provider.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/of.h>

#define USB_ID_DEBOUNCE_MS 5 /* ms */

#define RT_STS 0x10
#define INPUT_STS 0x0D
#define USBIN_SRC_DET_BIT BIT(2)
#define USBIN_OV_BIT BIT(1)

#define USBIN_9V BIT(5)
#define USBIN_UNREG BIT(4)
#define USBIN_LV BIT(3)

struct qcom_pmi8950_otg_info {
	struct device *dev;
	struct platform_device *pdev;

	struct regmap *regmap;
	struct extcon_dev *edev;
	int irq;
	struct delayed_work wq_detcable;
	unsigned long debounce_jiffies;

	u32 otg_base;
};

static const unsigned int qcom_pmi8950_otg_cable[] = {
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

#define RID_GND_DET_STS BIT(2)
static bool is_otg_present_schg_lite(struct qcom_pmi8950_otg_info *info)
{
	int rc;
	u8 reg;

	rc = regmap_bulk_read(info->regmap, info->otg_base + RT_STS, &reg, 1);
	if (rc < 0) {
		dev_err(info->dev, "Couldn't read otg RT status rc = %d\n", rc);
		return false;
	}

	return !!(reg & RID_GND_DET_STS);
}

static void qcom_pmi8950_otg_detect_cable(struct work_struct *work)
{
	struct qcom_pmi8950_otg_info *info =
		container_of(to_delayed_work(work), struct qcom_pmi8950_otg_info,
			     wq_detcable);

	bool otg_present = is_otg_present_schg_lite(info);

	dev_notice(info->dev, "%s: otg_present: %d\n", __func__, otg_present);

	extcon_set_state_sync(info->edev, EXTCON_USB_HOST, otg_present);
}

static irqreturn_t qcom_usb_irq_handler(int irq, void *dev_id)
{
	struct qcom_pmi8950_otg_info *info = dev_id;

	queue_delayed_work(system_power_efficient_wq, &info->wq_detcable,
			   info->debounce_jiffies);

	return IRQ_HANDLED;
}

static int qcom_pmi8950_otg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_pmi8950_otg_info *info;
	int ret;
	u32 base;
	u8 subtype;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!info->regmap) {
		dev_err(dev, "Parent regmap unavailable.\n");
		return -ENXIO;
	}

	ret = of_property_read_u32(dev->of_node, "reg", &base);
	if (ret) {
		dev_err(dev, "Failed to read reg");
		return ret;
	}

#define SUBTYPE_REG 0x5
	ret = regmap_bulk_read(info->regmap, base + SUBTYPE_REG, &subtype, 1);
	if (ret) {
		dev_err(dev, "Peripheral subtype read failed ret=%d\n", ret);
		return ret;
	}
	dev_info(dev, "subtype is 0x%x\n", subtype);
#define SMBCHG_LITE_OTG_SUBTYPE 0x58
	if (subtype != SMBCHG_LITE_OTG_SUBTYPE) {
		dev_err(dev, "Wrong subtype");
		return -ENXIO;
	}
	info->otg_base = base;

	info->dev = dev;
	info->pdev = pdev;

	info->edev = devm_extcon_dev_allocate(dev, qcom_pmi8950_otg_cable);
	if (IS_ERR(info->edev)) {
		dev_err(dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	ret = devm_extcon_dev_register(dev, info->edev);
	if (ret < 0) {
		dev_err(dev, "failed to register extcon device\n");
		return ret;
	}

	info->debounce_jiffies = msecs_to_jiffies(USB_ID_DEBOUNCE_MS);
	INIT_DELAYED_WORK(&info->wq_detcable, qcom_pmi8950_otg_detect_cable);

	info->irq = platform_get_irq_byname(pdev, "usbid-change");
	if (info->irq < 0) {
		dev_err(dev, "Failed to get irq: %d\n", info->irq);
		return info->irq;
	}

	ret = devm_request_threaded_irq(dev, info->irq, NULL,
					qcom_usb_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					pdev->name, info);
	if (ret < 0) {
		dev_err(dev, "failed to request handler for ID IRQ\n");
		return ret;
	}

	platform_set_drvdata(pdev, info);
	device_init_wakeup(dev, 1);

	/* Perform initial detection */
	qcom_pmi8950_otg_detect_cable(&info->wq_detcable.work);

	dev_notice(dev, "Probe ok");

	return 0;
}

static int qcom_pmi8950_otg_remove(struct platform_device *pdev)
{
	struct qcom_pmi8950_otg_info *info = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&info->wq_detcable);

	return 0;
}

static const struct of_device_id qcom_pmi8950_otg_id_table[] = {
	{ .compatible = "qcom,pmi8950-otg" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_pmi8950_otg_id_table);

static struct platform_driver qcom_pmi8950_otg_driver = {
	.probe = qcom_pmi8950_otg_probe,
	.remove = qcom_pmi8950_otg_remove,
	.driver =
		{
			.name = "qcom-pmi8950-otg",
			.of_match_table = qcom_pmi8950_otg_id_table,
		},
};
module_platform_driver(qcom_pmi8950_otg_driver);

MODULE_DESCRIPTION("Qualcomm PMI8950 OTG sense extcon driver");
MODULE_LICENSE("GPL v2");
