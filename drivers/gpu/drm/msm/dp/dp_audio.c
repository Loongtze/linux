// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 */


#define pr_fmt(fmt)	"[drm-dp] %s: " fmt, __func__

#include <linux/platform_device.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_edid.h>

#include "dp_catalog.h"
#include "dp_audio.h"
#include "dp_drm.h"
#include "dp_panel.h"
#include "dp_reg.h"
#include "dp_display.h"
#include "dp_utils.h"

struct msm_dp_audio_private {
	struct platform_device *pdev;
	struct drm_device *drm_dev;
	struct msm_dp_catalog *catalog;

	u32 channels;

	struct msm_dp_audio msm_dp_audio;
};

static void msm_dp_audio_stream_sdp(struct msm_dp_audio_private *audio)
{
	struct dp_sdp_header sdp_hdr = {
		.HB0 = 0x00,
		.HB1 = 0x02,
		.HB2 = 0x00,
		.HB3 = audio->channels - 1,
	};

	msm_dp_catalog_write_audio_stream(audio->catalog, &sdp_hdr);
}

static void msm_dp_audio_timestamp_sdp(struct msm_dp_audio_private *audio)
{
	struct dp_sdp_header sdp_hdr = {
		.HB0 = 0x00,
		.HB1 = 0x01,
		.HB2 = 0x17,
		.HB3 = 0x0 | (0x11 << 2),
	};

	msm_dp_catalog_write_audio_timestamp(audio->catalog, &sdp_hdr);
}

static void msm_dp_audio_infoframe_sdp(struct msm_dp_audio_private *audio)
{
	struct dp_sdp_header sdp_hdr = {
		.HB0 = 0x00,
		.HB1 = 0x84,
		.HB2 = 0x1b,
		.HB3 = 0x0 | (0x11 << 2),
	};

	msm_dp_catalog_write_audio_infoframe(audio->catalog, &sdp_hdr);
}

static void msm_dp_audio_copy_management_sdp(struct msm_dp_audio_private *audio)
{
	struct dp_sdp_header sdp_hdr = {
		.HB0 = 0x00,
		.HB1 = 0x05,
		.HB2 = 0x0f,
		.HB3 = 0x00,
	};

	msm_dp_catalog_write_audio_copy_mgmt(audio->catalog, &sdp_hdr);
}

static void msm_dp_audio_isrc_sdp(struct msm_dp_audio_private *audio)
{
	struct dp_sdp_header sdp_hdr = {
		.HB0 = 0x00,
		.HB1 = 0x06,
		.HB2 = 0x0f,
		.HB3 = 0x00,
	};

	msm_dp_catalog_write_audio_isrc(audio->catalog, &sdp_hdr);
}

static void msm_dp_audio_setup_sdp(struct msm_dp_audio_private *audio)
{
	msm_dp_catalog_audio_config_sdp(audio->catalog);

	msm_dp_audio_stream_sdp(audio);
	msm_dp_audio_timestamp_sdp(audio);
	msm_dp_audio_infoframe_sdp(audio);
	msm_dp_audio_copy_management_sdp(audio);
	msm_dp_audio_isrc_sdp(audio);
}

static void msm_dp_audio_setup_acr(struct msm_dp_audio_private *audio)
{
	u32 select = 0;
	struct msm_dp_catalog *catalog = audio->catalog;

	switch (audio->msm_dp_audio.bw_code) {
	case DP_LINK_BW_1_62:
		select = 0;
		break;
	case DP_LINK_BW_2_7:
		select = 1;
		break;
	case DP_LINK_BW_5_4:
		select = 2;
		break;
	case DP_LINK_BW_8_1:
		select = 3;
		break;
	default:
		drm_dbg_dp(audio->drm_dev, "Unknown link rate\n");
		select = 0;
		break;
	}

	msm_dp_catalog_audio_config_acr(catalog, select);
}

static void msm_dp_audio_safe_to_exit_level(struct msm_dp_audio_private *audio)
{
	struct msm_dp_catalog *catalog = audio->catalog;
	u32 safe_to_exit_level = 0;

	switch (audio->msm_dp_audio.lane_count) {
	case 1:
		safe_to_exit_level = 14;
		break;
	case 2:
		safe_to_exit_level = 8;
		break;
	case 4:
		safe_to_exit_level = 5;
		break;
	default:
		safe_to_exit_level = 14;
		drm_dbg_dp(audio->drm_dev,
				"setting the default safe_to_exit_level = %u\n",
				safe_to_exit_level);
		break;
	}

	msm_dp_catalog_audio_sfe_level(catalog, safe_to_exit_level);
}

static void msm_dp_audio_enable(struct msm_dp_audio_private *audio, bool enable)
{
	struct msm_dp_catalog *catalog = audio->catalog;

	msm_dp_catalog_audio_enable(catalog, enable);
}

static struct msm_dp_audio_private *msm_dp_audio_get_data(struct msm_dp *msm_dp_display)
{
	struct msm_dp_audio *msm_dp_audio;

	msm_dp_audio = msm_dp_display->msm_dp_audio;
	if (!msm_dp_audio) {
		DRM_ERROR("invalid msm_dp_audio data\n");
		return ERR_PTR(-EINVAL);
	}

	return container_of(msm_dp_audio, struct msm_dp_audio_private, msm_dp_audio);
}

int msm_dp_audio_prepare(struct drm_connector *connector,
			 struct drm_bridge *bridge,
			 struct hdmi_codec_daifmt *daifmt,
			 struct hdmi_codec_params *params)
{
	int rc = 0;
	struct msm_dp_audio_private *audio;
	struct msm_dp *msm_dp_display;

	msm_dp_display = to_dp_bridge(bridge)->msm_dp_display;

	/*
	 * there could be cases where sound card can be opened even
	 * before OR even when DP is not connected . This can cause
	 * unclocked access as the audio subsystem relies on the DP
	 * driver to maintain the correct state of clocks. To protect
	 * such cases check for connection status and bail out if not
	 * connected.
	 */
	if (!msm_dp_display->power_on) {
		rc = -EINVAL;
		goto end;
	}

	audio = msm_dp_audio_get_data(msm_dp_display);
	if (IS_ERR(audio)) {
		rc = PTR_ERR(audio);
		goto end;
	}

	audio->channels = params->channels;

	msm_dp_audio_setup_sdp(audio);
	msm_dp_audio_setup_acr(audio);
	msm_dp_audio_safe_to_exit_level(audio);
	msm_dp_audio_enable(audio, true);
	msm_dp_display_signal_audio_start(msm_dp_display);
	msm_dp_display->audio_enabled = true;

end:
	return rc;
}

void msm_dp_audio_shutdown(struct drm_connector *connector,
			   struct drm_bridge *bridge)
{
	struct msm_dp_audio_private *audio;
	struct msm_dp *msm_dp_display;

	msm_dp_display = to_dp_bridge(bridge)->msm_dp_display;
	audio = msm_dp_audio_get_data(msm_dp_display);
	if (IS_ERR(audio)) {
		DRM_ERROR("failed to get audio data\n");
		return;
	}

	/*
	 * if audio was not enabled there is no need
	 * to execute the shutdown and we can bail out early.
	 * This also makes sure that we dont cause an unclocked
	 * access when audio subsystem calls this without DP being
	 * connected. is_connected cannot be used here as its set
	 * to false earlier than this call
	 */
	if (!msm_dp_display->audio_enabled)
		return;

	msm_dp_audio_enable(audio, false);
	/* signal the dp display to safely shutdown clocks */
	msm_dp_display_signal_audio_complete(msm_dp_display);
}

struct msm_dp_audio *msm_dp_audio_get(struct platform_device *pdev,
			struct msm_dp_catalog *catalog)
{
	int rc = 0;
	struct msm_dp_audio_private *audio;
	struct msm_dp_audio *msm_dp_audio;

	if (!pdev || !catalog) {
		DRM_ERROR("invalid input\n");
		rc = -EINVAL;
		goto error;
	}

	audio = devm_kzalloc(&pdev->dev, sizeof(*audio), GFP_KERNEL);
	if (!audio) {
		rc = -ENOMEM;
		goto error;
	}

	audio->pdev = pdev;
	audio->catalog = catalog;

	msm_dp_audio = &audio->msm_dp_audio;

	return msm_dp_audio;
error:
	return ERR_PTR(rc);
}

void msm_dp_audio_put(struct msm_dp_audio *msm_dp_audio)
{
	struct msm_dp_audio_private *audio;

	if (!msm_dp_audio)
		return;

	audio = container_of(msm_dp_audio, struct msm_dp_audio_private, msm_dp_audio);

	devm_kfree(&audio->pdev->dev, audio);
}
