/*
 * soc-dapm.c  --  ALSA SoC Dynamic Audio Power Management
 *
 * Copyright 2005 Wolfson Microelectronics PLC.
 * Author: Liam Girdwood <lrg@slimlogic.co.uk>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  Features:
 *    o Changes power status of internal codec blocks depending on the
 *      dynamic configuration of codec internal audio paths and active
 *      DAC's/ADC's.
 *    o Platform power domain - can support external components i.e. amps and
 *      mic/meadphone insertion events.
 *    o Automatic Mic Bias support
 *    o Jack insertion power event initiation - e.g. hp insertion will enable
 *      sinks, dacs, etc
 *    o Delayed powerdown of audio susbsystem to reduce pops between a quick
 *      device reopen.
 *
 *  Todo:
 *    o DAPM power change sequencing - allow for configurable per
 *      codec sequences.
 *    o Support for analogue bias optimisation.
 *    o Support for reduced codec oversampling rates.
 *    o Support for reduced codec bias currents.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>

/* debug */
#ifdef DEBUG
#define dump_dapm(codec, action) dbg_dump_dapm(codec, action)
#else
#define dump_dapm(codec, action)
#endif

/* dapm power sequences - make this per codec in the future */
static int dapm_up_seq[] = {
	snd_soc_dapm_pre, snd_soc_dapm_micbias, snd_soc_dapm_mic,
	snd_soc_dapm_mux, snd_soc_dapm_dac, snd_soc_dapm_mixer, snd_soc_dapm_pga,
	snd_soc_dapm_adc, snd_soc_dapm_hp, snd_soc_dapm_spk, snd_soc_dapm_post
};
static int dapm_down_seq[] = {
	snd_soc_dapm_pre, snd_soc_dapm_adc, snd_soc_dapm_hp, snd_soc_dapm_spk,
	snd_soc_dapm_pga, snd_soc_dapm_mixer, snd_soc_dapm_dac, snd_soc_dapm_mic,
	snd_soc_dapm_micbias, snd_soc_dapm_mux, snd_soc_dapm_post
};

static int dapm_status = 1;
module_param(dapm_status, int, 0);
MODULE_PARM_DESC(dapm_status, "enable DPM sysfs entries");

static void pop_wait(u32 pop_time)
{
	if (pop_time)
		schedule_timeout_uninterruptible(msecs_to_jiffies(pop_time));
}

static void pop_dbg(u32 pop_time, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	if (pop_time) {
		vprintk(fmt, args);
		pop_wait(pop_time);
	}

	va_end(args);
}

/* create a new dapm widget */
static inline struct snd_soc_dapm_widget *dapm_cnew_widget(
	const struct snd_soc_dapm_widget *_widget)
{
	return kmemdup(_widget, sizeof(*_widget), GFP_KERNEL);
}

/* set up initial codec paths */
static void dapm_set_path_status(struct snd_soc_dapm_widget *w,
	struct snd_soc_dapm_path *p, int i)
{
	switch (w->id) {
	case snd_soc_dapm_switch:
	case snd_soc_dapm_mixer: {
		int val;
		struct soc_mixer_control *mc = (struct soc_mixer_control *)
			w->kcontrols[i].private_value;
		unsigned int reg = mc->reg;
		unsigned int shift = mc->shift;
		int max = mc->max;
		unsigned int mask = (1 << fls(max)) - 1;
		unsigned int invert = mc->invert;

		val = snd_soc_read(w->codec, reg);
		val = (val >> shift) & mask;

		if ((invert && !val) || (!invert && val))
			p->connect = 1;
		else
			p->connect = 0;
	}
	break;
	case snd_soc_dapm_mux: {
		struct soc_enum *e = (struct soc_enum *)w->kcontrols[i].private_value;
		int val, item, bitmask;

		for (bitmask = 1; bitmask < e->max; bitmask <<= 1)
		;
		val = snd_soc_read(w->codec, e->reg);
		item = (val >> e->shift_l) & (bitmask - 1);

		p->connect = 0;
		for (i = 0; i < e->max; i++) {
			if (!(strcmp(p->name, e->texts[i])) && item == i)
				p->connect = 1;
		}
	}
	break;
	/* does not effect routing - always connected */
	case snd_soc_dapm_pga:
	case snd_soc_dapm_output:
	case snd_soc_dapm_adc:
	case snd_soc_dapm_input:
	case snd_soc_dapm_dac:
	case snd_soc_dapm_micbias:
	case snd_soc_dapm_vmid:
		p->connect = 1;
	break;
	/* does effect routing - dynamically connected */
	case snd_soc_dapm_hp:
	case snd_soc_dapm_mic:
	case snd_soc_dapm_spk:
	case snd_soc_dapm_line:
	case snd_soc_dapm_pre:
	case snd_soc_dapm_post:
		p->connect = 0;
	break;
	}
}

/* connect mux widget to it's interconnecting audio paths */
static int dapm_connect_mux(struct snd_soc_codec *codec,
	struct snd_soc_dapm_widget *src, struct snd_soc_dapm_widget *dest,
	struct snd_soc_dapm_path *path, const char *control_name,
	const struct snd_kcontrol_new *kcontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int i;

	for (i = 0; i < e->max; i++) {
		if (!(strcmp(control_name, e->texts[i]))) {
			list_add(&path->list, &codec->dapm_paths);
			list_add(&path->list_sink, &dest->sources);
			list_add(&path->list_source, &src->sinks);
			path->name = (char*)e->texts[i];
			dapm_set_path_status(dest, path, 0);
			return 0;
		}
	}

	return -ENODEV;
}

/* connect mixer widget to it's interconnecting audio paths */
static int dapm_connect_mixer(struct snd_soc_codec *codec,
	struct snd_soc_dapm_widget *src, struct snd_soc_dapm_widget *dest,
	struct snd_soc_dapm_path *path, const char *control_name)
{
	int i;

	/* search for mixer kcontrol */
	for (i = 0; i < dest->num_kcontrols; i++) {
		if (!strcmp(control_name, dest->kcontrols[i].name)) {
			list_add(&path->list, &codec->dapm_paths);
			list_add(&path->list_sink, &dest->sources);
			list_add(&path->list_source, &src->sinks);
			path->name = dest->kcontrols[i].name;
			dapm_set_path_status(dest, path, i);
			return 0;
		}
	}
	return -ENODEV;
}

/* update dapm codec register bits */
static int dapm_update_bits(struct snd_soc_dapm_widget *widget)
{
	int change, power;
	unsigned short old, new;
	struct snd_soc_codec *codec = widget->codec;

	/* check for valid widgets */
	if (widget->reg < 0 || widget->id == snd_soc_dapm_input ||
		widget->id == snd_soc_dapm_output ||
		widget->id == snd_soc_dapm_hp ||
		widget->id == snd_soc_dapm_mic ||
		widget->id == snd_soc_dapm_line ||
		widget->id == snd_soc_dapm_spk)
		return 0;

	power = widget->power;
	if (widget->invert)
		power = (power ? 0:1);

	old = snd_soc_read(codec, widget->reg);
	new = (old & ~(0x1 << widget->shift)) | (power << widget->shift);

	change = old != new;
	if (change) {
		pop_dbg(codec->pop_time, "pop test %s : %s in %d ms\n",
			widget->name, widget->power ? "on" : "off",
			codec->pop_time);
		snd_soc_write(codec, widget->reg, new);
		pop_wait(codec->pop_time);
	}
	pr_debug("reg %x old %x new %x change %d\n", widget->reg,
		 old, new, change);
	return change;
}

/* ramps the volume up or down to minimise pops before or after a
 * DAPM power event */
static int dapm_set_pga(struct snd_soc_dapm_widget *widget, int power)
{
	const struct snd_kcontrol_new *k = widget->kcontrols;

	if (widget->muted && !power)
		return 0;
	if (!widget->muted && power)
		return 0;

	if (widget->num_kcontrols && k) {
		struct soc_mixer_control *mc =
			(struct soc_mixer_control *)k->private_value;
		unsigned int reg = mc->reg;
		unsigned int shift = mc->shift;
		int max = mc->max;
		unsigned int mask = (1 << fls(max)) - 1;
		unsigned int invert = mc->invert;

		if (power) {
			int i;
			/* power up has happended, increase volume to last level */
			if (invert) {
				for (i = max; i > widget->saved_value; i--)
					snd_soc_update_bits(widget->codec, reg, mask, i);
			} else {
				for (i = 0; i < widget->saved_value; i++)
					snd_soc_update_bits(widget->codec, reg, mask, i);
			}
			widget->muted = 0;
		} else {
			/* power down is about to occur, decrease volume to mute */
			int val = snd_soc_read(widget->codec, reg);
			int i = widget->saved_value = (val >> shift) & mask;
			if (invert) {
				for (; i < mask; i++)
					snd_soc_update_bits(widget->codec, reg, mask, i);
			} else {
				for (; i > 0; i--)
					snd_soc_update_bits(widget->codec, reg, mask, i);
			}
			widget->muted = 1;
		}
	}
	return 0;
}

/* create new dapm mixer control */
static int dapm_new_mixer(struct snd_soc_codec *codec,
	struct snd_soc_dapm_widget *w)
{
	int i, ret = 0;
	size_t name_len;
	struct snd_soc_dapm_path *path;

	/* add kcontrol */
	for (i = 0; i < w->num_kcontrols; i++) {

		/* match name */
		list_for_each_entry(path, &w->sources, list_sink) {

			/* mixer/mux paths name must match control name */
			if (path->name != (char*)w->kcontrols[i].name)
				continue;

			/* add dapm control with long name */
			name_len = 2 + strlen(w->name)
				+ strlen(w->kcontrols[i].name);
			path->long_name = kmalloc(name_len, GFP_KERNEL);
			if (path->long_name == NULL)
				return -ENOMEM;

			snprintf(path->long_name, name_len, "%s %s",
				 w->name, w->kcontrols[i].name);
			path->long_name[name_len - 1] = '\0';

			path->kcontrol = snd_soc_cnew(&w->kcontrols[i], w,
				path->long_name);
			ret = snd_ctl_add(codec->card, path->kcontrol);
			if (ret < 0) {
				printk(KERN_ERR "asoc: failed to add dapm kcontrol %s\n",
						path->long_name);
				kfree(path->long_name);
				path->long_name = NULL;
				return ret;
			}
		}
	}
	return ret;
}

/* create new dapm mux control */
static int dapm_new_mux(struct snd_soc_codec *codec,
	struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_path *path = NULL;
	struct snd_kcontrol *kcontrol;
	int ret = 0;

	if (!w->num_kcontrols) {
		printk(KERN_ERR "asoc: mux %s has no controls\n", w->name);
		return -EINVAL;
	}

	kcontrol = snd_soc_cnew(&w->kcontrols[0], w, w->name);
	ret = snd_ctl_add(codec->card, kcontrol);
	if (ret < 0)
		goto err;

	list_for_each_entry(path, &w->sources, list_sink)
		path->kcontrol = kcontrol;

	return ret;

err:
	printk(KERN_ERR "asoc: failed to add kcontrol %s\n", w->name);
	return ret;
}

/* create new dapm volume control */
static int dapm_new_pga(struct snd_soc_codec *codec,
	struct snd_soc_dapm_widget *w)
{
	struct snd_kcontrol *kcontrol;
	int ret = 0;

	if (!w->num_kcontrols)
		return -EINVAL;

	kcontrol = snd_soc_cnew(&w->kcontrols[0], w, w->name);
	ret = snd_ctl_add(codec->card, kcontrol);
	if (ret < 0) {
		printk(KERN_ERR "asoc: failed to add kcontrol %s\n", w->name);
		return ret;
	}

	return ret;
}

/* reset 'walked' bit for each dapm path */
static inline void dapm_clear_walk(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_path *p;

	list_for_each_entry(p, &codec->dapm_paths, list)
		p->walked = 0;
}

/*
 * Recursively check for a completed path to an active or physically connected
 * output widget. Returns number of complete paths.
 */
static int is_connected_output_ep(struct snd_soc_dapm_widget *widget)
{
	struct snd_soc_dapm_path *path;
	int con = 0;

	if (widget->id == snd_soc_dapm_adc && widget->active)
		return 1;

	if (widget->connected) {
		/* connected pin ? */
		if (widget->id == snd_soc_dapm_output && !widget->ext)
			return 1;

		/* connected jack or spk ? */
		if (widget->id == snd_soc_dapm_hp || widget->id == snd_soc_dapm_spk ||
			widget->id == snd_soc_dapm_line)
			return 1;
	}

	list_for_each_entry(path, &widget->sinks, list_source) {
		if (path->walked)
			continue;

		if (path->sink && path->connect) {
			path->walked = 1;
			con += is_connected_output_ep(path->sink);
		}
	}

	return con;
}

/*
 * Recursively check for a completed path to an active or physically connected
 * input widget. Returns number of complete paths.
 */
static int is_connected_input_ep(struct snd_soc_dapm_widget *widget)
{
	struct snd_soc_dapm_path *path;
	int con = 0;

	/* active stream ? */
	if (widget->id == snd_soc_dapm_dac && widget->active)
		return 1;

	if (widget->connected) {
		/* connected pin ? */
		if (widget->id == snd_soc_dapm_input && !widget->ext)
			return 1;

		/* connected VMID/Bias for lower pops */
		if (widget->id == snd_soc_dapm_vmid)
			return 1;

		/* connected jack ? */
		if (widget->id == snd_soc_dapm_mic || widget->id == snd_soc_dapm_line)
			return 1;
	}

	list_for_each_entry(path, &widget->sources, list_sink) {
		if (path->walked)
			continue;

		if (path->source && path->connect) {
			path->walked = 1;
			con += is_connected_input_ep(path->source);
		}
	}

	return con;
}

/*
 * Handler for generic register modifier widget.
 */
int dapm_reg_event(struct snd_soc_dapm_widget *w,
		   struct snd_kcontrol *kcontrol, int event)
{
	unsigned int val;

	if (SND_SOC_DAPM_EVENT_ON(event))
		val = w->on_val;
	else
		val = w->off_val;

	snd_soc_update_bits(w->codec, -(w->reg + 1),
			    w->mask << w->shift, val << w->shift);

	return 0;
}
EXPORT_SYMBOL_GPL(dapm_reg_event);

/*
 * Scan each dapm widget for complete audio path.
 * A complete path is a route that has valid endpoints i.e.:-
 *
 *  o DAC to output pin.
 *  o Input Pin to ADC.
 *  o Input pin to Output pin (bypass, sidetone)
 *  o DAC to ADC (loopback).
 */
static int dapm_power_widgets(struct snd_soc_codec *codec, int event)
{
	struct snd_soc_dapm_widget *w;
	int in, out, i, c = 1, *seq = NULL, ret = 0, power_change, power;

	/* do we have a sequenced stream event */
	if (event == SND_SOC_DAPM_STREAM_START) {
		c = ARRAY_SIZE(dapm_up_seq);
		seq = dapm_up_seq;
	} else if (event == SND_SOC_DAPM_STREAM_STOP) {
		c = ARRAY_SIZE(dapm_down_seq);
		seq = dapm_down_seq;
	}

	for(i = 0; i < c; i++) {
		list_for_each_entry(w, &codec->dapm_widgets, list) {

			/* is widget in stream order */
			if (seq && seq[i] && w->id != seq[i])
				continue;

			/* vmid - no action */
			if (w->id == snd_soc_dapm_vmid)
				continue;

			/* active ADC */
			if (w->id == snd_soc_dapm_adc && w->active) {
				in = is_connected_input_ep(w);
				dapm_clear_walk(w->codec);
				w->power = (in != 0) ? 1 : 0;
				dapm_update_bits(w);
				continue;
			}

			/* active DAC */
			if (w->id == snd_soc_dapm_dac && w->active) {
				out = is_connected_output_ep(w);
				dapm_clear_walk(w->codec);
				w->power = (out != 0) ? 1 : 0;
				dapm_update_bits(w);
				continue;
			}

			/* pre and post event widgets */
			if (w->id == snd_soc_dapm_pre) {
				if (!w->event)
					continue;

				if (event == SND_SOC_DAPM_STREAM_START) {
					ret = w->event(w,
						NULL, SND_SOC_DAPM_PRE_PMU);
					if (ret < 0)
						return ret;
				} else if (event == SND_SOC_DAPM_STREAM_STOP) {
					ret = w->event(w,
						NULL, SND_SOC_DAPM_PRE_PMD);
					if (ret < 0)
						return ret;
				}
				continue;
			}
			if (w->id == snd_soc_dapm_post) {
				if (!w->event)
					continue;

				if (event == SND_SOC_DAPM_STREAM_START) {
					ret = w->event(w,
						NULL, SND_SOC_DAPM_POST_PMU);
					if (ret < 0)
						return ret;
				} else if (event == SND_SOC_DAPM_STREAM_STOP) {
					ret = w->event(w,
						NULL, SND_SOC_DAPM_POST_PMD);
					if (ret < 0)
						return ret;
				}
				continue;
			}

			/* all other widgets */
			in = is_connected_input_ep(w);
			dapm_clear_walk(w->codec);
			out = is_connected_output_ep(w);
			dapm_clear_walk(w->codec);
			power = (out != 0 && in != 0) ? 1 : 0;
			power_change = (w->power == power) ? 0: 1;
			w->power = power;

			if (!power_change)
				continue;

			/* call any power change event handlers */
			if (w->event)
				pr_debug("power %s event for %s flags %x\n",
					 w->power ? "on" : "off",
					 w->name, w->event_flags);

			/* power up pre event */
			if (power && w->event &&
			    (w->event_flags & SND_SOC_DAPM_PRE_PMU)) {
				ret = w->event(w, NULL, SND_SOC_DAPM_PRE_PMU);
				if (ret < 0)
					return ret;
			}

			/* power down pre event */
			if (!power && w->event &&
			    (w->event_flags & SND_SOC_DAPM_PRE_PMD)) {
				ret = w->event(w, NULL, SND_SOC_DAPM_PRE_PMD);
				if (ret < 0)
					return ret;
			}

			/* Lower PGA volume to reduce pops */
			if (w->id == snd_soc_dapm_pga && !power)
				dapm_set_pga(w, power);

			dapm_update_bits(w);

			/* Raise PGA volume to reduce pops */
			if (w->id == snd_soc_dapm_pga && power)
				dapm_set_pga(w, power);

			/* power up post event */
			if (power && w->event &&
			    (w->event_flags & SND_SOC_DAPM_POST_PMU)) {
				ret = w->event(w,
					       NULL, SND_SOC_DAPM_POST_PMU);
				if (ret < 0)
					return ret;
			}

			/* power down post event */
			if (!power && w->event &&
			    (w->event_flags & SND_SOC_DAPM_POST_PMD)) {
				ret = w->event(w, NULL, SND_SOC_DAPM_POST_PMD);
				if (ret < 0)
					return ret;
			}
		}
	}

	return ret;
}

#ifdef DEBUG
static void dbg_dump_dapm(struct snd_soc_codec* codec, const char *action)
{
	struct snd_soc_dapm_widget *w;
	struct snd_soc_dapm_path *p = NULL;
	int in, out;

	printk("DAPM %s %s\n", codec->name, action);

	list_for_each_entry(w, &codec->dapm_widgets, list) {

		/* only display widgets that effect routing */
		switch (w->id) {
		case snd_soc_dapm_pre:
		case snd_soc_dapm_post:
		case snd_soc_dapm_vmid:
			continue;
		case snd_soc_dapm_mux:
		case snd_soc_dapm_output:
		case snd_soc_dapm_input:
		case snd_soc_dapm_switch:
		case snd_soc_dapm_hp:
		case snd_soc_dapm_mic:
		case snd_soc_dapm_spk:
		case snd_soc_dapm_line:
		case snd_soc_dapm_micbias:
		case snd_soc_dapm_dac:
		case snd_soc_dapm_adc:
		case snd_soc_dapm_pga:
		case snd_soc_dapm_mixer:
			if (w->name) {
				in = is_connected_input_ep(w);
				dapm_clear_walk(w->codec);
				out = is_connected_output_ep(w);
				dapm_clear_walk(w->codec);
				printk("%s: %s  in %d out %d\n", w->name,
					w->power ? "On":"Off",in, out);

				list_for_each_entry(p, &w->sources, list_sink) {
					if (p->connect)
						printk(" in  %s %s\n", p->name ? p->name : "static",
							p->source->name);
				}
				list_for_each_entry(p, &w->sinks, list_source) {
					if (p->connect)
						printk(" out %s %s\n", p->name ? p->name : "static",
							p->sink->name);
				}
			}
		break;
		}
	}
}
#endif

/* test and update the power status of a mux widget */
static int dapm_mux_update_power(struct snd_soc_dapm_widget *widget,
				 struct snd_kcontrol *kcontrol, int mask,
				 int mux, int val, struct soc_enum *e)
{
	struct snd_soc_dapm_path *path;
	int found = 0;

	if (widget->id != snd_soc_dapm_mux)
		return -ENODEV;

	if (!snd_soc_test_bits(widget->codec, e->reg, mask, val))
		return 0;

	/* find dapm widget path assoc with kcontrol */
	list_for_each_entry(path, &widget->codec->dapm_paths, list) {
		if (path->kcontrol != kcontrol)
			continue;

		if (!path->name || !e->texts[mux])
			continue;

		found = 1;
		/* we now need to match the string in the enum to the path */
		if (!(strcmp(path->name, e->texts[mux])))
			path->connect = 1; /* new connection */
		else
			path->connect = 0; /* old connection must be powered down */
	}

	if (found) {
		dapm_power_widgets(widget->codec, SND_SOC_DAPM_STREAM_NOP);
		dump_dapm(widget->codec, "mux power update");
	}

	return 0;
}

/* test and update the power status of a mixer or switch widget */
static int dapm_mixer_update_power(struct snd_soc_dapm_widget *widget,
				   struct snd_kcontrol *kcontrol, int reg,
				   int val_mask, int val, int invert)
{
	struct snd_soc_dapm_path *path;
	int found = 0;

	if (widget->id != snd_soc_dapm_mixer &&
	    widget->id != snd_soc_dapm_switch)
		return -ENODEV;

	if (!snd_soc_test_bits(widget->codec, reg, val_mask, val))
		return 0;

	/* find dapm widget path assoc with kcontrol */
	list_for_each_entry(path, &widget->codec->dapm_paths, list) {
		if (path->kcontrol != kcontrol)
			continue;

		/* found, now check type */
		found = 1;
		if (val)
			/* new connection */
			path->connect = invert ? 0:1;
		else
			/* old connection must be powered down */
			path->connect = invert ? 1:0;
		break;
	}

	if (found) {
		dapm_power_widgets(widget->codec, SND_SOC_DAPM_STREAM_NOP);
		dump_dapm(widget->codec, "mixer power update");
	}

	return 0;
}

/* show dapm widget status in sys fs */
static ssize_t dapm_widget_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct snd_soc_device *devdata = dev_get_drvdata(dev);
	struct snd_soc_codec *codec = devdata->codec;
	struct snd_soc_dapm_widget *w;
	int count = 0;
	char *state = "not set";

	list_for_each_entry(w, &codec->dapm_widgets, list) {

		/* only display widgets that burnm power */
		switch (w->id) {
		case snd_soc_dapm_hp:
		case snd_soc_dapm_mic:
		case snd_soc_dapm_spk:
		case snd_soc_dapm_line:
		case snd_soc_dapm_micbias:
		case snd_soc_dapm_dac:
		case snd_soc_dapm_adc:
		case snd_soc_dapm_pga:
		case snd_soc_dapm_mixer:
			if (w->name)
				count += sprintf(buf + count, "%s: %s\n",
					w->name, w->power ? "On":"Off");
		break;
		default:
		break;
		}
	}

	switch (codec->bias_level) {
	case SND_SOC_BIAS_ON:
		state = "On";
		break;
	case SND_SOC_BIAS_PREPARE:
		state = "Prepare";
		break;
	case SND_SOC_BIAS_STANDBY:
		state = "Standby";
		break;
	case SND_SOC_BIAS_OFF:
		state = "Off";
		break;
	}
	count += sprintf(buf + count, "PM State: %s\n", state);

	return count;
}

static DEVICE_ATTR(dapm_widget, 0444, dapm_widget_show, NULL);

int snd_soc_dapm_sys_add(struct device *dev)
{
	if (!dapm_status)
		return 0;
	return device_create_file(dev, &dev_attr_dapm_widget);
}

static void snd_soc_dapm_sys_remove(struct device *dev)
{
	if (dapm_status) {
		device_remove_file(dev, &dev_attr_dapm_widget);
	}
}

/* free all dapm widgets and resources */
static void dapm_free_widgets(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_widget *w, *next_w;
	struct snd_soc_dapm_path *p, *next_p;

	list_for_each_entry_safe(w, next_w, &codec->dapm_widgets, list) {
		list_del(&w->list);
		kfree(w);
	}

	list_for_each_entry_safe(p, next_p, &codec->dapm_paths, list) {
		list_del(&p->list);
		kfree(p->long_name);
		kfree(p);
	}
}

static int snd_soc_dapm_set_pin(struct snd_soc_codec *codec,
	char *pin, int status)
{
	struct snd_soc_dapm_widget *w;

	list_for_each_entry(w, &codec->dapm_widgets, list) {
		if (!strcmp(w->name, pin)) {
			pr_debug("dapm: %s: pin %s\n", codec->name, pin);
			w->connected = status;
			return 0;
		}
	}

	pr_err("dapm: %s: configuring unknown pin %s\n", codec->name, pin);
	return -EINVAL;
}

/**
 * snd_soc_dapm_sync - scan and power dapm paths
 * @codec: audio codec
 *
 * Walks all dapm audio paths and powers widgets according to their
 * stream or path usage.
 *
 * Returns 0 for success.
 */
int snd_soc_dapm_sync(struct snd_soc_codec *codec)
{
	int ret = dapm_power_widgets(codec, SND_SOC_DAPM_STREAM_NOP);
	dump_dapm(codec, "sync");
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_sync);

static int snd_soc_dapm_add_route(struct snd_soc_codec *codec,
	const char *sink, const char *control, const char *source)
{
	struct snd_soc_dapm_path *path;
	struct snd_soc_dapm_widget *wsource = NULL, *wsink = NULL, *w;
	int ret = 0;

	/* find src and dest widgets */
	list_for_each_entry(w, &codec->dapm_widgets, list) {

		if (!wsink && !(strcmp(w->name, sink))) {
			wsink = w;
			continue;
		}
		if (!wsource && !(strcmp(w->name, source))) {
			wsource = w;
		}
	}

	if (wsource == NULL || wsink == NULL)
		return -ENODEV;

	path = kzalloc(sizeof(struct snd_soc_dapm_path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	path->source = wsource;
	path->sink = wsink;
	INIT_LIST_HEAD(&path->list);
	INIT_LIST_HEAD(&path->list_source);
	INIT_LIST_HEAD(&path->list_sink);

	/* check for external widgets */
	if (wsink->id == snd_soc_dapm_input) {
		if (wsource->id == snd_soc_dapm_micbias ||
			wsource->id == snd_soc_dapm_mic ||
			wsink->id == snd_soc_dapm_line ||
			wsink->id == snd_soc_dapm_output)
			wsink->ext = 1;
	}
	if (wsource->id == snd_soc_dapm_output) {
		if (wsink->id == snd_soc_dapm_spk ||
			wsink->id == snd_soc_dapm_hp ||
			wsink->id == snd_soc_dapm_line ||
			wsink->id == snd_soc_dapm_input)
			wsource->ext = 1;
	}

	/* connect static paths */
	if (control == NULL) {
		list_add(&path->list, &codec->dapm_paths);
		list_add(&path->list_sink, &wsink->sources);
		list_add(&path->list_source, &wsource->sinks);
		path->connect = 1;
		return 0;
	}

	/* connect dynamic paths */
	switch(wsink->id) {
	case snd_soc_dapm_adc:
	case snd_soc_dapm_dac:
	case snd_soc_dapm_pga:
	case snd_soc_dapm_input:
	case snd_soc_dapm_output:
	case snd_soc_dapm_micbias:
	case snd_soc_dapm_vmid:
	case snd_soc_dapm_pre:
	case snd_soc_dapm_post:
		list_add(&path->list, &codec->dapm_paths);
		list_add(&path->list_sink, &wsink->sources);
		list_add(&path->list_source, &wsource->sinks);
		path->connect = 1;
		return 0;
	case snd_soc_dapm_mux:
		ret = dapm_connect_mux(codec, wsource, wsink, path, control,
			&wsink->kcontrols[0]);
		if (ret != 0)
			goto err;
		break;
	case snd_soc_dapm_switch:
	case snd_soc_dapm_mixer:
		ret = dapm_connect_mixer(codec, wsource, wsink, path, control);
		if (ret != 0)
			goto err;
		break;
	case snd_soc_dapm_hp:
	case snd_soc_dapm_mic:
	case snd_soc_dapm_line:
	case snd_soc_dapm_spk:
		list_add(&path->list, &codec->dapm_paths);
		list_add(&path->list_sink, &wsink->sources);
		list_add(&path->list_source, &wsource->sinks);
		path->connect = 0;
		return 0;
	}
	return 0;

err:
	printk(KERN_WARNING "asoc: no dapm match for %s --> %s --> %s\n", source,
		control, sink);
	kfree(path);
	return ret;
}

/**
 * snd_soc_dapm_add_routes - Add routes between DAPM widgets
 * @codec: codec
 * @route: audio routes
 * @num: number of routes
 *
 * Connects 2 dapm widgets together via a named audio path. The sink is
 * the widget receiving the audio signal, whilst the source is the sender
 * of the audio signal.
 *
 * Returns 0 for success else error. On error all resources can be freed
 * with a call to snd_soc_card_free().
 */
int snd_soc_dapm_add_routes(struct snd_soc_codec *codec,
			    const struct snd_soc_dapm_route *route, int num)
{
	int i, ret;

	for (i = 0; i < num; i++) {
		ret = snd_soc_dapm_add_route(codec, route->sink,
					     route->control, route->source);
		if (ret < 0) {
			printk(KERN_ERR "Failed to add route %s->%s\n",
			       route->source,
			       route->sink);
			return ret;
		}
		route++;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_add_routes);

/**
 * snd_soc_dapm_new_widgets - add new dapm widgets
 * @codec: audio codec
 *
 * Checks the codec for any new dapm widgets and creates them if found.
 *
 * Returns 0 for success.
 */
int snd_soc_dapm_new_widgets(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_widget *w;

	list_for_each_entry(w, &codec->dapm_widgets, list)
	{
		if (w->new)
			continue;

		switch(w->id) {
		case snd_soc_dapm_switch:
		case snd_soc_dapm_mixer:
			dapm_new_mixer(codec, w);
			break;
		case snd_soc_dapm_mux:
			dapm_new_mux(codec, w);
			break;
		case snd_soc_dapm_adc:
		case snd_soc_dapm_dac:
		case snd_soc_dapm_pga:
			dapm_new_pga(codec, w);
			break;
		case snd_soc_dapm_input:
		case snd_soc_dapm_output:
		case snd_soc_dapm_micbias:
		case snd_soc_dapm_spk:
		case snd_soc_dapm_hp:
		case snd_soc_dapm_mic:
		case snd_soc_dapm_line:
		case snd_soc_dapm_vmid:
		case snd_soc_dapm_pre:
		case snd_soc_dapm_post:
			break;
		}
		w->new = 1;
	}

	dapm_power_widgets(codec, SND_SOC_DAPM_STREAM_NOP);
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_widgets);

/**
 * snd_soc_dapm_get_volsw - dapm mixer get callback
 * @kcontrol: mixer control
 * @uinfo: control element information
 *
 * Callback to get the value of a dapm mixer control.
 *
 * Returns 0 for success.
 */
int snd_soc_dapm_get_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int shift = mc->shift;
	unsigned int rshift = mc->rshift;
	int max = mc->max;
	unsigned int invert = mc->invert;
	unsigned int mask = (1 << fls(max)) - 1;

	/* return the saved value if we are powered down */
	if (widget->id == snd_soc_dapm_pga && !widget->power) {
		ucontrol->value.integer.value[0] = widget->saved_value;
		return 0;
	}

	ucontrol->value.integer.value[0] =
		(snd_soc_read(widget->codec, reg) >> shift) & mask;
	if (shift != rshift)
		ucontrol->value.integer.value[1] =
			(snd_soc_read(widget->codec, reg) >> rshift) & mask;
	if (invert) {
		ucontrol->value.integer.value[0] =
			max - ucontrol->value.integer.value[0];
		if (shift != rshift)
			ucontrol->value.integer.value[1] =
				max - ucontrol->value.integer.value[1];
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_volsw);

/**
 * snd_soc_dapm_put_volsw - dapm mixer set callback
 * @kcontrol: mixer control
 * @uinfo: control element information
 *
 * Callback to set the value of a dapm mixer control.
 *
 * Returns 0 for success.
 */
int snd_soc_dapm_put_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int shift = mc->shift;
	unsigned int rshift = mc->rshift;
	int max = mc->max;
	unsigned int mask = (1 << fls(max)) - 1;
	unsigned int invert = mc->invert;
	unsigned short val, val2, val_mask;
	int ret;

	val = (ucontrol->value.integer.value[0] & mask);

	if (invert)
		val = max - val;
	val_mask = mask << shift;
	val = val << shift;
	if (shift != rshift) {
		val2 = (ucontrol->value.integer.value[1] & mask);
		if (invert)
			val2 = max - val2;
		val_mask |= mask << rshift;
		val |= val2 << rshift;
	}

	mutex_lock(&widget->codec->mutex);
	widget->value = val;

	/* save volume value if the widget is powered down */
	if (widget->id == snd_soc_dapm_pga && !widget->power) {
		widget->saved_value = val;
		mutex_unlock(&widget->codec->mutex);
		return 1;
	}

	dapm_mixer_update_power(widget, kcontrol, reg, val_mask, val, invert);
	if (widget->event) {
		if (widget->event_flags & SND_SOC_DAPM_PRE_REG) {
			ret = widget->event(widget, kcontrol,
						SND_SOC_DAPM_PRE_REG);
			if (ret < 0) {
				ret = 1;
				goto out;
			}
		}
		ret = snd_soc_update_bits(widget->codec, reg, val_mask, val);
		if (widget->event_flags & SND_SOC_DAPM_POST_REG)
			ret = widget->event(widget, kcontrol,
						SND_SOC_DAPM_POST_REG);
	} else
		ret = snd_soc_update_bits(widget->codec, reg, val_mask, val);

out:
	mutex_unlock(&widget->codec->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_put_volsw);

/**
 * snd_soc_dapm_get_enum_double - dapm enumerated double mixer get callback
 * @kcontrol: mixer control
 * @uinfo: control element information
 *
 * Callback to get the value of a dapm enumerated double mixer control.
 *
 * Returns 0 for success.
 */
int snd_soc_dapm_get_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget = snd_kcontrol_chip(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned short val, bitmask;

	for (bitmask = 1; bitmask < e->max; bitmask <<= 1)
		;
	val = snd_soc_read(widget->codec, e->reg);
	ucontrol->value.enumerated.item[0] = (val >> e->shift_l) & (bitmask - 1);
	if (e->shift_l != e->shift_r)
		ucontrol->value.enumerated.item[1] =
			(val >> e->shift_r) & (bitmask - 1);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_enum_double);

/**
 * snd_soc_dapm_put_enum_double - dapm enumerated double mixer set callback
 * @kcontrol: mixer control
 * @uinfo: control element information
 *
 * Callback to set the value of a dapm enumerated double mixer control.
 *
 * Returns 0 for success.
 */
int snd_soc_dapm_put_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget = snd_kcontrol_chip(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned short val, mux;
	unsigned short mask, bitmask;
	int ret = 0;

	for (bitmask = 1; bitmask < e->max; bitmask <<= 1)
		;
	if (ucontrol->value.enumerated.item[0] > e->max - 1)
		return -EINVAL;
	mux = ucontrol->value.enumerated.item[0];
	val = mux << e->shift_l;
	mask = (bitmask - 1) << e->shift_l;
	if (e->shift_l != e->shift_r) {
		if (ucontrol->value.enumerated.item[1] > e->max - 1)
			return -EINVAL;
		val |= ucontrol->value.enumerated.item[1] << e->shift_r;
		mask |= (bitmask - 1) << e->shift_r;
	}

	mutex_lock(&widget->codec->mutex);
	widget->value = val;
	dapm_mux_update_power(widget, kcontrol, mask, mux, val, e);
	if (widget->event) {
		if (widget->event_flags & SND_SOC_DAPM_PRE_REG) {
			ret = widget->event(widget,
				kcontrol, SND_SOC_DAPM_PRE_REG);
			if (ret < 0)
				goto out;
		}
		ret = snd_soc_update_bits(widget->codec, e->reg, mask, val);
		if (widget->event_flags & SND_SOC_DAPM_POST_REG)
			ret = widget->event(widget,
				kcontrol, SND_SOC_DAPM_POST_REG);
	} else
		ret = snd_soc_update_bits(widget->codec, e->reg, mask, val);

out:
	mutex_unlock(&widget->codec->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_put_enum_double);

/**
 * snd_soc_dapm_new_control - create new dapm control
 * @codec: audio codec
 * @widget: widget template
 *
 * Creates a new dapm control based upon the template.
 *
 * Returns 0 for success else error.
 */
int snd_soc_dapm_new_control(struct snd_soc_codec *codec,
	const struct snd_soc_dapm_widget *widget)
{
	struct snd_soc_dapm_widget *w;

	if ((w = dapm_cnew_widget(widget)) == NULL)
		return -ENOMEM;

	w->codec = codec;
	INIT_LIST_HEAD(&w->sources);
	INIT_LIST_HEAD(&w->sinks);
	INIT_LIST_HEAD(&w->list);
	list_add(&w->list, &codec->dapm_widgets);

	/* machine layer set ups unconnected pins and insertions */
	w->connected = 1;
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_control);

/**
 * snd_soc_dapm_new_controls - create new dapm controls
 * @codec: audio codec
 * @widget: widget array
 * @num: number of widgets
 *
 * Creates new DAPM controls based upon the templates.
 *
 * Returns 0 for success else error.
 */
int snd_soc_dapm_new_controls(struct snd_soc_codec *codec,
	const struct snd_soc_dapm_widget *widget,
	int num)
{
	int i, ret;

	for (i = 0; i < num; i++) {
		ret = snd_soc_dapm_new_control(codec, widget);
		if (ret < 0) {
			printk(KERN_ERR
			       "ASoC: Failed to create DAPM control %s: %d\n",
			       widget->name, ret);
			return ret;
		}
		widget++;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_controls);


/**
 * snd_soc_dapm_stream_event - send a stream event to the dapm core
 * @codec: audio codec
 * @stream: stream name
 * @event: stream event
 *
 * Sends a stream event to the dapm core. The core then makes any
 * necessary widget power changes.
 *
 * Returns 0 for success else error.
 */
int snd_soc_dapm_stream_event(struct snd_soc_codec *codec,
	char *stream, int event)
{
	struct snd_soc_dapm_widget *w;

	if (stream == NULL)
		return 0;

	mutex_lock(&codec->mutex);
	list_for_each_entry(w, &codec->dapm_widgets, list)
	{
		if (!w->sname)
			continue;
		pr_debug("widget %s\n %s stream %s event %d\n",
			 w->name, w->sname, stream, event);
		if (strstr(w->sname, stream)) {
			switch(event) {
			case SND_SOC_DAPM_STREAM_START:
				w->active = 1;
				break;
			case SND_SOC_DAPM_STREAM_STOP:
				w->active = 0;
				break;
			case SND_SOC_DAPM_STREAM_SUSPEND:
				if (w->active)
					w->suspend = 1;
				w->active = 0;
				break;
			case SND_SOC_DAPM_STREAM_RESUME:
				if (w->suspend) {
					w->active = 1;
					w->suspend = 0;
				}
				break;
			case SND_SOC_DAPM_STREAM_PAUSE_PUSH:
				break;
			case SND_SOC_DAPM_STREAM_PAUSE_RELEASE:
				break;
			}
		}
	}
	mutex_unlock(&codec->mutex);

	dapm_power_widgets(codec, event);
	dump_dapm(codec, __func__);
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_stream_event);

/**
 * snd_soc_dapm_set_bias_level - set the bias level for the system
 * @socdev: audio device
 * @level: level to configure
 *
 * Configure the bias (power) levels for the SoC audio device.
 *
 * Returns 0 for success else error.
 */
int snd_soc_dapm_set_bias_level(struct snd_soc_device *socdev,
				enum snd_soc_bias_level level)
{
	struct snd_soc_codec *codec = socdev->codec;
	struct snd_soc_card *card = socdev->card;
	int ret = 0;

	if (card->set_bias_level)
		ret = card->set_bias_level(card, level);
	if (ret == 0 && codec->set_bias_level)
		ret = codec->set_bias_level(codec, level);

	return ret;
}

/**
 * snd_soc_dapm_enable_pin - enable pin.
 * @snd_soc_codec: SoC codec
 * @pin: pin name
 *
 * Enables input/output pin and it's parents or children widgets iff there is
 * a valid audio route and active audio stream.
 * NOTE: snd_soc_dapm_sync() needs to be called after this for DAPM to
 * do any widget power switching.
 */
int snd_soc_dapm_enable_pin(struct snd_soc_codec *codec, char *pin)
{
	return snd_soc_dapm_set_pin(codec, pin, 1);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_enable_pin);

/**
 * snd_soc_dapm_disable_pin - disable pin.
 * @codec: SoC codec
 * @pin: pin name
 *
 * Disables input/output pin and it's parents or children widgets.
 * NOTE: snd_soc_dapm_sync() needs to be called after this for DAPM to
 * do any widget power switching.
 */
int snd_soc_dapm_disable_pin(struct snd_soc_codec *codec, char *pin)
{
	return snd_soc_dapm_set_pin(codec, pin, 0);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_disable_pin);

/**
 * snd_soc_dapm_nc_pin - permanently disable pin.
 * @codec: SoC codec
 * @pin: pin name
 *
 * Marks the specified pin as being not connected, disabling it along
 * any parent or child widgets.  At present this is identical to
 * snd_soc_dapm_disable_pin() but in future it will be extended to do
 * additional things such as disabling controls which only affect
 * paths through the pin.
 *
 * NOTE: snd_soc_dapm_sync() needs to be called after this for DAPM to
 * do any widget power switching.
 */
int snd_soc_dapm_nc_pin(struct snd_soc_codec *codec, char *pin)
{
	return snd_soc_dapm_set_pin(codec, pin, 0);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_nc_pin);

/**
 * snd_soc_dapm_get_pin_status - get audio pin status
 * @codec: audio codec
 * @pin: audio signal pin endpoint (or start point)
 *
 * Get audio pin status - connected or disconnected.
 *
 * Returns 1 for connected otherwise 0.
 */
int snd_soc_dapm_get_pin_status(struct snd_soc_codec *codec, char *pin)
{
	struct snd_soc_dapm_widget *w;

	list_for_each_entry(w, &codec->dapm_widgets, list) {
		if (!strcmp(w->name, pin))
			return w->connected;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_pin_status);

/**
 * snd_soc_dapm_free - free dapm resources
 * @socdev: SoC device
 *
 * Free all dapm widgets and resources.
 */
void snd_soc_dapm_free(struct snd_soc_device *socdev)
{
	struct snd_soc_codec *codec = socdev->codec;

	snd_soc_dapm_sys_remove(socdev->dev);
	dapm_free_widgets(codec);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_free);

/* Module information */
MODULE_AUTHOR("Liam Girdwood, lrg@slimlogic.co.uk");
MODULE_DESCRIPTION("Dynamic Audio Power Management core for ALSA SoC");
MODULE_LICENSE("GPL");
