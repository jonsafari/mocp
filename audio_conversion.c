/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Code for conversion between float and fixed point types is based on
 * libsamplerate:
 * Copyright (C) 2002-2004 Erik de Castro Lopo <erikd@mega-nerd.com>
 */

/* For future: audio conversion should be performed in order:
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
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_SAMPLERATE
# include <samplerate.h>
#endif

#define DEBUG

#include "audio_conversion.h"
#include "common.h"
#include "log.h"
#include "options.h"
#include "compat.h"

/* Byte order conversion */
/* TODO: use functions from byteswap.h if available */
#define SWAP_INT16(l) ((int16_t) \
			( (l & 0xff) << 8) | (((l) >> 8) & 0xff))
#define SWAP_INT32(l) ((int32_t) (\
			(((l) & 0x000000ff) << 24) | \
			(((l) & 0x0000ff00) << 8) | \
			(((l) & 0x00ff0000) >> 8) | \
			(((l) & 0xff000000) >> 24) \
			) \
		)

#if 0
#ifdef WORDS_BIGENDIAN
# define INT16_BE_TO_NE(l)	(l)
# define INT16_LE_TO_NE(l)	SWAP_INT16 (l)
# define INT32_BE_TO_NE(l)	(l)
# define INT32_LE_TO_NE(l)	SWAP_INT32 (l)
#else
# define INT16_BE_TO_NE(l)	SWAP_INT16 (l)
# define INT16_LE_TO_NE(l)	(l)
# define INT32_BE_TO_NE(l)	SWAP_INT32 (l)
# define INT32_LE_TO_NE(l)	(l)
#endif

/* The byte order conversion is symetric, so this is true: */
#define INT16_NE_TO_LE		INT16_LE_TO_NE (l)
#define INT32_NE_TO_BE		INT32_BE_TO_NE (l)
#endif

static void float_to_s8 (const float *in, char *out, const size_t samples)
{
	size_t i;

	assert (in != NULL);
	assert (out != NULL);

	for (i = 0; i < samples; i++) {
		float f = in[i] * INT32_MAX;
		
		if (f >= INT32_MAX)
			out[i] = INT8_MAX;
		else if (f <= INT32_MIN)
			out[i] = INT8_MIN;
		else {
#ifdef HAVE_LRINTF
			out[i] = lrintf(f) >> 24;
#else
			out[i] = (int)f >> 24;
#endif
		}
		
	}
}

static void float_to_s16 (const float *in, char *out,
		const size_t samples)
{
	size_t i;

	assert (in != NULL);
	assert (out != NULL);

	for (i = 0; i < samples; i++) {
		int16_t *out_val = (int16_t *)(out + i*2);
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

static void float_to_s32 (const float *in, char *out,
		const size_t samples)
{
	size_t i;

	/* maximum and minimum values of 32-bit samples */
	const int S32_MAX = (1 << 23) - 1;
	const int S32_MIN = -(1 << 23);

	assert (in != NULL);
	assert (out != NULL);

	for (i = 0; i < samples; i++) {
		int32_t *out_val = (int32_t *)(out + i*sizeof(int32_t));
		float f = in[i] * S32_MAX;
		
		if (f >= S32_MAX)
			*out_val = S32_MAX;
		else if (f <= S32_MIN)
			*out_val = S32_MIN;
		else {
#ifdef HAVE_LRINTF
			*out_val = lrintf(f) << 8;
#else
			*out_val = (int32_t)f << 8;
#endif
		}
		
	}
}

static void s8_to_float (const char *in, float *out,
		const size_t samples)
{
	size_t i;

	assert (in != NULL);
	assert (out != NULL);
	
	for (i = 0; i < samples; i++)
		out[i] = *in++ / (float)(INT8_MAX + 1);
}

static void s16_to_float (const char *in, float *out,
		const size_t samples)
{
	size_t i;
	const int16_t *in_16 = (int16_t *)in;

	assert (in != NULL);
	assert (out != NULL);
	
	for (i = 0; i < samples; i++)
		out[i] = *in_16++ / (float)(INT16_MAX + 1);
}

static void s32_to_float (const char *in, float *out,
		const size_t samples)
{
	size_t i;
	const int32_t *in_32 = (int32_t *)in;

	assert (in != NULL);
	assert (out != NULL);
	
	for (i = 0; i < samples; i++)
		out[i] = *in_32++ / ((float)INT32_MAX + 1.0);
}

/* Convert fixed point samples in format fmt (size in bytes) to float.
 * Size of converted sound is put in new_size. Returned memory is malloc()ed. */
static float *fixed_to_float (const char *buf, const size_t size,
		const long fmt, size_t *new_size)
{
	float *out = NULL;
	char fmt_name[SFMT_STR_MAX];
	
	switch (fmt & SFMT_MASK_FORMAT) {
		case SFMT_S8:
			*new_size = sizeof(float) * size;
			out = (float *)xmalloc (*new_size);
			s8_to_float (buf, out, size);
			break;
		case SFMT_S16:
			*new_size = sizeof(float) * size / 2;
			out = (float *)xmalloc (*new_size);
			s16_to_float (buf, out, size / 2);
			break;
		case SFMT_S32:
			*new_size = sizeof(float) * size / 4;
			out = (float *)xmalloc (*new_size);
			s32_to_float (buf, out, size / 4);
			break;
		default:
			 error ("Can't convert %s to float!",
					 sfmt_str(fmt, fmt_name,
						 sizeof(fmt_name)));
	}

	return out;
}

/* Convert float samples to fixed point format fmt. Returned samples of size
 * new_size bytes is malloc()ed. */
static char *float_to_fixed (const float *buf, const size_t samples,
		const long fmt, size_t *new_size)
{
	char fmt_name[SFMT_STR_MAX];
	char *new_snd = NULL;
	
	switch (fmt & SFMT_MASK_FORMAT) {
		case SFMT_S8:
			*new_size = samples;
			new_snd = (char *) xmalloc(samples);
			float_to_s8 (buf, new_snd, samples);
			break;
		case SFMT_S16:
			*new_size = samples * 2;
			new_snd = (char *) xmalloc(*new_size);
			float_to_s16 (buf, new_snd, samples);
			break;
		case SFMT_S32:
			*new_size = samples * 4;
			new_snd = (char *) xmalloc(*new_size);
			float_to_s32 (buf, new_snd, samples);
			break;
		default:
			error ("Can't convert from float to %s",
					sfmt_str(fmt, fmt_name,
						sizeof(fmt_name)));
			abort ();
	}

	return new_snd;
}

static void change_sign_8 (uint8_t *buf, const size_t samples)
{
	size_t i;

	for (i = 0; i < samples; i++)
		*buf++ ^= 1 << 7;
}
static void change_sign_16 (uint16_t *buf, const size_t samples)
{
	size_t i;

	for (i = 0; i < samples; i++)
		*buf++ ^= 1 << 15;
}

static void change_sign_32 (uint32_t *buf, const size_t samples)
{
	size_t i;

	for (i = 0; i < samples; i++)
		*buf++ ^= 1 << 31;
}

/* Change the signs of samples in format *fmt.  Also changes fmt to the new
 * format. */
static void change_sign (char *buf, const size_t size, long *fmt)
{
	char fmt_name[SFMT_STR_MAX];
	
	switch (*fmt & SFMT_MASK_FORMAT) {
		case SFMT_S8:
		case SFMT_U8:
			change_sign_8 ((uint8_t *)buf, size);
			if (*fmt & SFMT_S8)
				*fmt = sfmt_set_fmt (*fmt, SFMT_U8);
			else
				*fmt = sfmt_set_fmt (*fmt, SFMT_S8);
			break;
		case SFMT_S16:
		case SFMT_U16:
			change_sign_16 ((uint16_t *)buf, size / 2);
			if (*fmt & SFMT_S16)
				*fmt = sfmt_set_fmt (*fmt, SFMT_U16);
			else
				*fmt = sfmt_set_fmt (*fmt, SFMT_S16);
			break;
		case SFMT_S32:
		case SFMT_U32:
			change_sign_32 ((uint32_t *)buf, size/4);
			if (*fmt & SFMT_S32)
				*fmt = sfmt_set_fmt (*fmt, SFMT_U32);
			else
				*fmt = sfmt_set_fmt (*fmt, SFMT_S32);
			break;
		default:
			error ("Request for changing sign of unknown format: %s",
					sfmt_str(*fmt, fmt_name,
						sizeof(fmt_name)));
			abort ();
	}
}

static void int16_bswap_array (int16_t *buf, const size_t num)
{
	size_t i;

	for (i = 0; i < num; i++)
		buf[i] = SWAP_INT16 (buf[i]);
}

static void int32_bswap_array (int32_t *buf, const size_t num)
{
	size_t i;

	for (i = 0; i < num; i++)
		buf[i] = SWAP_INT32 (buf[i]);
}

/* Swap endianness of fixed point samples. */
static void swap_endian (char *buf, const size_t size, const long fmt)
{
	if ((fmt & (SFMT_S8 | SFMT_U8 | SFMT_FLOAT)))
		return;

	switch (fmt & SFMT_MASK_FORMAT) {
		case SFMT_S16:
		case SFMT_U16:
			int16_bswap_array ((int16_t *)buf, size / 2);
			break;
		case SFMT_S32:
		case SFMT_U32:
			int32_bswap_array ((int32_t *)buf, size / 4);
			break;
		default:
			error ("Can't convert to native endian!");
			abort (); /* we can't do any smarter thing */
	}
}

/* Initialize the audio_conversion structure for conversion between parameters
 * from and to. Return 0 on error. */
int audio_conv_new (struct audio_conversion *conv,
		const struct sound_params *from,
		const struct sound_params *to)
{
	assert (from->rate != to->rate || from->fmt != to->fmt
			|| from->channels != to->channels);
	
	if (from->channels != to->channels) {

		/* the only conversion we can do */
		if (!(from->channels == 1 && to->channels == 2)) {
			error ("Can't change number of channels");
			return 0;
		}
	}

	if (from->rate != to->rate) {
#ifdef HAVE_SAMPLERATE
		int err;
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
			error ("Can't resample from %dHz to %dHz: %s",
					from->rate, to->rate,
					src_strerror(err));
			return 0;
		}
#else
		error ("Resampling not supported.");
		return 0;
#endif
	}
#ifdef HAVE_SAMPLERATE
	else
		conv->src_state = NULL;
#endif
	
	conv->from = *from;
	conv->to = *to;

#ifdef HAVE_SAMPLERATE
	conv->resample_buf = NULL;
	conv->resample_buf_nsamples = 0;
#endif

	return 1;
}

#ifdef HAVE_SAMPLERATE
static float *resample_sound (struct audio_conversion *conv, const float *buf,
		const size_t samples, const int nchannels,
		size_t *resampled_samples)
{
	SRC_DATA resample_data;
	float *output;
	float *new_input_start;
	int output_samples = 0;

	resample_data.end_of_input = 0;
	resample_data.src_ratio = conv->to.rate / (double)conv->from.rate;
	
	resample_data.input_frames = samples / nchannels
		+ conv->resample_buf_nsamples / nchannels;
	resample_data.output_frames = resample_data.input_frames
		* resample_data.src_ratio;

	if (conv->resample_buf) {
		conv->resample_buf = (float *)xrealloc (conv->resample_buf,
				sizeof(float) * resample_data.input_frames
				* nchannels);
		new_input_start = conv->resample_buf
			+ conv->resample_buf_nsamples;
	}
	else {
		conv->resample_buf = (float *)xmalloc (sizeof(float)
				* resample_data.input_frames
				* nchannels);
		new_input_start = conv->resample_buf;
	}

	output = (float *)xmalloc (sizeof(float) * resample_data.output_frames
				* nchannels);

	/*debug ("Resampling %lu bytes of data by ratio %f", (unsigned long)size,
			resample_data.src_ratio);*/
	
	memcpy (new_input_start, buf, samples * sizeof(float));
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
			* nchannels;
		resample_data.input_frames -= resample_data.input_frames_used;
		resample_data.data_out += resample_data.output_frames_gen
			* nchannels;
		resample_data.output_frames -= resample_data.output_frames_gen;
		output_samples += resample_data.output_frames_gen * nchannels;
	} while (resample_data.input_frames && resample_data.output_frames_gen
			&& resample_data.output_frames);

	*resampled_samples = output_samples;

	if (resample_data.input_frames) {
		conv->resample_buf_nsamples = resample_data.input_frames
			* nchannels;
		if (conv->resample_buf != resample_data.data_in) {
			float *new;

			new = (float *)xmalloc (sizeof(float) *
					conv->resample_buf_nsamples);
			memcpy (new, resample_data.data_in, sizeof(float) *
					conv->resample_buf_nsamples);
			free (conv->resample_buf);
			conv->resample_buf = new;
		}
	}
	else {
		free (conv->resample_buf);
		conv->resample_buf = NULL;
		conv->resample_buf_nsamples = 0;
	}
	
	return output;
}
#endif

/* Double the channels from */
static char *mono_to_stereo (const char *mono, const size_t size,
		const long format)
{
	int Bps = sfmt_Bps (format);
	size_t i;
	char *stereo;

	stereo = (char *)xmalloc (size * 2);

	for (i = 0; i < size; i += Bps) {
		memcpy (stereo + (i * 2), mono + i, Bps);
		memcpy (stereo + (i * 2 + Bps), mono + i, Bps);
	}

	return stereo;
}

static int16_t *s32_to_s16 (int32_t *in, const size_t samples)
{
	size_t i;
	int16_t *new;

	new = (int16_t *)xmalloc (samples * 2);

	for (i = 0; i < samples; i++)
		new[i] = in[i] >> 16;

	return new;
}

static uint16_t *u32_to_u16 (uint32_t *in, const size_t samples)
{
	size_t i;
	uint16_t *new;

	new = (uint16_t *)xmalloc (samples * 2);

	for (i = 0; i < samples; i++)
		new[i] = in[i] >> 16;

	return new;
}

/* Do the sound conversion.  buf of length size is the sample buffer to
 * convert and the size of the converted sound is put into *conv_len.
 * Return the converted sound in malloc()ed memory. */
char *audio_conv (struct audio_conversion *conv, const char *buf,
		const size_t size, size_t *conv_len)
{
	char *curr_sound;
	long curr_sfmt = conv->from.fmt;
	
	*conv_len = size;

	curr_sound = (char *)xmalloc (size);
	memcpy (curr_sound, buf, size);

	if (!(curr_sfmt & SFMT_NE)) {
		swap_endian (curr_sound, *conv_len, curr_sfmt);
		curr_sfmt = sfmt_set_endian (curr_sfmt, SFMT_NE);
	}

	/* Special case (optimization): if we only need to convert 32bit samples
	 * to 16bit, we can do it very simply and quickly. */
	if (((curr_sfmt & SFMT_MASK_FORMAT) == SFMT_S32
				|| (curr_sfmt & SFMT_MASK_FORMAT)
				== SFMT_U32)
			&& (((conv->to.fmt & SFMT_MASK_FORMAT) == SFMT_S16)
				|| ((conv->to.fmt & SFMT_MASK_FORMAT)
					== SFMT_U16))
			&& conv->from.rate == conv->to.rate) {
		char *new_sound;
		
		if ((curr_sfmt & SFMT_MASK_FORMAT) == SFMT_S32) {
			new_sound = (char *)s32_to_s16 ((int32_t *)curr_sound,
					*conv_len / 4);
			curr_sfmt = sfmt_set_fmt (curr_sfmt, SFMT_S16);
		}
		else {
			new_sound = (char *)u32_to_u16 ((uint32_t *)curr_sound,
					*conv_len / 4);
			curr_sfmt = sfmt_set_fmt (curr_sfmt, SFMT_U16);
		}
		
		if (curr_sound != buf)
			free (curr_sound);
		curr_sound = new_sound;
		*conv_len /= 2;

		logit ("Fast conversion!");
	}

	/* convert to float if necessary */
	if ((conv->from.rate != conv->to.rate
				|| conv->to.fmt == SFMT_FLOAT
				|| !sfmt_same_bps(conv->to.fmt, curr_sfmt))
			&& conv->from.fmt != SFMT_FLOAT) {
		char *new_sound;
		
		new_sound = (char *)fixed_to_float (curr_sound, *conv_len,
				curr_sfmt, conv_len);
		curr_sfmt = sfmt_set_fmt (curr_sfmt, SFMT_FLOAT);
		assert (new_sound != NULL);
		
		if (curr_sound != buf)
			free (curr_sound);
		curr_sound = new_sound;
	}
	
#ifdef HAVE_SAMPLERATE
	if (conv->from.rate != conv->to.rate) {
		char *new_sound = (char *)resample_sound (conv,
				(float *)curr_sound,
				*conv_len / sizeof(float), conv->to.channels,
				conv_len);
		*conv_len *= sizeof(float);
		if (curr_sound != buf)
			free (curr_sound);
		curr_sound = new_sound;
	}
#endif

	if ((curr_sfmt & SFMT_MASK_FORMAT)
			!= (conv->to.fmt & SFMT_MASK_FORMAT)) {

		if (sfmt_same_bps(curr_sfmt, conv->to.fmt))
			change_sign (curr_sound, size, &curr_sfmt);
		else {
			char *new_sound;
			
			assert (curr_sfmt & SFMT_FLOAT);
			
			new_sound = float_to_fixed ((float *)curr_sound,
					*conv_len / sizeof(float),
					conv->to.fmt, conv_len);
			curr_sfmt = sfmt_set_fmt (curr_sfmt, conv->to.fmt);
			assert (new_sound != NULL);
		
			if (curr_sound != buf)
				free (curr_sound);
			curr_sound = new_sound;
		}
	}

	if ((curr_sfmt & SFMT_MASK_ENDIANES)
			!= (conv->to.fmt & SFMT_MASK_ENDIANES)) {
		swap_endian (curr_sound, *conv_len, curr_sfmt);
		curr_sfmt = sfmt_set_endian (curr_sfmt,
				conv->to.fmt & SFMT_MASK_ENDIANES);
	}

	if (conv->from.channels == 1 && conv->to.channels == 2) {
		char *new_sound;

		new_sound = mono_to_stereo (curr_sound, *conv_len,
				conv->from.fmt);
		*conv_len *= 2;
				
		if (curr_sound != buf)
			free (curr_sound);
		curr_sound = new_sound;
	}
	
	return curr_sound;
}

void audio_conv_destroy (struct audio_conversion *conv)
{
	assert (conv != NULL);

#ifdef HAVE_SAMPLERATE
	if (conv->resample_buf)
		free (conv->resample_buf);
	if (conv->src_state)
		src_delete (conv->src_state);
#endif
}
