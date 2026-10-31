// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <linux/workqueue.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "qdsp6/q6apm.h"
#include "qdsp6/q6prm.h"
#include "qdsp6/q6dsp-common.h"
#include "common.h"
#include "sdw.h"

#define I2S_MCLKFS 256

#define I2S_MCLK_RATE(rate) \
	((rate) * (I2S_MCLKFS))
#define I2S_BIT_RATE(rate, channels, format) \
	((rate) * (channels) * (format))

static struct snd_soc_dapm_widget sc8280xp_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_SPK("DP0 Jack", NULL),
	SND_SOC_DAPM_SPK("DP1 Jack", NULL),
	SND_SOC_DAPM_SPK("DP2 Jack", NULL),
	SND_SOC_DAPM_SPK("DP3 Jack", NULL),
	SND_SOC_DAPM_SPK("DP4 Jack", NULL),
	SND_SOC_DAPM_SPK("DP5 Jack", NULL),
	SND_SOC_DAPM_SPK("DP6 Jack", NULL),
	SND_SOC_DAPM_SPK("DP7 Jack", NULL),
};

static const struct snd_kcontrol_new max98090_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headset Mic12"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic56"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Receiver"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static const struct snd_soc_dapm_widget max98090_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic12", NULL),
	SND_SOC_DAPM_MIC("Headset Mic56", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Receiver", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

struct qcom_snd_soc_common {
	const char *driver_name;
	const struct snd_soc_dapm_widget *dapm_widgets;
	int num_dapm_widgets;
	const struct snd_soc_dapm_route *dapm_routes;
	int num_dapm_routes;
	const struct snd_kcontrol_new *controls;
	int num_controls;
	unsigned int codec_dai_fmt;
	bool codec_sysclk_set;
	bool mi2s_mclk_enable;
	bool mi2s_bclk_enable;
	bool wcd_jack;
	int (*snd_prepare)(struct snd_pcm_substream *substream);
};

struct sc8280xp_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	const struct qcom_snd_soc_common *priv;
	bool jack_setup;
};

static inline int sc8280xp_get_mclk_freq(struct snd_pcm_hw_params *params)
{
	int rate = params_rate(params);

	switch (rate) {
	case 11025:
	case 44100:
	case 88200:
		return I2S_MCLK_RATE(44100);
	default:
		break;
	}

	return I2S_MCLK_RATE(rate);
}

static inline int sc8280xp_get_bclk_freq(struct snd_pcm_hw_params *params)
{
	return I2S_BIT_RATE(params_rate(params),
			    params_channels(params),
			    snd_pcm_format_width(params_format(params)));
}

static int sc8280xp_tdm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	struct qcom_snd_tdm_slot_cfg cpu_cfg;
	struct qcom_snd_tdm_slot_cfg codec_cfg;
	int bclk_freq;
	int ret;
	int i;

	ret = qcom_snd_get_dai_tdm_slots(rtd, &cpu_cfg, &codec_cfg);
	if (ret)
		return ret == -ENOENT ? 0 : ret;

	if (!cpu_cfg.slots)
		return 0;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
	if (ret && ret != -ENOTSUPP)
		return ret;

	if (data->priv->codec_dai_fmt) {
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_fmt(codec_dai,
						  data->priv->codec_dai_fmt);
			if (ret && ret != -ENOTSUPP)
				return ret;
		}
	}

	ret = qcom_snd_apply_dai_tdm_slots_cfg(rtd, &cpu_cfg, &codec_cfg);
	if (ret)
		return ret;

	bclk_freq = snd_soc_tdm_params_to_bclk(params, cpu_cfg.slot_width, cpu_cfg.slots, 1);
	if (bclk_freq <= 0)
		return -EINVAL;

	if (data->priv->mi2s_bclk_enable) {
		ret = snd_soc_dai_set_sysclk(cpu_dai, LPAIF_MI2S_BCLK, bclk_freq,
					     SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "%s: failed to set cpu sysclk: %d\n",
				__func__, ret);
			return ret;
		}
	}

	if (data->priv->codec_sysclk_set) {
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_sysclk(codec_dai, 0, bclk_freq,
						     SND_SOC_CLOCK_IN);
			if (ret && ret != -ENOTSUPP) {
				dev_err(rtd->dev, "%s: failed to set codec sysclk on %s: %d\n",
					__func__, codec_dai->name, ret);
				return ret;
			}
		}
	}

	return 0;
}

static int sc8280xp_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack  = NULL;
	int dp_pcm_id = 0;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 17);
		break;
	case DISPLAY_PORT_RX_0:
		/* DISPLAY_PORT dai ids are not contiguous */
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	if (data->priv->wcd_jack)
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);

	return 0;
}

static int sc8280xp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);
	channels->min = 2;
	channels->max = 2;
	switch (cpu_dai->id) {
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		channels->min = 1;
		break;
	default:
		break;
	}


	return 0;
}

static int sc8280xp_snd_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	int mclk_freq = sc8280xp_get_mclk_freq(params);
	int bclk_freq = sc8280xp_get_bclk_freq(params);
	int ret, i;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX ... QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX ... QUINARY_MI2S_TX:
	case SENARY_MI2S_RX ... SENARY_MI2S_TX:
	case LPI_MI2S_RX_0 ... LPI_MI2S_TX_4:
		ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
		if (ret && ret != -ENOTSUPP)
			return ret;

		if (data->priv->codec_dai_fmt) {
			ret = snd_soc_dai_set_fmt(codec_dai,
						  data->priv->codec_dai_fmt);
			if (ret && ret != -ENOTSUPP)
				return ret;
		}

		if (data->priv->mi2s_mclk_enable) {
			ret = snd_soc_dai_set_sysclk(cpu_dai,
						     LPAIF_MI2S_MCLK, mclk_freq,
						     SND_SOC_CLOCK_OUT);
			if (ret)
				return ret;
		}

		if (data->priv->mi2s_bclk_enable) {
			ret = snd_soc_dai_set_sysclk(cpu_dai,
						     LPAIF_MI2S_BCLK, bclk_freq,
						     SND_SOC_CLOCK_OUT);
			if (ret)
				return ret;
		}

		if (data->priv->codec_sysclk_set) {
			/*
			 * Every codec on the link needs its PLL reference clock;
			 * a card may instantiate several amps (dada has two, at
			 * 0x40 and 0x42) and only codec[0] would get programmed
			 * otherwise, leaving the second amp's REFCLK_INPUT at its
			 * reset FREQ=0x00 (32768 Hz) with an unlocked PLL.
			 *
			 * The frequency must be the clock that physically reaches
			 * the codec: platforms that route MCLK pass mclk_freq, but
			 * dada has no MCLK pad (secondary_mi2s_default muxes only
			 * sck/ws/data1) and leaves .mi2s_mclk_enable unset, so
			 * both CS35L43 are fed from BCLK instead.
			 */
			for (i = 0; i < rtd->dai_link->num_codecs; i++) {
				ret = snd_soc_dai_set_sysclk(
					snd_soc_rtd_to_codec(rtd, i), 0,
					data->priv->mi2s_mclk_enable ?
							mclk_freq : bclk_freq,
					SND_SOC_CLOCK_IN);
				if (ret && ret != -ENOTSUPP)
					return ret;
			}
		}
		break;
	case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:
		return sc8280xp_tdm_hw_params(substream, params);
	default:
		break;
	}

	return 0;
}

/*
 * WSA and WSA2 are handled as a single interface with the
 * following channels mask:
 *  __________________________________________________
 *  | Bits  |     3    |     2    |   1     |     0   |
 *  ---------------------------------------------------
 *  | Line  | WSA2 Ch2 | WSA2 Ch1 | WSA Ch2 | WSA Ch1 |
 *  ---------------------------------------------------
 *
 * The Ayaneo Pocket S2 speakers are connected only to
 * the WSA2 interface and the WSA interface is not enabled.
 *
 * Set the channel mapping on the WSA2 channels only.
 */
static const unsigned int ayaneo_ps2_channels_mapping[] = {
	0,			/* WSA Ch1 */
	0,			/* WSA Ch2 */
	PCM_CHANNEL_FL,		/* WSA2 Ch1 */
	PCM_CHANNEL_FR		/* WSA2 Ch2 */
};

static int ayaneo_ps2_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int channels = substream->runtime->channels;

	if (cpu_dai->id != WSA_CODEC_DMA_RX_0)
		return 0;

	if (channels != 2)
		return -EINVAL;

	return snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
					   ARRAY_SIZE(ayaneo_ps2_channels_mapping),
					   ayaneo_ps2_channels_mapping);
}

static int sc8280xp_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	if (data->priv->snd_prepare) {
		int ret;

		ret = data->priv->snd_prepare(substream);
		if (ret)
			return ret;
	}

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sc8280xp_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sc8280xp_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_params = sc8280xp_snd_hw_params,
	.hw_free = sc8280xp_snd_hw_free,
	.prepare = sc8280xp_snd_prepare,
};

static void sc8280xp_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sc8280xp_snd_init;
			link->be_hw_params_fixup = sc8280xp_be_hw_params_fixup;
			link->ops = &sc8280xp_be_ops;
		}
	}
}

/*
 * dada: golden mixer defaults captured from the working vendor rootfs
 * (amixer -c0 contents, BYTES-class DSP protection telemetry excluded).
 * Applied from late_probe so raw ALSA (arecord/aplay) works with zero
 * userspace configuration.  Board-guarded: other machines using this
 * driver are unaffected.
 */
#define DADA_CTL_ENUM	0
#define DADA_CTL_INT	1

struct dada_ctl_default {
	const char *name;
	int kind;
	int count;
	long values[2];
};

/* Golden control defaults captured from working vendor rootfs (amixer -c0 contents).
 * Generated; BYTES-class DSP protection telemetry excluded (driver/DSP-managed). */
static const struct dada_ctl_default dada_ctl_defaults[] = {
	{ "ADC1 MUX", DADA_CTL_ENUM, 1, {4} },
	{ "ADC1 Switch", DADA_CTL_INT, 1, {1} },
	{ "ADC1 Volume", DADA_CTL_INT, 1, {20} },
	{ "ADC1_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "ADC2 MUX", DADA_CTL_ENUM, 1, {4} },
	{ "ADC2 Switch", DADA_CTL_INT, 1, {1} },
	{ "ADC2 Volume", DADA_CTL_INT, 1, {20} },
	{ "ADC2_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "ADC3 MUX", DADA_CTL_ENUM, 1, {3} },
	{ "ADC3 Switch", DADA_CTL_INT, 1, {1} },
	{ "ADC3 Volume", DADA_CTL_INT, 1, {0} },
	{ "ADC3_MIXER Switch", DADA_CTL_INT, 1, {0} },
	{ "ADC4 MUX", DADA_CTL_ENUM, 1, {4} },
	{ "ADC4 Switch", DADA_CTL_INT, 1, {1} },
	{ "ADC4 Volume", DADA_CTL_INT, 1, {0} },
	{ "ADC4_MIXER Switch", DADA_CTL_INT, 1, {0} },
	{ "AUX_HPF Switch", DADA_CTL_INT, 1, {0} },
	{ "B AMP Enable Switch", DADA_CTL_INT, 1, {1} },
	{ "B ASP TX1 Source", DADA_CTL_ENUM, 1, {3} },
	{ "B ASP TX2 Source", DADA_CTL_ENUM, 1, {4} },
	{ "B ASP TX3 Source", DADA_CTL_ENUM, 1, {0} },
	{ "B ASP TX4 Source", DADA_CTL_ENUM, 1, {0} },
	{ "B Amp Gain", DADA_CTL_INT, 1, {10} },
	{ "B DC Watchdog Duration", DADA_CTL_INT, 1, {0} },
	{ "B DC Watchdog Enable", DADA_CTL_INT, 1, {0} },
	{ "B DC Watchdog Mode", DADA_CTL_ENUM, 1, {0} },
	{ "B DC Watchdog Threshold", DADA_CTL_INT, 1, {0} },
	{ "B DSP RX1 Source", DADA_CTL_ENUM, 1, {0} },
	{ "B DSP RX2 Source", DADA_CTL_ENUM, 1, {2} },
	{ "B DSP1 Firmware", DADA_CTL_ENUM, 1, {9} },
	{ "B DSP1 Preload Switch", DADA_CTL_INT, 1, {1} },
	{ "B Digital PCM Volume", DADA_CTL_INT, 1, {817} },
	{ "B High Rate PCM Source", DADA_CTL_ENUM, 1, {10} },
	{ "B PCM Source", DADA_CTL_ENUM, 1, {1} },
	{ "B Ultrasonic Mode", DADA_CTL_ENUM, 1, {0} },
	{ "CLSH Switch", DADA_CTL_INT, 1, {0} },
	{ "DEC0 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC0_BCS Switch", DADA_CTL_INT, 1, {0} },
	{ "DEC1 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC2 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC3 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC4 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC5 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC6 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DEC7 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "DMIC0 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC1 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC1_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC2 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC2_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC3 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC3_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC4 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC4_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC5 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC5_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC6 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC6_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC7 Switch", DADA_CTL_INT, 1, {0} },
	{ "DMIC7_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DMIC8_MIXER Switch", DADA_CTL_INT, 1, {1} },
	{ "DSD_L Switch", DADA_CTL_INT, 1, {0} },
	{ "DSD_R Switch", DADA_CTL_INT, 1, {0} },
	{ "EAR_RDAC Switch", DADA_CTL_INT, 1, {0} },
	{ "HPH Type", DADA_CTL_INT, 1, {0} },
	{ "HPHL Impedance", DADA_CTL_INT, 1, {268435454} },
	{ "HPHL Switch", DADA_CTL_INT, 1, {0} },
	{ "HPHL Volume", DADA_CTL_INT, 1, {20} },
	{ "HPHL_COMP Switch", DADA_CTL_INT, 1, {0} },
	{ "HPHL_RDAC Switch", DADA_CTL_INT, 1, {0} },
	{ "HPHR Impedance", DADA_CTL_INT, 1, {268435454} },
	{ "HPHR Switch", DADA_CTL_INT, 1, {0} },
	{ "HPHR Volume", DADA_CTL_INT, 1, {20} },
	{ "HPHR_COMP Switch", DADA_CTL_INT, 1, {0} },
	{ "HPHR_RDAC Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR0 INP0 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR0 INP0 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR0 INP1 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR0 INP1 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR0 INP2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR0 INP2 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR0 INP3 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR0 INP3 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR1 Band1 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR1 Band2 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR1 Band3 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR1 Band4 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR1 Band5 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR1 INP0 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR1 INP0 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR1 INP1 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR1 INP1 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR1 INP2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR1 INP2 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR1 INP3 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "IIR1 INP3 Volume", DADA_CTL_INT, 1, {84} },
	{ "IIR2 Band1 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR2 Band2 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR2 Band3 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR2 Band4 Switch", DADA_CTL_INT, 1, {0} },
	{ "IIR2 Band5 Switch", DADA_CTL_INT, 1, {0} },
	{ "LDOH Enable Switch", DADA_CTL_INT, 1, {0} },
	{ "LO Switch", DADA_CTL_INT, 1, {0} },
	{ "MBHC Switch", DADA_CTL_INT, 1, {0} },
	{ "MultiMedia2 Mixer TX_CODEC_DMA_TX_3", DADA_CTL_INT, 2, {1,0} },
	{ "MultiMedia2 Mixer VA_CODEC_DMA_TX_0", DADA_CTL_INT, 2, {0,0} },
	{ "RDAC3_MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX HPH Mode", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0 DEM MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0 MIX2 INP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0_1 INTERP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0_1 MIX1 INP0", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0_1 MIX1 INP1", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0_1 MIX1 INP2", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0_2 INTERP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT0_2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1 DEM MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1 MIX2 INP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1_1 INTERP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1_1 MIX1 INP0", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1_1 MIX1 INP1", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1_1 MIX1 INP2", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1_2 INTERP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT1_2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2 MIX2 INP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2_1 INTERP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2_1 MIX1 INP0", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2_1 MIX1 INP1", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2_1 MIX1 INP2", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2_2 INTERP", DADA_CTL_ENUM, 1, {0} },
	{ "RX INT2_2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX MIX TX0 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX MIX TX1 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX MIX TX2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_COMP1 Switch", DADA_CTL_INT, 1, {0} },
	{ "RX_COMP2 Switch", DADA_CTL_INT, 1, {0} },
	{ "RX_EAR Mode Switch", DADA_CTL_INT, 1, {0} },
	{ "RX_HPH HD2 Mode Switch", DADA_CTL_INT, 1, {0} },
	{ "RX_HPH PWR Mode", DADA_CTL_ENUM, 1, {0} },
	{ "RX_MACRO RX0 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_MACRO RX1 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_MACRO RX2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_MACRO RX3 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_MACRO RX4 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_MACRO RX5 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "RX_RX0 Digital Volume", DADA_CTL_INT, 1, {84} },
	{ "RX_RX0 Mix Digital Volume", DADA_CTL_INT, 1, {84} },
	{ "RX_RX1 Digital Volume", DADA_CTL_INT, 1, {84} },
	{ "RX_RX1 Mix Digital Volume", DADA_CTL_INT, 1, {84} },
	{ "RX_RX2 Digital Volume", DADA_CTL_INT, 1, {84} },
	{ "RX_RX2 Mix Digital Volume", DADA_CTL_INT, 1, {84} },
	{ "RX_Softclip Switch", DADA_CTL_INT, 1, {0} },
	{ "SECONDARY_MI2S_RX Audio Mixer MultiMedia1", DADA_CTL_INT, 2, {1,0} },
	{ "T AMP Enable Switch", DADA_CTL_INT, 1, {1} },
	{ "T ASP TX1 Source", DADA_CTL_ENUM, 1, {3} },
	{ "T ASP TX2 Source", DADA_CTL_ENUM, 1, {4} },
	{ "T ASP TX3 Source", DADA_CTL_ENUM, 1, {0} },
	{ "T ASP TX4 Source", DADA_CTL_ENUM, 1, {0} },
	{ "T Amp Gain", DADA_CTL_INT, 1, {10} },
	{ "T DC Watchdog Duration", DADA_CTL_INT, 1, {0} },
	{ "T DC Watchdog Enable", DADA_CTL_INT, 1, {0} },
	{ "T DC Watchdog Mode", DADA_CTL_ENUM, 1, {0} },
	{ "T DC Watchdog Threshold", DADA_CTL_INT, 1, {0} },
	{ "T DSP RX1 Source", DADA_CTL_ENUM, 1, {0} },
	{ "T DSP RX2 Source", DADA_CTL_ENUM, 1, {2} },
	{ "T DSP1 Firmware", DADA_CTL_ENUM, 1, {9} },
	{ "T DSP1 Preload Switch", DADA_CTL_INT, 1, {1} },
	{ "T Digital PCM Volume", DADA_CTL_INT, 1, {913} },
	{ "T High Rate PCM Source", DADA_CTL_ENUM, 1, {10} },
	{ "T PCM Source", DADA_CTL_ENUM, 1, {2} },
	{ "T Ultrasonic Mode", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC0 MUX", DADA_CTL_ENUM, 1, {1} },
	{ "TX DEC1 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC3 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC4 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC5 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC6 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DEC7 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX0", DADA_CTL_ENUM, 1, {4} },
	{ "TX DMIC MUX1", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX2", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX3", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX4", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX5", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX6", DADA_CTL_ENUM, 1, {0} },
	{ "TX DMIC MUX7", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX0", DADA_CTL_ENUM, 1, {2} },
	{ "TX SMIC MUX1", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX2", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX3", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX4", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX5", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX6", DADA_CTL_ENUM, 1, {0} },
	{ "TX SMIC MUX7", DADA_CTL_ENUM, 1, {0} },
	{ "TX0 MODE", DADA_CTL_ENUM, 1, {1} },
	{ "TX1 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "TX2 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "TX3 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC0", DADA_CTL_INT, 1, {1} },
	{ "TX_AIF1_CAP Mixer DEC1", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC2", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC3", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC4", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC5", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC6", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF1_CAP Mixer DEC7", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC0", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC1", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC2", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC3", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC4", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC5", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC6", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF2_CAP Mixer DEC7", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC0", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC1", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC2", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC3", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC4", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC5", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC6", DADA_CTL_INT, 1, {0} },
	{ "TX_AIF3_CAP Mixer DEC7", DADA_CTL_INT, 1, {0} },
	{ "TX_DEC0 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC1 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC2 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC3 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC4 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC5 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC6 Volume", DADA_CTL_INT, 1, {84} },
	{ "TX_DEC7 Volume", DADA_CTL_INT, 1, {84} },
	{ "VA DEC0 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "VA DEC1 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "VA DEC2 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "VA DEC3 MUX", DADA_CTL_ENUM, 1, {0} },
	{ "VA DMIC MUX0", DADA_CTL_ENUM, 1, {1} },
	{ "VA DMIC MUX1", DADA_CTL_ENUM, 1, {2} },
	{ "VA DMIC MUX2", DADA_CTL_ENUM, 1, {0} },
	{ "VA DMIC MUX3", DADA_CTL_ENUM, 1, {0} },
	{ "VA_AIF1_CAP Mixer DEC0", DADA_CTL_INT, 1, {1} },
	{ "VA_AIF1_CAP Mixer DEC1", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF1_CAP Mixer DEC2", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF1_CAP Mixer DEC3", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF2_CAP Mixer DEC0", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF2_CAP Mixer DEC1", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF2_CAP Mixer DEC2", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF2_CAP Mixer DEC3", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF3_CAP Mixer DEC0", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF3_CAP Mixer DEC1", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF3_CAP Mixer DEC2", DADA_CTL_INT, 1, {0} },
	{ "VA_AIF3_CAP Mixer DEC3", DADA_CTL_INT, 1, {0} },
	{ "VA_DEC0 MODE", DADA_CTL_ENUM, 1, {2} },
	{ "VA_DEC0 Volume", DADA_CTL_INT, 1, {124} },
	{ "VA_DEC1 MODE", DADA_CTL_ENUM, 1, {2} },
	{ "VA_DEC1 Volume", DADA_CTL_INT, 1, {124} },
	{ "VA_DEC2 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "VA_DEC2 Volume", DADA_CTL_INT, 1, {84} },
	{ "VA_DEC3 MODE", DADA_CTL_ENUM, 1, {0} },
	{ "VA_DEC3 Volume", DADA_CTL_INT, 1, {84} },
	{ "stream0.vol_ctrl0 MultiMedia1 Playback Volu", DADA_CTL_INT, 1, {16384} },
};

static struct snd_soc_card *dada_defaults_card;

static int dada_apply_ctl_defaults(struct snd_soc_card *card)
{
	int i, fails = 0;

	for (i = 0; i < ARRAY_SIZE(dada_ctl_defaults); i++) {
		const struct dada_ctl_default *d = &dada_ctl_defaults[i];
		struct snd_ctl_elem_id id = {};
		struct snd_ctl_elem_value uc = {};
		struct snd_kcontrol *kctl;
		int j, bad = 0;

		id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		strscpy(id.name, d->name, sizeof(id.name));

		kctl = snd_ctl_find_id(card->snd_card, &id);
		if (!kctl) {
			dev_dbg(card->dev, "dada-audio-defaults: '%s' not registered yet\n", d->name);
			fails++;
			continue;
		}

		/*
		 * Get-only controls exist (e.g. wcd939x "HPH Type" is
		 * SOC_SINGLE_EXT(..., get, NULL)); calling a NULL put
		 * oopses, so skip what cannot be applied.
		 */
		if (!kctl->get || !kctl->put) {
			dev_dbg(card->dev, "dada-audio-defaults: '%s' get/put unavailable, skip\n", d->name);
			continue;
		}

		if (kctl->get(kctl, &uc) < 0) {
			dev_warn(card->dev, "dada-audio-defaults: '%s' get failed\n", d->name);
			fails++;
			continue;
		}

		if (d->kind == DADA_CTL_ENUM) {
			uc.value.enumerated.item[0] = d->values[0];
		} else {
			for (j = 0; j < d->count; j++)
				uc.value.integer.value[j] = d->values[j];
		}
		kctl->put(kctl, &uc);

		memset(&uc, 0, sizeof(uc));
		if (kctl->get(kctl, &uc) < 0)
			bad = 1;
		else if (d->kind == DADA_CTL_ENUM)
			bad = uc.value.enumerated.item[0] != d->values[0];
		else
			for (j = 0; j < d->count; j++)
				bad |= uc.value.integer.value[j] != d->values[j];

		if (bad) {
			dev_warn(card->dev, "dada-audio-defaults: '%s' verify mismatch\n", d->name);
			fails++;
		}
	}
	return fails;
}

static void dada_defaults_retry_fn(struct work_struct *work)
{
	int fails;

	if (!dada_defaults_card)
		return;
	fails = dada_apply_ctl_defaults(dada_defaults_card);
	if (fails)
		dev_warn(dada_defaults_card->dev, "dada-audio-defaults: %d controls not at golden values\n", fails);
	else
		dev_info(dada_defaults_card->dev, "dada-audio-defaults: all %zu controls at golden values\n",
			 ARRAY_SIZE(dada_ctl_defaults));
}

static DECLARE_DELAYED_WORK(dada_defaults_work, dada_defaults_retry_fn);

static int dada_audio_defaults_late_probe(struct snd_soc_card *card)
{
	int fails;

	dada_defaults_card = card;
	fails = dada_apply_ctl_defaults(card);
	dev_info(card->dev, "dada-audio-defaults: pass1 fails=%d (retry in 2s)\n", fails);
	schedule_delayed_work(&dada_defaults_work, msecs_to_jiffies(2000));
	return 0;
}


static int sc8280xp_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sc8280xp_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->priv = of_device_get_match_data(dev);
	if (!data->priv)
		return -ENODEV;

	card->owner = THIS_MODULE;
	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	card->dapm_widgets = data->priv->dapm_widgets;
	card->num_dapm_widgets = data->priv->num_dapm_widgets;
	card->dapm_routes = data->priv->dapm_routes;
	card->num_dapm_routes = data->priv->num_dapm_routes;
	card->controls = data->priv->controls;
	card->num_controls = data->priv->num_controls;

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = data->priv->driver_name;
	sc8280xp_add_be_ops(card);
	if (of_machine_is_compatible("xiaomi,dada"))
		card->late_probe = dada_audio_defaults_late_probe;
	return devm_snd_soc_register_card(dev, card);
}

static struct qcom_snd_soc_common ayaneo_ps2_priv_data = {
	.driver_name = "ayaneo-ps2",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.snd_prepare = ayaneo_ps2_snd_prepare,
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common eliza_priv_data = {
	.driver_name = "eliza",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common hawi_priv_data = {
	.driver_name = "hawi",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.codec_sysclk_set = true,
	.mi2s_bclk_enable = true,
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common kaanapali_priv_data = {
	.driver_name = "kaanapali",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common qcs9100_priv_data = {
	.driver_name = "sa8775p",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
};

static const struct qcom_snd_soc_common qcs615_priv_data = {
	.driver_name = "qcs615",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.codec_sysclk_set = true,
};

static const struct qcom_snd_soc_common qcm6490_priv_data = {
	.driver_name = "qcm6490",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common qcs6490_priv_data = {
	.driver_name = "qcs6490",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common qcs8275_priv_data = {
	.driver_name = "qcs8300",
	.dapm_widgets = max98090_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(max98090_dapm_widgets),
	.controls = max98090_controls,
	.num_controls = ARRAY_SIZE(max98090_controls),
	.codec_sysclk_set = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_BC_FC,
};

static const struct qcom_snd_soc_common sc8280xp_priv_data = {
	.driver_name = "sc8280xp",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common sm8450_priv_data = {
	.driver_name = "sm8450",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/* I2S Connected to HDMI */
	.mi2s_mclk_enable = true,
	.mi2s_bclk_enable = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_BC_FC |
			 SND_SOC_DAIFMT_NB_NF |
			 SND_SOC_DAIFMT_I2S,
};

static const struct qcom_snd_soc_common sm8475_priv_data = {
	.driver_name = "sm8475",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common sm8550_priv_data = {
	.driver_name = "sm8550",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/* I2S Connected to HDMI */
	.mi2s_mclk_enable = true,
	.mi2s_bclk_enable = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_BC_FC |
			 SND_SOC_DAIFMT_NB_NF |
			 SND_SOC_DAIFMT_I2S,
};

static const struct qcom_snd_soc_common sm8650_priv_data = {
	.driver_name = "sm8650",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/* I2S Connected to HDMI */
	.mi2s_mclk_enable = true,
	.mi2s_bclk_enable = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_BC_FC |
			 SND_SOC_DAIFMT_NB_NF |
			 SND_SOC_DAIFMT_I2S,
};

static const struct qcom_snd_soc_common sm8750_priv_data = {
	.driver_name = "sm8750",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/*
	 * sc8280xp_snd_hw_params only calls
	 * snd_soc_dai_set_sysclk(cpu_dai, LPAIF_MI2S_BCLK, ...) when this is
	 * set, and q6i2s_set_sysclk is the only thing that does
	 * clk_prepare_enable() on the "bclk" clock of the q6apm bedai node
	 * (LPASS_CLK_ID_SEC_MI2S_IBIT).  Without it the LPASS never emits
	 * BCLK/FSYNC, so an external slave codec such as the CS35L43 pair on
	 * dada stays silent even though every DAPM widget is powered.
	 * Verified on hardware: clk_summary showed enable_count 0 for
	 * LPASS_CLK_ID_SEC_MI2S_IBIT while "Secondary MI2S Playback" was
	 * active.  The reference SM8750 tree enables it unconditionally.
	 */
	.mi2s_bclk_enable = true,
	/*
	 * sc8280xp_snd_hw_params only calls snd_soc_dai_set_sysclk(codec_dai,
	 * ...) when this is set, and that is the only caller that reaches
	 * cs35l43_component_set_sysclk() -> cs35l43_pll_config().  Without it
	 * CS35L43 REFCLK_INPUT stays at its reset FREQ=0x00 (32768 Hz) and
	 * FS_MON_0 is never written, so the PLL never locks and both speakers
	 * stay silent even though BCLK/FSYNC and every DAPM widget are up.
	 * Verified on hardware by hand-running the pll_config sequence over
	 * raw I2C (see docs/audio-fix.md, Blocker D).
	 */
	.codec_sysclk_set = true,
};

static const struct of_device_id snd_sc8280xp_dt_match[] = {
	{ .compatible = "ayaneo,pocket-s2-sndcard", .data = &ayaneo_ps2_priv_data },
	{ .compatible = "qcom,eliza-sndcard", .data = &eliza_priv_data },
	{ .compatible = "qcom,hawi-sndcard", .data = &hawi_priv_data },
	{ .compatible = "qcom,kaanapali-sndcard", .data = &kaanapali_priv_data },
	{ .compatible = "qcom,maili-sndcard", .data = &hawi_priv_data },
	{ .compatible = "qcom,qcm6490-idp-sndcard", .data = &qcm6490_priv_data },
	{ .compatible = "qcom,qcs615-sndcard", .data = &qcs615_priv_data },
	{ .compatible = "qcom,qcs6490-rb3gen2-sndcard", .data = &qcs6490_priv_data },
	{ .compatible = "qcom,qcs8275-sndcard", .data = &qcs8275_priv_data },
	{ .compatible = "qcom,qcs9075-sndcard", .data = &qcs9100_priv_data },
	{ .compatible = "qcom,qcs9100-sndcard", .data = &qcs9100_priv_data },
	{ .compatible = "qcom,sc8280xp-sndcard", .data = &sc8280xp_priv_data },
	{ .compatible = "qcom,sm8450-sndcard", .data = &sm8450_priv_data },
	{ .compatible = "qcom,sm8475-sndcard", .data = &sm8475_priv_data },
	{ .compatible = "qcom,sm8550-sndcard", .data = &sm8550_priv_data },
	{ .compatible = "qcom,sm8650-sndcard", .data = &sm8650_priv_data },
	{ .compatible = "qcom,sm8750-sndcard", .data = &sm8750_priv_data },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sc8280xp_dt_match);

static struct platform_driver snd_sc8280xp_driver = {
	.probe  = sc8280xp_platform_probe,
	.driver = {
		.name = "snd-sc8280xp",
		.of_match_table = snd_sc8280xp_dt_match,
	},
};
module_platform_driver(snd_sc8280xp_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SC8280XP ASoC Machine Driver");
MODULE_LICENSE("GPL");
