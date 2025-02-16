/*
 * linux/sound/pxa2xx-ac97.c -- AC97 support for the Intel PXA2xx chip.
 *
 * Author:	Nicolas Pitre
 * Created:	Dec 02, 2004
 * Copyright:	MontaVista Software Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/ac97_codec.h>
#include <sound/soc.h>
#include <sound/pxa2xx-lib.h>

#include <mach/hardware.h>
#include <mach/pxa-regs.h>

#include "pxa2xx-pcm.h"
#include "pxa2xx-ac97.h"

static void pxa2xx_ac97_warm_reset(struct snd_ac97 *ac97)
{
	pxa2xx_ac97_try_warm_reset(ac97);

	pxa2xx_ac97_finish_reset(ac97);
}

static void pxa2xx_ac97_cold_reset(struct snd_ac97 *ac97)
{
	pxa2xx_ac97_try_cold_reset(ac97);

	pxa2xx_ac97_finish_reset(ac97);
}

struct snd_ac97_bus_ops soc_ac97_ops = {
	.read	= pxa2xx_ac97_read,
	.write	= pxa2xx_ac97_write,
	.warm_reset	= pxa2xx_ac97_warm_reset,
	.reset	= pxa2xx_ac97_cold_reset,
};

static struct pxa2xx_pcm_dma_params pxa2xx_ac97_pcm_stereo_out = {
	.name			= "AC97 PCM Stereo out",
	.dev_addr		= __PREG(PCDR),
	.drcmr			= &DRCMR(12),
	.dcmd			= DCMD_INCSRCADDR | DCMD_FLOWTRG |
				  DCMD_BURST32 | DCMD_WIDTH4,
};

static struct pxa2xx_pcm_dma_params pxa2xx_ac97_pcm_stereo_in = {
	.name			= "AC97 PCM Stereo in",
	.dev_addr		= __PREG(PCDR),
	.drcmr			= &DRCMR(11),
	.dcmd			= DCMD_INCTRGADDR | DCMD_FLOWSRC |
				  DCMD_BURST32 | DCMD_WIDTH4,
};

static struct pxa2xx_pcm_dma_params pxa2xx_ac97_pcm_aux_mono_out = {
	.name			= "AC97 Aux PCM (Slot 5) Mono out",
	.dev_addr		= __PREG(MODR),
	.drcmr			= &DRCMR(10),
	.dcmd			= DCMD_INCSRCADDR | DCMD_FLOWTRG |
				  DCMD_BURST16 | DCMD_WIDTH2,
};

static struct pxa2xx_pcm_dma_params pxa2xx_ac97_pcm_aux_mono_in = {
	.name			= "AC97 Aux PCM (Slot 5) Mono in",
	.dev_addr		= __PREG(MODR),
	.drcmr			= &DRCMR(9),
	.dcmd			= DCMD_INCTRGADDR | DCMD_FLOWSRC |
				  DCMD_BURST16 | DCMD_WIDTH2,
};

static struct pxa2xx_pcm_dma_params pxa2xx_ac97_pcm_mic_mono_in = {
	.name			= "AC97 Mic PCM (Slot 6) Mono in",
	.dev_addr		= __PREG(MCDR),
	.drcmr			= &DRCMR(8),
	.dcmd			= DCMD_INCTRGADDR | DCMD_FLOWSRC |
				  DCMD_BURST16 | DCMD_WIDTH2,
};

#ifdef CONFIG_PM
static int pxa2xx_ac97_suspend(struct snd_soc_dai *dai)
{
	return pxa2xx_ac97_hw_suspend();
}

static int pxa2xx_ac97_resume(struct snd_soc_dai *dai)
{
	return pxa2xx_ac97_hw_resume();
}

#else
#define pxa2xx_ac97_suspend	NULL
#define pxa2xx_ac97_resume	NULL
#endif

static int pxa2xx_ac97_probe(struct platform_device *pdev,
			     struct snd_soc_dai *dai)
{
	return pxa2xx_ac97_hw_probe(pdev);
}

static void pxa2xx_ac97_remove(struct platform_device *pdev,
			       struct snd_soc_dai *dai)
{
	pxa2xx_ac97_hw_remove(pdev);
}

static int pxa2xx_ac97_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		cpu_dai->dma_data = &pxa2xx_ac97_pcm_stereo_out;
	else
		cpu_dai->dma_data = &pxa2xx_ac97_pcm_stereo_in;

	return 0;
}

static int pxa2xx_ac97_hw_aux_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		cpu_dai->dma_data = &pxa2xx_ac97_pcm_aux_mono_out;
	else
		cpu_dai->dma_data = &pxa2xx_ac97_pcm_aux_mono_in;

	return 0;
}

static int pxa2xx_ac97_hw_mic_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return -ENODEV;
	else
		cpu_dai->dma_data = &pxa2xx_ac97_pcm_mic_mono_in;

	return 0;
}

#define PXA2XX_AC97_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |\
		SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_44100 | \
		SNDRV_PCM_RATE_48000)

/*
 * There is only 1 physical AC97 interface for pxa2xx, but it
 * has extra fifo's that can be used for aux DACs and ADCs.
 */
struct snd_soc_dai pxa_ac97_dai[] = {
{
	.name = "pxa2xx-ac97",
	.id = 0,
	.ac97_control = 1,
	.probe = pxa2xx_ac97_probe,
	.remove = pxa2xx_ac97_remove,
	.suspend = pxa2xx_ac97_suspend,
	.resume = pxa2xx_ac97_resume,
	.playback = {
		.stream_name = "AC97 Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = PXA2XX_AC97_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,},
	.capture = {
		.stream_name = "AC97 Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = PXA2XX_AC97_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,},
	.ops = {
		.hw_params = pxa2xx_ac97_hw_params,},
},
{
	.name = "pxa2xx-ac97-aux",
	.id = 1,
	.ac97_control = 1,
	.playback = {
		.stream_name = "AC97 Aux Playback",
		.channels_min = 1,
		.channels_max = 1,
		.rates = PXA2XX_AC97_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,},
	.capture = {
		.stream_name = "AC97 Aux Capture",
		.channels_min = 1,
		.channels_max = 1,
		.rates = PXA2XX_AC97_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,},
	.ops = {
		.hw_params = pxa2xx_ac97_hw_aux_params,},
},
{
	.name = "pxa2xx-ac97-mic",
	.id = 2,
	.ac97_control = 1,
	.capture = {
		.stream_name = "AC97 Mic Capture",
		.channels_min = 1,
		.channels_max = 1,
		.rates = PXA2XX_AC97_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,},
	.ops = {
		.hw_params = pxa2xx_ac97_hw_mic_params,},
},
};

EXPORT_SYMBOL_GPL(pxa_ac97_dai);
EXPORT_SYMBOL_GPL(soc_ac97_ops);

static int __init pxa_ac97_init(void)
{
	return snd_soc_register_dais(pxa_ac97_dai, ARRAY_SIZE(pxa_ac97_dai));
}
module_init(pxa_ac97_init);

static void __exit pxa_ac97_exit(void)
{
	snd_soc_unregister_dais(pxa_ac97_dai, ARRAY_SIZE(pxa_ac97_dai));
}
module_exit(pxa_ac97_exit);

MODULE_AUTHOR("Nicolas Pitre");
MODULE_DESCRIPTION("AC97 driver for the Intel PXA2xx chip");
MODULE_LICENSE("GPL");
