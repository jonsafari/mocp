/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* The code is based on libxmms-flac written by Josh Coalson. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define DEBUG

#include <string.h>
#include <FLAC/all.h>
#include <stdlib.h>
#include <string.h>
#include "playlist.h"
#include "audio.h"
#include "file_types.h"
#include "server.h"
#include "main.h"
#include "log.h"

#define BITRATE_HIST_SEGMENT_MSEC	500
/* 500ms * 50 = 25s should be enough */
#define BITRATE_HIST_SIZE		50

#define MAX_SUPPORTED_CHANNELS		2

#define SAMPLES_PER_WRITE		512
#define SAMPLE_BUFFER_SIZE ((FLAC__MAX_BLOCK_SIZE + SAMPLES_PER_WRITE) * MAX_SUPPORTED_CHANNELS * (24/8))

struct flac_data
{
	FLAC__FileDecoder *decoder;
	int bitrate;
	int abort; /* abort playing (due to an error) */
	
	unsigned length;
	unsigned total_samples;
	
	FLAC__byte sample_buffer[SAMPLE_BUFFER_SIZE];
	unsigned sample_buffer_fill;
	
	/* sound parameters */
	unsigned bits_per_sample;
	unsigned sample_rate;
	unsigned channels;

	FLAC__uint64 last_decode_position;
};

/* Convert FLAC big-endian data into PCM little-endian. */
static size_t pack_pcm_signed (FLAC__byte *data,
		const FLAC__int32 * const input[], unsigned wide_samples,
		unsigned channels, unsigned bps)
{
	FLAC__byte * const start = data;
	FLAC__int32 sample;
	const FLAC__int32 *input_;
	unsigned samples, channel;
	const unsigned bytes_per_sample = bps / 8;
	const unsigned incr = bytes_per_sample * channels;

	for (channel = 0; channel < channels; channel++) {
		samples = wide_samples;
		data = start + bytes_per_sample * channel;
		input_ = input[channel];

		while(samples--) {
			sample = *input_++;

			switch(bps) {
				case 8:
					data[0] = sample ^ 0x80;
					break;

#ifdef WORDS_BIGENDIAN
				case 16:
					data[0] = (FLAC__byte)(sample >> 8);
					data[1] = (FLAC__byte)sample;
					break;
				case 24:
					data[0] = (FLAC__byte)(sample >> 16);
					data[1] = (FLAC__byte)(sample >> 8);
					data[2] = (FLAC__byte)sample;
					break;
#else
				case 24:
					data[2] = (FLAC__byte)(sample >> 16);
					/* fall through */
				case 16:
					data[1] = (FLAC__byte)(sample >> 8);
					data[0] = (FLAC__byte)sample;
#endif
			}

			data += incr;
		}
	}

	debug ("Converted %d bytes", wide_samples * channels * (bps/8));

	return wide_samples * channels * (bps/8);
}

static FLAC__StreamDecoderWriteStatus write_callback (
		const FLAC__FileDecoder *decoder ATTR_UNUSED,
		const FLAC__Frame *frame,
		const FLAC__int32 * const buffer[], void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	const unsigned wide_samples = frame->header.blocksize;

	if (data->abort)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	data->sample_buffer_fill = pack_pcm_signed (
			data->sample_buffer, buffer, wide_samples,
			data->channels, data->bits_per_sample);

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback (const FLAC__FileDecoder *decoder ATTR_UNUSED,
		const FLAC__StreamMetadata *metadata, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;

	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		debug ("Got metadata info");
		
		data->total_samples =
			(unsigned)(metadata->data.stream_info.total_samples
				   & 0xffffffff);
		data->bits_per_sample =
			metadata->data.stream_info.bits_per_sample;
		data->channels = metadata->data.stream_info.channels;
		data->sample_rate = metadata->data.stream_info.sample_rate;
		data->length = data->total_samples / data->sample_rate;
	}
}

static void error_callback (const FLAC__FileDecoder *decoder ATTR_UNUSED,
		FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	
	if (status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC) {
		debug ("Aborting due to error");
		data->abort = 1;
	}
	else
		error ("FLAC: lost sync");
}

static void *flac_open (const char *file)
{
	struct flac_data *data;

	data = (struct flac_data *)xmalloc (sizeof(struct flac_data));
	
	data->bitrate = -1;
	data->abort = 0;
	data->sample_buffer_fill = 0;
	data->last_decode_position = 0;

	if (!(data->decoder = FLAC__file_decoder_new())) {
		error ("FLAC__file_decoder_new() failed");
		free (data);
		return NULL;
	}

	FLAC__file_decoder_set_md5_checking (data->decoder, false);
	if (!FLAC__file_decoder_set_filename(data->decoder, file)) {
		free (data);
		error ("FLAC__file_decoder_set_filename() failed");
		return NULL;
	}
	FLAC__file_decoder_set_metadata_ignore_all (data->decoder);
	FLAC__file_decoder_set_metadata_respond (data->decoder,
			FLAC__METADATA_TYPE_STREAMINFO);
	FLAC__file_decoder_set_client_data (data->decoder, data);
	FLAC__file_decoder_set_metadata_callback (data->decoder,
			metadata_callback);
	FLAC__file_decoder_set_write_callback (data->decoder, write_callback);
	FLAC__file_decoder_set_error_callback (data->decoder, error_callback);

	if (FLAC__file_decoder_init(data->decoder) != FLAC__FILE_DECODER_OK) {
		free (data);
		error ("FLAC__file_decoder_init() failed");
		return NULL;
	}

	if (!FLAC__file_decoder_process_until_end_of_metadata(data->decoder)) {
		free (data);
		error ("FLAC__file_decoder_process_until_end_of_metadata()"
				" failed.");
		return NULL;
	}

	return data;
}

static void flac_close (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	FLAC__file_decoder_finish (data->decoder);
	FLAC__file_decoder_delete (data->decoder);
}

static void fill_tag (FLAC__StreamMetadata_VorbisComment_Entry *comm,
		struct file_tags *tags)
{
	char *name, *value;
	FLAC__byte *eq;
	int value_length;

	eq = memchr (comm->entry, '=', comm->length);
	if (!eq)
		return;

	name = (char *)xmalloc (sizeof(char) * (eq - comm->entry + 1));
	strncpy (name, comm->entry, eq - comm->entry);
	name[eq - comm->entry] = 0;
	value_length = comm->length - (eq - comm->entry + 1);
	
	if (value_length == 0) {
		free (name);
		return;
	}

	value = (char *)xmalloc (sizeof(char) * (value_length + 1));
	strncpy (value, eq + 1, value_length);
	value[value_length] = 0;

	if (!strcasecmp(name, "title"))
		tags->title = value;
	else if (!strcasecmp(name, "artist"))
		tags->artist = value;
	else if (!strcasecmp(name, "album"))
		tags->album = value;
	else if (!strcasecmp(name, "tracknumber")
			|| !strcasecmp(name, "track")) {
		tags->track = atoi (value);
		free (value);
	}
	else
		free (value);
}

static void get_vorbiscomments (const char *filename, struct file_tags *tags)
{
	FLAC__Metadata_SimpleIterator *iterator
		= FLAC__metadata_simple_iterator_new();
	FLAC__bool got_vorbis_comments = false;

	debug ("Reading comments for %s", filename);
	
	if (!iterator) {
		logit ("FLAC__metadata_simple_iterator_new() failed.");
		return;
	}

	if (!FLAC__metadata_simple_iterator_init(iterator, filename, true,
				true)) {
		logit ("FLAC__metadata_simple_iterator_init failed.");
		FLAC__metadata_simple_iterator_delete(iterator);
		return;
	}

	do {
		if (FLAC__metadata_simple_iterator_get_block_type(iterator)
				== FLAC__METADATA_TYPE_VORBIS_COMMENT) {
			FLAC__StreamMetadata *block;
			
			block = FLAC__metadata_simple_iterator_get_block (
					iterator);
			if (block) {
				unsigned i;
				const FLAC__StreamMetadata_VorbisComment *vc
					= &block->data.vorbis_comment;

				for (i = 0; i < vc->num_comments; i++)
					fill_tag (&vc->comments[i], tags);

				FLAC__metadata_object_delete (block);
				got_vorbis_comments = true;
			}
		}
	} while (!got_vorbis_comments
			&& FLAC__metadata_simple_iterator_next(iterator));
	
	FLAC__metadata_simple_iterator_delete(iterator);
}

static void flac_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	if (tags_sel & TAGS_TIME) {
		struct flac_data *data;
		
		if ((data = flac_open(file_name))) {
			info->time = data->length;
			flac_close (data);
		}
	}

	if (tags_sel & TAGS_COMMENTS)
		get_vorbiscomments (file_name, info);
}

static int flac_seek (void *void_data, int sec)
{
	struct flac_data *data = (struct flac_data *)void_data;

	if (sec < 0 || (unsigned)sec > data->length)
		return -1;

	FLAC__uint64 target_sample = (FLAC__uint64)((sec/(double)data->length)
			* (double)data->total_samples);
	
	if (FLAC__file_decoder_seek_absolute(data->decoder, target_sample))
		return sec;
	else {
		logit ("FLAC__file_decoder_seek_absolute() failed.");
		return -1;
	}
}

static int flac_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct flac_data *data = (struct flac_data *)void_data;
	unsigned to_copy;
	int bytes_per_sample;
	FLAC__uint64 decode_position;

	bytes_per_sample = data->bits_per_sample / 8;
	sound_params->format = bytes_per_sample;
	sound_params->rate = data->sample_rate;
	sound_params->channels = data->channels;
	
	if (!data->sample_buffer_fill) {
		debug ("decoding...");
		
		if (FLAC__file_decoder_get_state(data->decoder)
				== FLAC__FILE_DECODER_END_OF_FILE) {
			logit ("EOF");
			return 0;
		}

		if (!FLAC__file_decoder_process_single(data->decoder)) {
			error ("Read error processing frame.");
			return 0;
		}

		/* Count the bitrate */
		if(!FLAC__file_decoder_get_decode_position(data->decoder,
					&decode_position))
			decode_position = 0;
		if (decode_position > data->last_decode_position) {
			int bytes_per_sec = bytes_per_sample * data->sample_rate
				* data->channels;
		 
			data->bitrate = (decode_position
				- data->last_decode_position) * 8.0
				/ (data->sample_buffer_fill
						/ (float)bytes_per_sec)
				/ 1000;
		}

		data->last_decode_position = decode_position;
	}
	else
		debug ("Some date remain in the buffer.");

	debug ("Decoded %d bytes", data->sample_buffer_fill);

	to_copy = MIN((unsigned)buf_len, data->sample_buffer_fill);
	memcpy (buf, data->sample_buffer, to_copy);
	memmove (data->sample_buffer, data->sample_buffer + to_copy,
			data->sample_buffer_fill - to_copy);
	data->sample_buffer_fill -= to_copy;

	return to_copy;
}

static int flac_get_bitrate (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	return data->bitrate;
}

static int flac_get_duration (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	return data->length;
}

static struct decoder_funcs decoder_funcs = {
	flac_open,
	flac_close,
	flac_decode,
	flac_seek,
	flac_info,
	flac_get_bitrate,
	flac_get_duration
};

struct decoder_funcs *flac_get_funcs ()
{
	return &decoder_funcs;
}
