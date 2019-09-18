/*
 * Copyright (C) 2019 AngeloGioacchino Del Regno <kholk11@gmail.com>
 *
 * from DT configurations on Sony Xperia Loire platform
 * H455TAX01.0 IPS LCD Panel Driver
 *
 * Copyright (c) 2016 Sony Mobile Communications Inc.
 * Parameters from dsi-panel-somc-synaptics-jdi-720p-cmd.dtsi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/display_timing.h>
#include <video/videomode.h>

//#define MDSS_BUG_SOLVED

struct h455tax01_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct backlight_device *backlight;

	struct regulator *vddio_supply;
	struct regulator *avdd_supply;
	struct regulator *tvdd_supply;
	struct regulator *tvddio_supply;

	struct gpio_desc *pan_reset_gpio;
	struct gpio_desc *ts_vddio_gpio;
	struct gpio_desc *ts_reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static const u8 cmd_unk1[2] = {0xb0, 0x00};
static const u8 cmd_unk2[2] = {0xd6, 0x01};
static const u8 cmd_on_unk3[3] = {0xc4, 0x70, 0x03};
static const u8 cmd_on_unk4[14] =
	{
		0xEC, 0x64, 0xDC, 0x7A, 0x7A, 0x3D, 0x00, 0x0B,
		0x0B, 0x13, 0x15, 0x68, 0x0B, 0xB5,
	};
static const u8 cmd_unk5[2] = {0xb0, 0x03};
static const u8 cmd_on_unk6[2] = {0x35, 0x00};
static const u8 cmd_on_unk7[2] = {0x36, 0x00};
static const u8 cmd_on_unk8[2] = {0x3A, 0x77};
static const u8 cmd_on_unk9[5] = {0x2A, 0x00, 0x00, 0x02, 0xCF};
static const u8 cmd_on_unk10[5] = {0x2B, 0x00, 0x00, 0x04, 0xFF};
static const u8 cmd_on_unk11[3] = {0x44, 0x00, 0x00};

static const u8 cmd_off_unk4[14] =
	{
		0xEC, 0x64, 0xDC, 0x7A, 0x7A, 0x3D, 0x00, 0x0B,
		0x0B, 0x13, 0x15, 0x68, 0x0B, 0x95,
	};

static inline struct h455tax01_panel *to_h455tax01(struct drm_panel *panel)
{
	return container_of(panel, struct h455tax01_panel, base);
}

static int h455tax01_panel_enable(struct drm_panel *panel)
{
	struct h455tax01_panel *h455tax01_panel = to_h455tax01(panel);

	if (h455tax01_panel->enabled)
		return 0;

	h455tax01_panel->enabled = true;

	return 0;
}

static int h455tax01_panel_init(struct h455tax01_panel *h455tax01_panel)
{
	struct device *dev = &h455tax01_panel->dsi->dev;
	ssize_t wr_sz = 0;
	int rc = 0;

	h455tax01_panel->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_unk1, sizeof(cmd_unk1));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 1: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_unk2, sizeof(cmd_unk2));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 2: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk3, sizeof(cmd_on_unk3));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 3: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk4, sizeof(cmd_on_unk4));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 4: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_unk5, sizeof(cmd_unk5));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 5: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk6, sizeof(cmd_on_unk6));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 6: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk7, sizeof(cmd_on_unk7));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 7: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk8, sizeof(cmd_on_unk8));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 8: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk9, sizeof(cmd_on_unk9));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 9: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk10, sizeof(cmd_on_unk10));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 10: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_on_unk11, sizeof(cmd_on_unk11));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 11: %ld\n", wr_sz);

	rc = mipi_dsi_dcs_exit_sleep_mode(h455tax01_panel->dsi);
	if (rc < 0) {
		dev_err(dev, "Cannot send exit sleep cmd: %d\n", rc);
		return rc;
	}

	msleep(120);

	return rc;
}

static int h455tax01_panel_on(struct h455tax01_panel *h455tax01_panel)
{
	struct device *dev = &h455tax01_panel->dsi->dev;
	int rc = 0;

	rc = mipi_dsi_dcs_set_display_on(h455tax01_panel->dsi);
	if (rc < 0) {
		dev_err(dev, "Cannot send disp on cmd: %d\n", rc);
		return rc;
	}

	msleep(120);

	return rc;
}

static int h455tax01_panel_disable(struct drm_panel *panel)
{
	struct h455tax01_panel *h455tax01_panel = to_h455tax01(panel);

	if (!h455tax01_panel->enabled)
		return 0;

	h455tax01_panel->enabled = false;

	return 0;
}

static int h455tax01_panel_off(struct h455tax01_panel *h455tax01_panel)
{
	struct device *dev = &h455tax01_panel->dsi->dev;
	ssize_t wr_sz = 0;
	int rc;

	h455tax01_panel->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	rc = mipi_dsi_dcs_set_display_off(h455tax01_panel->dsi);
	if (rc < 0)
		dev_err(dev, "Cannot set display off: %d\n", rc);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_unk1, sizeof(cmd_unk1));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send OFF command 1: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_unk2, sizeof(cmd_unk2));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send OFF command 2: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_off_unk4, sizeof(cmd_off_unk4));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send OFF command 4: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(h455tax01_panel->dsi,
					cmd_unk5, sizeof(cmd_unk5));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send OFF command 5: %ld\n", wr_sz);

	rc = mipi_dsi_dcs_enter_sleep_mode(h455tax01_panel->dsi);
	if (rc < 0)
		dev_err(dev, "Cannot enter sleep mode: %d\n", rc);

	msleep(100);

	return rc;
}

static int h455tax01_panel_unprepare(struct drm_panel *panel)
{
	struct h455tax01_panel *h455tax01_panel = to_h455tax01(panel);
	int rc = 0;

	if (!h455tax01_panel->prepared)
		return 0;
#ifdef MDSS_BUG_SOLVED
	if (h455tax01_panel->ts_reset_gpio) {
		gpiod_set_value(h455tax01_panel->ts_reset_gpio, 0);
		usleep_range(10000, 11000);
	}
#endif
	h455tax01_panel_off(h455tax01_panel);

	/* TODO: LAB/IBB */
#ifdef MDSS_BUG_SOLVED
	regulator_disable(h455tax01_panel->tvdd_supply);
	regulator_disable(h455tax01_panel->avdd_supply);
	regulator_disable(h455tax01_panel->vddio_supply);

	if (h455tax01_panel->pan_reset_gpio) {
		gpiod_set_value(h455tax01_panel->pan_reset_gpio, 0);
		usleep_range(10000, 11000);
	}

#endif
	h455tax01_panel->prepared = false;

	return rc;
}

static int h455tax01_panel_prepare(struct drm_panel *panel)
{
	struct h455tax01_panel *h455tax01_panel = to_h455tax01(panel);
	struct device *dev = &h455tax01_panel->dsi->dev;
	int rc;

	if (h455tax01_panel->prepared)
		return 0;

	/* Power rail VDDIO => in-cell panel main */
	rc = regulator_enable(h455tax01_panel->vddio_supply);
	if (rc < 0)
		return rc;

	msleep(80);

	/* Power rail AVDD => in-cell touch-controller main */
	rc = regulator_enable(h455tax01_panel->avdd_supply);
	if (rc < 0)
		dev_err(dev, "Cannot enable AVDD: %d\n", rc);
	else
		usleep_range(1000, 1100);

	/* TODO: LAB/IBB */

#ifdef MDSS_BUG_SOLVED
	/* Enable the in-cell supply to panel */
	rc = regulator_enable(h455tax01_panel->tvdd_supply);
	if (rc < 0) {
		dev_err(dev, "Cannot enable TVDD: %d\n", rc);
		goto poweroff_s1;
	} else {
		usleep_range(1000, 1100);
	}
#endif
	/* Enable the in-cell supply to touch-controller */
	rc = regulator_enable(h455tax01_panel->tvddio_supply);
	if (rc) {
		dev_err(dev, "Cannot enable TVDDIO: %d", rc);
		goto poweroff_s2;
	}
	usleep_range(1000, 1100);

	if (h455tax01_panel->ts_reset_gpio)
		gpiod_set_value(h455tax01_panel->ts_reset_gpio, 1);

#ifdef MDSS_BUG_SOLVED
	if (h455tax01_panel->pan_reset_gpio) {
		gpiod_set_value(h455tax01_panel->pan_reset_gpio, 0);
		usleep_range(10000, 10000);
		gpiod_set_value(h455tax01_panel->pan_reset_gpio, 1);
		usleep_range(10000, 11000);
	};
#endif

	rc = h455tax01_panel_init(h455tax01_panel);
	if (rc < 0) {
		dev_err(dev, "Cannot initialize panel: %d\n", rc);
		goto poweroff_s2;
	}

	rc = h455tax01_panel_on(h455tax01_panel);
	if (rc < 0) {
		dev_err(dev, "Cannot poweron panel: %d\n", rc);
		goto poweroff_s2;
	}

	h455tax01_panel->prepared = true;

	return 0;

poweroff_s2:
	/* Disable it to avoid current/voltage spikes in the enable path */
	regulator_disable(h455tax01_panel->tvdd_supply);
poweroff_s1:
	regulator_disable(h455tax01_panel->avdd_supply);
	regulator_disable(h455tax01_panel->vddio_supply);

	return rc;
}

static const struct drm_display_mode default_mode = {
	.clock = 149506,
	.hdisplay = 720,
	.hsync_start = 720 + 20,
	.hsync_end = 720 + 20 + 8,
	.htotal = 720 + 20 + 8 + 8,
	.vdisplay = 1280,
	.vsync_start = 1280 + 1500,
	.vsync_end = 1280 + 1500 + 8,
	.vtotal = 1280 + 1500 + 8 + 8,
	.vrefresh = 60,
	//.flags = 0,
};

static int h455tax01_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct h455tax01_panel *h455tax01_panel = to_h455tax01(panel);
	struct device *dev = &h455tax01_panel->dsi->dev;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 56;
	panel->connector->display_info.height_mm = 100;

	return 1;
}

static const struct drm_panel_funcs h455tax01_panel_funcs = {
	.disable = h455tax01_panel_disable,
	.unprepare = h455tax01_panel_unprepare,
	.prepare = h455tax01_panel_prepare,
	.enable = h455tax01_panel_enable,
	.get_modes = h455tax01_panel_get_modes,
};

static const struct of_device_id h455tax01_of_match[] = {
	{ .compatible = "jdi,syn-incell-h455tax01", },
	{ }
};
MODULE_DEVICE_TABLE(of, h455tax01_of_match);

static int h455tax01_panel_add(struct h455tax01_panel *h455tax01_panel)
{
	struct device *dev = &h455tax01_panel->dsi->dev;
	int rc;

	h455tax01_panel->mode = &default_mode;

	h455tax01_panel->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(h455tax01_panel->vddio_supply)) {
		dev_err(dev, "cannot get vddio regulator: %ld\n",
			PTR_ERR(h455tax01_panel->vddio_supply));
		return PTR_ERR(h455tax01_panel->vddio_supply);
	}

	h455tax01_panel->avdd_supply = devm_regulator_get_optional(dev, "avdd");
	if (IS_ERR(h455tax01_panel->avdd_supply)) {
		dev_err(dev, "cannot get avdd regulator: %ld\n",
			PTR_ERR(h455tax01_panel->avdd_supply));
		h455tax01_panel->avdd_supply = NULL;
	}

	h455tax01_panel->tvdd_supply = devm_regulator_get_optional(dev, "tvdd");
	if (IS_ERR(h455tax01_panel->tvdd_supply)) {
		dev_err(dev, "cannot get tvdd regulator: %ld\n",
			PTR_ERR(h455tax01_panel->tvdd_supply));
		h455tax01_panel->tvdd_supply = NULL;
	}

	h455tax01_panel->tvddio_supply = devm_regulator_get_optional(dev, "tvddio");
	if (IS_ERR(h455tax01_panel->tvddio_supply)) {
		dev_err(dev, "cannot get tvddio regulator: %ld\n",
			PTR_ERR(h455tax01_panel->tvddio_supply));
		h455tax01_panel->tvddio_supply = NULL;
	}

	h455tax01_panel->pan_reset_gpio = devm_gpiod_get(dev,
					"preset", GPIOD_ASIS);
	if (IS_ERR(h455tax01_panel->pan_reset_gpio)) {
		dev_err(dev, "cannot get preset-gpio: %ld\n",
			PTR_ERR(h455tax01_panel->pan_reset_gpio));
		h455tax01_panel->pan_reset_gpio = NULL;
	}

	h455tax01_panel->ts_reset_gpio = devm_gpiod_get(dev,
					"treset", GPIOD_ASIS);
	if (IS_ERR(h455tax01_panel->ts_reset_gpio)) {
		dev_err(dev, "cannot get treset-gpio: %ld\n",
			PTR_ERR(h455tax01_panel->ts_reset_gpio));
		h455tax01_panel->ts_reset_gpio = NULL;
	}

	drm_panel_init(&h455tax01_panel->base);
	h455tax01_panel->base.funcs = &h455tax01_panel_funcs;
	h455tax01_panel->base.dev = dev;

	rc = drm_panel_add(&h455tax01_panel->base);
	if (rc < 0)
		pr_err("drm panel add failed\n");

	return rc;
}

static void h455tax01_panel_del(struct h455tax01_panel *h455tax01_panel)
{
	if (h455tax01_panel->base.dev)
		drm_panel_remove(&h455tax01_panel->base);
}

static int h455tax01_panel_probe(struct mipi_dsi_device *dsi)
{
	struct h455tax01_panel *h455tax01_panel;
	int rc;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	h455tax01_panel = devm_kzalloc(&dsi->dev,
				sizeof(*h455tax01_panel), GFP_KERNEL);
	if (!h455tax01_panel)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, h455tax01_panel);
	h455tax01_panel->dsi = dsi;

	rc = h455tax01_panel_add(h455tax01_panel);
	if (rc < 0)
		return rc;

	return mipi_dsi_attach(dsi);
}

static int h455tax01_panel_remove(struct mipi_dsi_device *dsi)
{
	struct h455tax01_panel *h455tax01_panel = mipi_dsi_get_drvdata(dsi);
	struct device *dev = &h455tax01_panel->dsi->dev;
	int ret;

	ret = h455tax01_panel_disable(&h455tax01_panel->base);
	if (ret < 0)
		dev_err(dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(dev, "Cannot detach from DSI host: %d\n", ret);

	h455tax01_panel_del(h455tax01_panel);

	return 0;
}

static void h455tax01_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct h455tax01_panel *h455tax01_panel = mipi_dsi_get_drvdata(dsi);

	h455tax01_panel_disable(&h455tax01_panel->base);
}

static struct mipi_dsi_driver h455tax01_panel_driver = {
	.driver = {
		.name = "panel-jdi-syn-h455tax01",
		.of_match_table = h455tax01_of_match,
	},
	.probe = h455tax01_panel_probe,
	.remove = h455tax01_panel_remove,
	.shutdown = h455tax01_panel_shutdown,
};
module_mipi_dsi_driver(h455tax01_panel_driver);

MODULE_AUTHOR("AngeloGioacchino Del Regno <kholk11@gmail.com>");
MODULE_DESCRIPTION("JDI Synaptics H455TAX01 In-Cell IPS LCD");
MODULE_LICENSE("GPL v2");
