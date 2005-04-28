/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Code for convertion between float and fixed point types is based on
 * libsamplerate:
 * Copyright (C) 2002-2004 Erik de Castro Lopo <erikd@mega-nerd.com>
 */

/* For future: audio convertion should be performed in order:
 * channels -> rate -> format
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* for lrintf() */
#define _ISOC9X_SOURCE  1
#define _ISOC99_SOURCE  1
#define __USE_ISOC9X    1
#define __USE_ISOC99    1

#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_SAMPLERATE
# include <samplerate.h>
#endif

#define DEBUG

#include "audio_convertion.h"
#include "main.h"
#include "log.h"
#include "options.h"

static void audio_conv_float_to_s16 (const float *in, char *out,
		const size_t samples)
{
	size_t i;

	assert (in != NULL);
	assert (out != NULL);

	for (i = 0; i < samples; i++) {
		short *out_val = (short *)(out + i*2);
		float f = in[i] * INT32_MAX;
		
		if (f >= INT32_MAX)
			*out_val = INT16_MAX;
		else if (f <= INT32_MIN)
			*out_val = INT16_MIN;
		else {
#ifdef HAVE_LRINTF
			*out_val = lrintf(f) >> 16;
#else
			*out_val = ((int)f >> 16);
#endif
		}
		
	}
}

static void audio_conv_s16_to_float (const char *in, float *out,
		const size_t samples)
{
	size_t i;

	assert (in != NULL);
	assert (out != NULL);
	
	for (i = 0; i < samples; i++)
		out[i] = *((short *)(in + i*2)) / (float)INT16_MAX;
}

/* Initialize the audio_convertion structure for convertion between parameters
 * from and to. Return 0 on error. */
int audio_conv_new (struct audio_convertion *conv,
		const struct sound_params *from,
		const struct sound_params *to)
{
	int err;
	
	assert (from->rate != to->rate || from->format != to->format
			|| from->channels != to->channels);
	
	if (from->format != to->format) {
		error ("PCM format convertion not supported");
		return 0;
	}

	if (from->channels != to->channels) {
		error ("Can't change number of channels");
		return 0;
	}

	if (from->rate != to->rate) {
#ifdef HAVE_SAMPLERATE
		int resample_type = -1;
		char *method = options_get_str ("ResampleMethod");

		if (!strcasecmp(method, "SincBestQuality"))
			resample_type = SRC_SINC_BEST_QUALITY;
		else if (!strcasecmp(method, "SincMediumQuality"))
			resample_type = SRC_SINC_MEDIUM_QUALITY;
		else if (!strcasecmp(method, "SincFastest"))
			resample_type = SRC_SINC_FASTEST;
		else if (!strcasecmp(method, "ZeroOrderHold"))
			resample_type = SRC_ZERO_ORDER_HOLD;
		else if (!strcasecmp(method, "Linear"))
			resample_type = SRC_LINEAR;
		else
			fatal ("Bad ResampleMethod option");
		
		conv->src_state = src_new (resample_type, to->channels, &err);
		if (!conv->src_state) {
			error ("Can't resammple from %dHz to %dHz: %s",
					from->rate, to->rate,
					src_strerror(err));
			return 0;
		}
#else
		error ("Resampling not supported.");
		return 0;
#endif
	}
	else
		conv->src_state = NULL;
	
	conv->from = *from;
	conv->to = *to;
	conv->resample_buf = NULL;
	conv->resample_buf_nsamples = 0;

	return 1;
}

#ifdef HAVE_SAMPLERATE
static char *resample_sound (struct audio_convertion *conv, const char *buf,
		const size_t size, size_t *resampled_size)
{
	SRC_DATA resample_data;
	float *output;
	float *new_input_start;
	int output_samples = 0;
	char *resampled;

	resample_data.end_of_input = 0;
	resample_data.src_ratio = conv->to.rate / (double)conv->from.rate;
	
	resample_data.input_frames = size / conv->from.format
		/ conv->to.channels
		+ conv->resample_buf_nsamples / conv->to.channels;
	resample_data.output_frames = resample_data.input_frames
		* resample_data.src_ratio;

	if (conv->resample_buf) {
		conv->resample_buf = (float *)xrealloc (conv->resample_buf,
				sizeof(float) * resample_data.input_frames
				* conv->to.channels);
		new_input_start = conv->resample_buf
			+ conv->resample_buf_nsamples;
	}
	else {
		conv->resample_buf = (float *)xmalloc (sizeof(float)
				* resample_data.input_frames
				* conv->to.channels);
		new_input_start = conv->resample_buf;
	}

	output = (float *)xmalloc (sizeof(float) * resample_data.input_frames
				* conv->to.channels * resample_data.src_ratio);

	/*debug ("Resampling %lu bytes of data by ratio %f", (unsigned long)size,
			resample_data.src_ratio);*/
	
	if (conv->from.format == 2)
		audio_conv_s16_to_float (buf, new_input_start, size/2);
	else if (conv->from.format == 1)
		/* TODO */;
	else
		fatal ("Unsupported format for conversion: %dbps",
				conv->from.format * 8);

	resample_data.data_in = conv->resample_buf;
	resample_data.data_out = output;

	do {
		int err;

		if ((err = src_process(conv->src_state, &resample_data))) {
			error ("Can't resample: %s", src_strerror(err));
			free (output);
			return NULL;
		}

		resample_data.data_in += resample_data.input_frames_used
			* conv->to.channels;
		resample_data.input_frames -= resample_data.input_frames_used;
		resample_data.data_out += resample_data.output_frames_gen
			* conv->to.channels;
		resample_data.output_frames -= resample_data.output_frames_gen;
		output_samples += resample_data.output_frames_gen
			* conv->to.channels;
	} while (resample_data.input_frames && resample_data.output_frames_gen
			&& resample_data.output_frames);

	resampled = (char *)xmalloc (sizeof(short) * output_samples);
	if (conv->from.format == 2) {
		audio_conv_float_to_s16 (output, resampled, output_samples);
		*resampled_size = output_samples * 2;
	}
#if 0
	else
		/* TODO */;
#endif

	if (resample_data.input_frames) {
		conv->resample_buf_nsamples = resample_data.input_frames
			* conv->to.channels;
		if (conv->resample_buf != resample_data.data_in)
			memmove (conv->resample_buf, resample_data.data_in,
					sizeof(float)
					* conv->resample_buf_nsamples);
		conv->resample_buf = (float *) xrealloc (conv->resample_buf,
				sizeof(float) * conv->resample_buf_nsamples);
	}
	else {
		free (conv->resample_buf);
		conv->resample_buf = NULL;
		conv->resample_buf_nsamples = 0;
	}
	
	free (output);

	return resampled;
}
#endif

char *audio_conv (struct audio_convertion *conv, const char *buf,
		const size_t size, size_t *conv_len)
{
#ifdef HAVE_SAMPLERATE
	if (conv->from.rate != conv->to.rate)
		return resample_sound (conv, buf, size, conv_len);
#endif
	return NULL;
}

void audio_conv_destroy (struct audio_convertion *conv)
{
	assert (conv != NULL);

	if (conv->resample_buf)
		free (conv->resample_buf);
	if (conv->src_state)
		src_delete (conv->src_state);
}
