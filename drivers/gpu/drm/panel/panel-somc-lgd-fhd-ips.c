/*
 * Copyright (C) 2019 AngeloGioacchino Del Regno <kholk11@gmail.com>
 *
 * from DT configurations on Sony Xperia Tone platform
 * LG xxxx IPS LCD Panel Driver -- TODO: RETRIEVE PANEL MODEL!!!
 *
 * Copyright (c) 2016 Sony Mobile Communications Inc.
 * Parameters from dsi-panel-somc-synaptics-lgd-1080p-cmd.dtsi
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

struct lgd_fhd_ips_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct backlight_device *backlight;

	struct regulator *vddio_supply;
	struct regulator *avdd_supply;
	struct regulator *pvddio_supply;
	struct regulator *tvddio_supply;

	struct gpio_desc *pan_reset_gpio;
	struct gpio_desc *ts_reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static const u8 cmd_on_unk1[2] = {0xb0, 0x04};
static const u8 cmd_on_unk2[2] = {0xd6, 0x01};

static const u8 cmd_on_unk3[32] =
	{
		0xC1, 0x84, 0x00, 0x10, 0xF0, 0x47, 0xF9, 0xFF,
		0xAF, 0xFF, 0xAF, 0xCF, 0x9A, 0x73, 0x8D, 0xFD,
		0xF5, 0x7F, 0xFD, 0xFF, 0x0F, 0xF1, 0x1F, 0x00,
		0xAA, 0x40, 0x02, 0xC2, 0x11, 0x08, 0x00, 0x01,
	};

static const u8 cmd_on_unk4[10] =
	{
		0xCB, 0x8D, 0xF4, 0x4B, 0x2C, 0x00, 0x04, 0x08,
		0x00, 0x00,
	};


static inline struct lgd_fhd_ips_panel *to_lgd_fhd_ips(struct drm_panel *panel)
{
	return container_of(panel, struct lgd_fhd_ips_panel, base);
}

static int lgd_fhd_ips_panel_enable(struct drm_panel *panel)
{
	struct lgd_fhd_ips_panel *lgd_panel = to_lgd_fhd_ips(panel);

	if (lgd_panel->enabled)
		return 0;

	lgd_panel->enabled = true;

	return 0;
}

static int lgd_fhd_ips_panel_init(struct lgd_fhd_ips_panel *lgd_panel)
{
	struct device *dev = &lgd_panel->dsi->dev;
	ssize_t wr_sz = 0;
	int rc = 0;

	lgd_panel->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	wr_sz = mipi_dsi_generic_write(lgd_panel->dsi,
					cmd_on_unk1, sizeof(cmd_on_unk1));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 1: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(lgd_panel->dsi,
					cmd_on_unk2, sizeof(cmd_on_unk2));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 2: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(lgd_panel->dsi,
					cmd_on_unk3, sizeof(cmd_on_unk3));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 3: %ld\n", wr_sz);

	wr_sz = mipi_dsi_generic_write(lgd_panel->dsi,
					cmd_on_unk4, sizeof(cmd_on_unk4));
	if (wr_sz < 0)
		dev_err(dev, "Cannot send ON command 4: %ld\n", wr_sz);

	rc = mipi_dsi_dcs_exit_sleep_mode(lgd_panel->dsi);
	if (rc < 0) {
		dev_err(dev, "Cannot send exit sleep cmd: %d\n", rc);
		return rc;
	}

	msleep(120);

	return rc;
}

static int lgd_fhd_ips_panel_on(struct lgd_fhd_ips_panel *lgd_panel)
{
	struct device *dev = &lgd_panel->dsi->dev;
	int rc = 0;

	rc = mipi_dsi_dcs_set_display_on(lgd_panel->dsi);
	if (rc < 0) {
		dev_err(dev, "Cannot send disp on cmd: %d\n", rc);
		return rc;
	}

	msleep(120);

	return rc;
}

static int lgd_fhd_ips_panel_disable(struct drm_panel *panel)
{
	struct lgd_fhd_ips_panel *lgd_panel = to_lgd_fhd_ips(panel);

	if (!lgd_panel->enabled)
		return 0;

	lgd_panel->enabled = false;

	return 0;
}

static int lgd_fhd_ips_panel_off(struct lgd_fhd_ips_panel *lgd_panel)
{
	struct device *dev = &lgd_panel->dsi->dev;
	int rc;

	lgd_panel->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	rc = mipi_dsi_dcs_set_display_off(lgd_panel->dsi);
	if (rc < 0)
		dev_err(dev, "Cannot set display off: %d\n", rc);

	rc = mipi_dsi_dcs_enter_sleep_mode(lgd_panel->dsi);
	if (rc < 0)
		dev_err(dev, "Cannot enter sleep mode: %d\n", rc);

	msleep(100);

	return rc;
}

static int lgd_fhd_ips_panel_unprepare(struct drm_panel *panel)
{
	struct lgd_fhd_ips_panel *lgd_panel = to_lgd_fhd_ips(panel);
	int rc = 0;

	if (!lgd_panel->prepared)
		return 0;
#ifdef MDSS_BUG_SOLVED
	if (lgd_panel->ts_reset_gpio) {
		gpiod_set_value(lgd_panel->ts_reset_gpio, 0);
		usleep_range(10000, 11000);
	}
#endif
	lgd_fhd_ips_panel_off(lgd_panel);

	/* TODO: LAB/IBB */
#ifdef MDSS_BUG_SOLVED
	regulator_disable(lgd_panel->avdd_supply);
	regulator_disable(lgd_panel->vddio_supply);

	if (lgd_panel->pan_reset_gpio) {
		gpiod_set_value(lgd_panel->pan_reset_gpio, 0);
		usleep_range(10000, 11000);
	}

	if (lgd_panel->pvddio_supply)
		regulator_disable(lgd_panel->pvddio_supply);
#endif
	lgd_panel->prepared = false;

	return rc;
}

static int lgd_fhd_ips_panel_prepare(struct drm_panel *panel)
{
	struct lgd_fhd_ips_panel *lgd_panel = to_lgd_fhd_ips(panel);
	struct device *dev = &lgd_panel->dsi->dev;
	int rc;

	if (lgd_panel->prepared)
		return 0;

	/* Power rail VDDIO => in-cell panel main */
	rc = regulator_enable(lgd_panel->vddio_supply);
	if (rc < 0)
		return rc;

	msleep(80);

	/* Power rail AVDD => in-cell touch-controller main */
	rc = regulator_enable(lgd_panel->avdd_supply);
	if (rc < 0)
		dev_err(dev, "Cannot enable AVDD: %d\n", rc);
	else
		usleep_range(1000, 1100);

	/* TODO: LAB/IBB */

	/* Enable the in-cell supply to panel */
	if (lgd_panel->pvddio_supply)
		rc = regulator_enable(lgd_panel->pvddio_supply);
	if (rc) {
		dev_err(dev, "Cannot enable pvddio: %d", rc);
		goto poweroff_s1;
	}
	usleep_range(1000, 1100);

	/* Enable the in-cell supply to touch-controller */
	rc = regulator_enable(lgd_panel->tvddio_supply);
	if (rc) {
		dev_err(dev, "Cannot enable TVDDIO: %d", rc);
		goto poweroff_s2;
	}
	usleep_range(1000, 1100);

	if (lgd_panel->ts_reset_gpio)
		gpiod_set_value(lgd_panel->ts_reset_gpio, 1);

#ifdef MDSS_BUG_SOLVED
	if (lgd_panel->pan_reset_gpio) {
		gpiod_set_value(lgd_panel->pan_reset_gpio, 0);
		usleep_range(10000, 10000);
		gpiod_set_value(lgd_panel->pan_reset_gpio, 1);
		usleep_range(10000, 11000);
	};
#endif

	rc = lgd_fhd_ips_panel_init(lgd_panel);
	if (rc < 0) {
		dev_err(dev, "Cannot initialize panel: %d\n", rc);
		goto poweroff_s2;
	}

	rc = lgd_fhd_ips_panel_on(lgd_panel);
	if (rc < 0) {
		dev_err(dev, "Cannot poweron panel: %d\n", rc);
		goto poweroff_s2;
	}

	lgd_panel->prepared = true;

	return 0;

poweroff_s2:
	/* Disable it to avoid current/voltage spikes in the enable path */
	if (lgd_panel->pvddio_supply)
		regulator_disable(lgd_panel->pvddio_supply);
	// TODO: TVDDIO not disabled!

poweroff_s1:
	regulator_disable(lgd_panel->avdd_supply);
	regulator_disable(lgd_panel->vddio_supply);

	return rc;
}

static const struct drm_display_mode default_mode = {
	//.clock = 299013,
	.clock = 149506,
	.hdisplay = 1080,
	.hsync_start = 1080 + 56,
	.hsync_end = 1080 + 56 + 8,
	.htotal = 1080 + 56 + 8 + 8,
	.vdisplay = 1920,
	.vsync_start = 1920 + 227,
	.vsync_end = 1920 + 227 + 8,
	.vtotal = 1920 + 227 + 8 + 8,
	//.vrefresh = 120,
	.vrefresh = 60,
	//.flags = 0,
};

static int lgd_fhd_ips_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct lgd_fhd_ips_panel *lgd_panel = to_lgd_fhd_ips(panel);
	struct device *dev = &lgd_panel->dsi->dev;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 61;
	panel->connector->display_info.height_mm = 110;

	return 1;
}

static const struct drm_panel_funcs lgd_fhd_ips_panel_funcs = {
	.disable = lgd_fhd_ips_panel_disable,
	.unprepare = lgd_fhd_ips_panel_unprepare,
	.prepare = lgd_fhd_ips_panel_prepare,
	.enable = lgd_fhd_ips_panel_enable,
	.get_modes = lgd_fhd_ips_panel_get_modes,
};

static const struct of_device_id lgd_fhd_ips_of_match[] = {
	{ .compatible = "lgd,syn-incell-fhd-ips-lcd", },
	{ }
};
MODULE_DEVICE_TABLE(of, lgd_fhd_ips_of_match);

static int lgd_fhd_ips_panel_add(struct lgd_fhd_ips_panel *lgd_panel)
{
	struct device *dev = &lgd_panel->dsi->dev;
	int rc;

	lgd_panel->mode = &default_mode;

	lgd_panel->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(lgd_panel->vddio_supply)) {
		dev_err(dev, "cannot get vddio regulator: %ld\n",
			PTR_ERR(lgd_panel->vddio_supply));
		return PTR_ERR(lgd_panel->vddio_supply);
	}

	lgd_panel->avdd_supply = devm_regulator_get_optional(dev, "avdd");
	if (IS_ERR(lgd_panel->avdd_supply)) {
		dev_err(dev, "cannot get avdd regulator: %ld\n",
			PTR_ERR(lgd_panel->avdd_supply));
		lgd_panel->avdd_supply = NULL;
	}

	lgd_panel->pvddio_supply = devm_regulator_get_optional(dev, "pvddio");
	if (IS_ERR(lgd_panel->pvddio_supply)) {
		dev_err(dev, "cannot get pvddio regulator: %ld\n",
			PTR_ERR(lgd_panel->pvddio_supply));
		lgd_panel->pvddio_supply = NULL;
	}

	lgd_panel->tvddio_supply = devm_regulator_get_optional(dev, "tvddio");
	if (IS_ERR(lgd_panel->tvddio_supply)) {
		dev_err(dev, "cannot get tvddio regulator: %ld\n",
			PTR_ERR(lgd_panel->tvddio_supply));
		lgd_panel->tvddio_supply = NULL;
	}

	lgd_panel->pan_reset_gpio = devm_gpiod_get(dev,
					"preset", GPIOD_ASIS);
	if (IS_ERR(lgd_panel->pan_reset_gpio)) {
		dev_err(dev, "cannot get preset-gpio: %ld\n",
			PTR_ERR(lgd_panel->pan_reset_gpio));
		lgd_panel->pan_reset_gpio = NULL;
	}

	lgd_panel->ts_reset_gpio = devm_gpiod_get(dev,
					"treset", GPIOD_ASIS);
	if (IS_ERR(lgd_panel->ts_reset_gpio)) {
		dev_err(dev, "cannot get treset-gpio: %ld\n",
			PTR_ERR(lgd_panel->ts_reset_gpio));
		lgd_panel->ts_reset_gpio = NULL;
	}

	drm_panel_init(&lgd_panel->base);
	lgd_panel->base.funcs = &lgd_fhd_ips_panel_funcs;
	lgd_panel->base.dev = dev;

	rc = drm_panel_add(&lgd_panel->base);
	if (rc < 0)
		pr_err("drm panel add failed\n");

	return rc;
}

static void lgd_fhd_ips_panel_del(struct lgd_fhd_ips_panel *lgd_panel)
{
	if (lgd_panel->base.dev)
		drm_panel_remove(&lgd_panel->base);
}

static int lgd_fhd_ips_panel_probe(struct mipi_dsi_device *dsi)
{
	struct lgd_fhd_ips_panel *lgd_panel;
	int rc;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	lgd_panel = devm_kzalloc(&dsi->dev, sizeof(*lgd_panel), GFP_KERNEL);
	if (!lgd_panel)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, lgd_panel);
	lgd_panel->dsi = dsi;

	rc = lgd_fhd_ips_panel_add(lgd_panel);
	if (rc < 0)
		return rc;

	return mipi_dsi_attach(dsi);
}

static int lgd_fhd_ips_panel_remove(struct mipi_dsi_device *dsi)
{
	struct lgd_fhd_ips_panel *lgd_panel = mipi_dsi_get_drvdata(dsi);
	struct device *dev = &lgd_panel->dsi->dev;
	int ret;

	ret = lgd_fhd_ips_panel_disable(&lgd_panel->base);
	if (ret < 0)
		dev_err(dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(dev, "Cannot detach from DSI host: %d\n", ret);

	lgd_fhd_ips_panel_del(lgd_panel);

	return 0;
}

static void lgd_fhd_ips_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct lgd_fhd_ips_panel *lgd_panel = mipi_dsi_get_drvdata(dsi);

	lgd_fhd_ips_panel_disable(&lgd_panel->base);
}

static struct mipi_dsi_driver lgd_fhd_ips_panel_driver = {
	.driver = {
		.name = "panel-lgd-fhd-ips",
		.of_match_table = lgd_fhd_ips_of_match,
	},
	.probe = lgd_fhd_ips_panel_probe,
	.remove = lgd_fhd_ips_panel_remove,
	.shutdown = lgd_fhd_ips_panel_shutdown,
};
module_mipi_dsi_driver(lgd_fhd_ips_panel_driver);

MODULE_AUTHOR("AngeloGioacchino Del Regno <kholk11@gmail.com>");
MODULE_DESCRIPTION("LGD FullHD IPS MIPI LCD");
MODULE_LICENSE("GPL v2");
