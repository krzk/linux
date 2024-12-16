// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2024 Linaro Limited

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

#define CMD_MODE		0
#define CMD_MODE_FHD		0
#define VIDEO_MODE		1
#define VIDEO_MODE_WITH_DSC	1

struct novatek_nt37801 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
};

static const struct regulator_bulk_data novatek_nt37801_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vci" },
	{ .supply = "vdd" },
};

static inline struct novatek_nt37801 *to_novatek_nt37801(struct drm_panel *panel)
{
	return container_of(panel, struct novatek_nt37801, panel);
}

static void novatek_nt37801_reset(struct novatek_nt37801 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 21000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 21000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 21000);
	pr_err("AAA %s:%d\n", __func__, __LINE__);
}

static int novatek_nt37801_on(struct novatek_nt37801 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

#if CMD_MODE
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x0b, 0x0b, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x00);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x059f);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0c7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
				     0x89, 0x28, 0x00, 0x28, 0xc2, 0x00, 0x02,
				     0x68, 0x04, 0x6c, 0x00, 0x0a, 0x02, 0x77,
				     0x01, 0xe9, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb,
				     0x00, 0x01, 0x00, 0x11, 0x33, 0x33, 0x33,
				     0x55, 0x57, 0xd0, 0x00, 0x00, 0x44, 0x56,
				     0x77, 0x78, 0x9a, 0xbc, 0xdd, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3, 0xdc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x00, 0x18, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51,
				     0x07, 0xff, 0x07, 0xff, 0x0f, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9c, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_MEMORY_START);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2, 0x55, 0x01, 0xff, 0x03);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
#endif
#if CMD_MODE_FHD
	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x0b, 0x0b, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x00);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x0437);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x095f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
				     0x89, 0x28, 0x00, 0x28, 0xc2, 0x00, 0x02,
				     0x0e, 0x03, 0xdd, 0x00, 0x07, 0x02, 0x77,
				     0x02, 0x8b, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb,
				     0x00, 0x01, 0x00, 0x11, 0x33, 0x33, 0x33,
				     0x55, 0x57, 0xd0, 0x00, 0x00, 0x44, 0x56,
				     0x77, 0x78, 0x9a, 0xbc, 0xdd, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x93,
				     0x89, 0x28, 0x00, 0x28, 0xd2, 0x00, 0x02,
				     0x25, 0x03, 0xb6, 0x00, 0x07, 0x02, 0xab,
				     0x02, 0x8b, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x95,
				     0x89, 0x28, 0x00, 0x28, 0xd2, 0x00, 0x01,
				     0xc3, 0x02, 0xfc, 0x00, 0x05, 0x02, 0xab,
				     0x03, 0xd1, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x03, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3, 0xdc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x00, 0x18, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51,
				     0x07, 0xff, 0x07, 0xff, 0x0f, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9c, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_MEMORY_START);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
#endif
#if VIDEO_MODE
	pr_err("AAA %s:%d\n", __func__, __LINE__);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc6, 0xa2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xec,
				     0x10, 0x00, 0x00, 0x00, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x17, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x00, 0x14, 0x00, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x0b, 0x0b, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x00);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x059f);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0c7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
				     0x89, 0x28, 0x00, 0x28, 0xc2, 0x00, 0x02,
				     0x68, 0x04, 0x6c, 0x00, 0x0a, 0x02, 0x77,
				     0x01, 0xe9, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb,
				     0x00, 0x01, 0x00, 0x11, 0x33, 0x33, 0x33,
				     0x55, 0x57, 0xd0, 0x00, 0x00, 0x44, 0x56,
				     0x77, 0x78, 0x9a, 0xbc, 0xdd, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3, 0xdc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51,
				     0x07, 0xff, 0x07, 0xff, 0x0f, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9c, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_MEMORY_START);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2, 0x55, 0x01, 0xff, 0x03);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
#endif

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	return dsi_ctx.accum_err;
}

static int novatek_nt37801_off(struct novatek_nt37801 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };


#if CMD_MODE
	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	pr_err("AAA %s:%d\n", __func__, __LINE__);
#endif
#if CMD_MODE_FHD
	pr_err("AAA %s:%d\n", __func__, __LINE__);
	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
#endif
#if VIDEO_MODE
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	pr_err("AAA %s:%d\n", __func__, __LINE__);
#endif
	return dsi_ctx.accum_err;
}

static int novatek_nt37801_prepare(struct drm_panel *panel)
{
	struct novatek_nt37801 *ctx = to_novatek_nt37801(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	ret = regulator_bulk_enable(ARRAY_SIZE(novatek_nt37801_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	novatek_nt37801_reset(ctx);

	ret = novatek_nt37801_on(ctx);
	if (ret < 0)
		goto err;

	pr_err("AAA %s:%d\n", __func__, __LINE__);

#if CMD_MODE || CMD_MODE_FHD || VIDEO_MODE_WITH_DSC
	pr_err("AAA %s:%d\n", __func__, __LINE__);
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		goto err;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		goto err;
	}
#endif

	msleep(28); /* TODO: Is this panel-dependent? */

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	return 0;

err:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(novatek_nt37801_supplies),
			       ctx->supplies);

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	return ret;
}

static int novatek_nt37801_unprepare(struct drm_panel *panel)
{
	struct novatek_nt37801 *ctx = to_novatek_nt37801(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = novatek_nt37801_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_bulk_disable(ARRAY_SIZE(novatek_nt37801_supplies),
			       ctx->supplies);

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	return 0;
}

#if CMD_MODE
static const struct drm_display_mode novatek_nt37801_mode = {
	.clock = (1440 + 20 + 4 + 20) * (3200 + 20 + 2 + 18) * 120 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 20,
	.hsync_end = 1440 + 20 + 4,
	.htotal = 1440 + 20 + 4 + 20,
	.vdisplay = 3200,
	.vsync_start = 3200 + 20,
	.vsync_end = 3200 + 20 + 2,
	.vtotal = 3200 + 20 + 2 + 18,
	.width_mm = 0,
	.height_mm = 0,
	.type = DRM_MODE_TYPE_DRIVER,
};
#endif
#if CMD_MODE_FHD
static const struct drm_display_mode novatek_nt37801_mode = {
	.clock = (1080 + 20 + 4 + 20) * (2400 + 20 + 2 + 18) * 120 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 20,
	.hsync_end = 1080 + 20 + 4,
	.htotal = 1080 + 20 + 4 + 20,
	.vdisplay = 2400,
	.vsync_start = 2400 + 20,
	.vsync_end = 2400 + 20 + 2,
	.vtotal = 2400 + 20 + 2 + 18,
	.width_mm = 0,
	.height_mm = 0,
	.type = DRM_MODE_TYPE_DRIVER,
};
#endif
#if VIDEO_MODE
static const struct drm_display_mode novatek_nt37801_mode = {
	.clock = (1440 + 100 + 20 + 20) * (3200 + 44 + 2 + 20) * 120 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 100,
	.hsync_end = 1440 + 100 + 20,
	.htotal = 1440 + 100 + 20 + 20,
	.vdisplay = 3200,
	.vsync_start = 3200 + 44,
	.vsync_end = 3200 + 44 + 2,
	.vtotal = 3200 + 44 + 2 + 20,
	//.width_mm = 0,
	//.height_mm = 0,
	.width_mm = 71,
	.height_mm = 157,
	//.type = DRM_MODE_TYPE_DRIVER,
};
#endif

static int novatek_nt37801_get_modes(struct drm_panel *panel,
				       struct drm_connector *connector)
{
#if CMD_MODE
	return drm_connector_helper_get_modes_fixed(connector,
						    &novatek_nt37801_mode);
#endif
#if CMD_MODE_FHD
	return drm_connector_helper_get_modes_fixed(connector,
						    &novatek_nt37801_mode);
#endif
#if VIDEO_MODE
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &novatek_nt37801_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	pr_err("AAA %s:%d %s - %dx%d mm\n", __func__, __LINE__,
	       mode->name,
	       mode->width_mm, mode->height_mm);
	drm_mode_probed_add(connector, mode);

	return 1;
#endif
}

static const struct drm_panel_funcs novatek_nt37801_panel_funcs = {
	.prepare = novatek_nt37801_prepare,
	.unprepare = novatek_nt37801_unprepare,
	.get_modes = novatek_nt37801_get_modes,
};

static int novatek_nt37801_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	return 0;
}

static const struct backlight_ops novatek_nt37801_bl_ops = {
	.update_status = novatek_nt37801_bl_update_status,
};

static struct backlight_device *
novatek_nt37801_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 4095,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &novatek_nt37801_bl_ops, &props);
}

static int novatek_nt37801_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct novatek_nt37801 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(novatek_nt37801_supplies),
					    novatek_nt37801_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
#if CMD_MODE
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_CLOCK_NON_CONTINUOUS;
#endif
#if CMD_MODE_FHD
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;
#endif
#if VIDEO_MODE
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;
#endif

	drm_panel_init(&ctx->panel, dev, &novatek_nt37801_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = novatek_nt37801_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

#if CMD_MODE
	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 1 */
	ctx->dsc.slice_height = 40;
	ctx->dsc.slice_width = 720;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(1440 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1440 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;
	pr_err("AAA %s:%d\n", __func__, __LINE__);
#endif
#if CMD_MODE_FHD
	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 1 */
	ctx->dsc.slice_height = 40;
	ctx->dsc.slice_width = 540;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(1080 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1080 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;
	pr_err("AAA %s:%d\n", __func__, __LINE__);
#endif
#if VIDEO_MODE_WITH_DSC
	pr_err("AAA %s:%d\n", __func__, __LINE__);
	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 1 */
	ctx->dsc.slice_height = 40;
	ctx->dsc.slice_width = 720;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(1440 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1440 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;
#endif

	pr_err("AAA %s:%d\n", __func__, __LINE__);
	ret = mipi_dsi_attach(dsi);
	pr_err("AAA %s:%d ret=%d\n", __func__, __LINE__, ret);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}
	pr_err("AAA %s:%d\n", __func__, __LINE__);

	return 0;
}

static void novatek_nt37801_remove(struct mipi_dsi_device *dsi)
{
	struct novatek_nt37801 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id novatek_nt37801_of_match[] = {
	{ .compatible = "novatek,nt37801" },
	{}
};
MODULE_DEVICE_TABLE(of, novatek_nt37801_of_match);

static struct mipi_dsi_driver novatek_nt37801_driver = {
	.probe = novatek_nt37801_probe,
	.remove = novatek_nt37801_remove,
	.driver = {
		.name = "panel-novatek-nt37801",
		.of_match_table = novatek_nt37801_of_match,
	},
};
module_mipi_dsi_driver(novatek_nt37801_driver);

MODULE_AUTHOR("Krzysztof Kozlowski <krzysztof.kozlowski@linaro.org>");
MODULE_DESCRIPTION("Panel driver for the Novatek NT37801/NT37810 AMOLED DSI panel");
MODULE_LICENSE("GPL");
