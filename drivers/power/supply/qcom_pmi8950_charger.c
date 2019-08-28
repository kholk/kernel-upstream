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

#define RID_GND_DET_STS BIT(2)

#define SUBTYPE_REG 0x5

#define SMBCHG_USB_CHGPTH_SUBTYPE 0x4
#define SMBCHG_LITE_USB_CHGPTH_SUBTYPE 0x54
#define SMBCHG_LITE_OTG_SUBTYPE 0x58

struct qcom_pmi8950_charger_data;

struct qcom_pmi8950_charger_info {
	struct device *dev;
	struct platform_device *pdev;
	struct qcom_pmi8950_charger_data *data;

	struct regmap *regmap;
	struct extcon_dev *edev;
	int irq;
	struct delayed_work wq_detcable;
	unsigned long debounce_jiffies;

	u32 reg_base;
};

struct qcom_pmi8950_charger_data {
	bool (*is_present)(struct qcom_pmi8950_charger_info *);
	unsigned int extcon_id;
	const char *irq_name;
	unsigned long irq_trigger;
};

static const unsigned int qcom_pmi8950_charger_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static bool is_src_detect_high(struct qcom_pmi8950_charger_info *info)
{
	int rc;
	u8 reg;

	rc = regmap_bulk_read(info->regmap, info->reg_base + RT_STS, &reg, 1);
	if (rc < 0) {
		dev_err(info->dev, "Couldn't read usb rt status rc = %d\n", rc);
		return false;
	}

	dev_info(info->dev, "RT_STS: %u\n", reg);

	return reg &= USBIN_SRC_DET_BIT;
}

static bool is_usb_present(struct qcom_pmi8950_charger_info *info)
{
	int rc;
	u8 reg;

	rc = regmap_bulk_read(info->regmap, info->reg_base + RT_STS, &reg, 1);
	if (rc < 0) {
		dev_err(info->dev, "Couldn't read usb rt status rc = %d\n", rc);
		return false;
	}

	dev_info(info->dev, "RT_STS: %u\n", reg);

	if (!(reg & USBIN_SRC_DET_BIT) || (reg & USBIN_OV_BIT))
		return false;

	rc = regmap_bulk_read(info->regmap, info->reg_base + INPUT_STS, &reg,
			      1);
	if (rc < 0) {
		dev_err(info->dev, "Couldn't read usb status rc = %d\n", rc);
		return false;
	}

	dev_info(info->dev, "INPUT_STS: %u\n", reg);

	return !!(reg & (USBIN_9V | USBIN_UNREG | USBIN_LV));
}

static bool is_otg_present_schg_lite(struct qcom_pmi8950_charger_info *info)
{
	int rc;
	u8 reg;

	rc = regmap_bulk_read(info->regmap, info->reg_base + RT_STS, &reg, 1);
	if (rc < 0) {
		dev_err(info->dev, "Couldn't read otg RT status rc = %d\n", rc);
		return false;
	}

	return !!(reg & RID_GND_DET_STS);
}

static void qcom_pmi8950_charger_detect_cable(struct work_struct *work)
{
	struct qcom_pmi8950_charger_info *info =
		container_of(to_delayed_work(work),
			     struct qcom_pmi8950_charger_info, wq_detcable);

	bool present = info->data->is_present(info);

	extcon_set_state_sync(info->edev, info->data->extcon_id, present);
}

/* Specifics: */

static bool is_charger_present(struct qcom_pmi8950_charger_info *info)
{
	bool usb_present = is_usb_present(info);
	bool src_detect = is_src_detect_high(info);

	dev_notice(info->dev, "%s: otg_present: %d, src_detect: %d\n", __func__,
		   usb_present, src_detect);

	return src_detect && usb_present;
}

static bool is_otg_present(struct qcom_pmi8950_charger_info *info)
{
	bool otg_present = is_otg_present_schg_lite(info);

	dev_notice(info->dev, "%s: otg_present: %d\n", __func__, otg_present);

	return otg_present;
}

static struct qcom_pmi8950_charger_data charger_data = {
	.is_present = is_charger_present,
	.extcon_id = EXTCON_USB,
	.irq_name = "usbin-src-det",
	.irq_trigger = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
};

static struct qcom_pmi8950_charger_data otg_data = {
	.is_present = is_otg_present,
	.extcon_id = EXTCON_USB_HOST,
	.irq_name = "usbid-change",
	.irq_trigger = IRQF_TRIGGER_FALLING,
};

static irqreturn_t qcom_pmi8950_charger_irq_handler(int irq, void *dev_id)
{
	struct qcom_pmi8950_charger_info *info = dev_id;

	queue_delayed_work(system_power_efficient_wq, &info->wq_detcable,
			   info->debounce_jiffies);

	return IRQ_HANDLED;
}

static int qcom_pmi8950_charger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_pmi8950_charger_info *info;
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

	ret = regmap_bulk_read(info->regmap, base + SUBTYPE_REG, &subtype, 1);
	if (ret) {
		dev_err(dev, "Peripheral subtype read failed ret=%d\n", ret);
		return ret;
	}
	dev_info(dev, "subtype is 0x%x\n", subtype);

	switch (subtype) {
	case SMBCHG_USB_CHGPTH_SUBTYPE:
	case SMBCHG_LITE_USB_CHGPTH_SUBTYPE:
		info->data = &charger_data;
		break;
	case SMBCHG_LITE_OTG_SUBTYPE:
		info->data = &otg_data;
		break;
	default:
		dev_err(dev, "Wrong subtype");
		return -ENXIO;
	}
	info->reg_base = base;

	info->dev = dev;
	info->pdev = pdev;

	info->edev = devm_extcon_dev_allocate(dev, qcom_pmi8950_charger_cable);
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
	INIT_DELAYED_WORK(&info->wq_detcable,
			  qcom_pmi8950_charger_detect_cable);

	info->irq = platform_get_irq_byname(pdev, info->data->irq_name);
	if (info->irq < 0) {
		dev_err(dev, "Failed to get irq: %d\n", info->irq);
		return info->irq;
	}

	ret = devm_request_threaded_irq(
		dev, info->irq, NULL, qcom_pmi8950_charger_irq_handler,
		info->data->irq_trigger | IRQF_ONESHOT,
		pdev->name, info);
	if (ret < 0) {
		dev_err(dev, "failed to request handler for ID IRQ\n");
		return ret;
	}

	platform_set_drvdata(pdev, info);
	device_init_wakeup(dev, 1);

	/* Perform initial detection */
	qcom_pmi8950_charger_detect_cable(&info->wq_detcable.work);

	dev_notice(dev, "Probe ok");

	return 0;
}

static int qcom_pmi8950_charger_remove(struct platform_device *pdev)
{
	struct qcom_pmi8950_charger_info *info = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&info->wq_detcable);

	return 0;
}

static const struct of_device_id qcom_pmi8950_charger_id_table[] = {
	{ .compatible = "qcom,pmi8950-charger" },
	// TODO: Use .data here, cross-validate subtype?
	{ .compatible = "qcom,pmi8950-otg" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_pmi8950_charger_id_table);

static struct platform_driver qcom_pmi8950_charger_driver = {
	.probe = qcom_pmi8950_charger_probe,
	.remove = qcom_pmi8950_charger_remove,
	.driver =
		{
			.name = "qcom-pmi8950-charger",
			.of_match_table = qcom_pmi8950_charger_id_table,
		},
};
module_platform_driver(qcom_pmi8950_charger_driver);

MODULE_DESCRIPTION("Qualcomm USB SRC extcon driver");
MODULE_LICENSE("GPL v2");
