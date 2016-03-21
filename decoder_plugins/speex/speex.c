/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Based on (and includes code from) ogg123 copyright by
 * Stan Seibert <volsung@xiph.org> AND OTHER CONTRIBUTORS
 * and speexdec copyright by Jean-Marc Valin
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <assert.h>
#include <speex/speex.h>
#include <speex/speex_header.h>
#include <speex/speex_stereo.h>
#include <speex/speex_callbacks.h>
#include <ogg/ogg.h>

/*#define DEBUG*/

#include "common.h"
#include "decoder.h"
#include "io.h"
#include "audio.h"
#include "log.h"

/* Use speex's audio enhancement feature */
#define ENHANCE_AUDIO 1

struct spx_data
{
	struct io_stream *stream;
	struct decoder_error error;
	int ok;			/* was the stream opened succesfully? */

	SpeexBits bits;
	void *st;		/* speex decoder state */
	ogg_sync_state oy;
	ogg_page og;
	ogg_packet op;
	ogg_stream_state os;
	SpeexStereoState stereo;
	SpeexHeader *header;

	int frame_size;
	int rate;
	int nchannels;
	int frames_per_packet;
	int bitrate;

	int16_t *output;
	int output_start;
	int output_left;
	char *comment_packet;
	int comment_packet_len;
};

static void *process_header (struct spx_data *data)
{
	void *st;
	const SpeexMode *mode;
	int modeID;
	SpeexCallback callback;
	int enhance = ENHANCE_AUDIO;

	data->header = speex_packet_to_header ((char*)data->op.packet,
			data->op.bytes);
	if (!data->header) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't open speex file: can't read header");
		return NULL;
	}

	if (data->header->mode >= SPEEX_NB_MODES) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't open speex file: Mode number %"PRId32
				" does not exist in this version",
				data->header->mode);
		return NULL;
	}

	modeID = data->header->mode;
	mode = speex_mode_list[modeID];

	if (mode->bitstream_version < data->header->mode_bitstream_version) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't open speex file: The file was encoded "
				"with a newer version of Speex.");
		return NULL;
	}

	if (mode->bitstream_version > data->header->mode_bitstream_version) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't open speex file: The file was encoded "
				"with an older version of Speex.");
		return NULL;
	}

	st = speex_decoder_init (mode);
	speex_decoder_ctl(st, SPEEX_SET_ENH, &enhance);
	speex_decoder_ctl(st, SPEEX_GET_FRAME_SIZE, &data->frame_size);

	callback.callback_id = SPEEX_INBAND_STEREO;
	callback.func = speex_std_stereo_request_handler;
	callback.data = &data->stereo;
	speex_decoder_ctl(st, SPEEX_SET_HANDLER, &callback);
	speex_decoder_ctl(st, SPEEX_SET_SAMPLING_RATE, &data->header->rate);

	return st;
}

/* Read the speex header. Return 0 on error. */
static int read_speex_header (struct spx_data *data)
{
	int packet_count = 0;
	int stream_init = 0;
	char *buf;
	ssize_t nb_read;
	int header_packets = 2;

	while (packet_count < header_packets) {

		/* Get the ogg buffer for writing */
		buf = ogg_sync_buffer (&data->oy, 200);

		/* Read bitstream from input file */
		nb_read = io_read (data->stream, buf, 200);

		if (nb_read < 0) {
			decoder_error (&data->error, ERROR_FATAL, 0,
					"Can't open speex file: IO error: %s",
					io_strerror(data->stream));
			return 0;
		}

		if (nb_read == 0) {
			decoder_error (&data->error, ERROR_FATAL, 0,
					"Can't open speex header");
			return 0;
		}

		ogg_sync_wrote (&data->oy, nb_read);

		/* Loop for all complete pages we got (most likely only one) */
		while (ogg_sync_pageout(&data->oy, &data->og) == 1) {

			if (stream_init == 0) {
				ogg_stream_init(&data->os,
						ogg_page_serialno(&data->og));
				stream_init = 1;
			}

			/* Add page to the bitstream */
			ogg_stream_pagein (&data->os, &data->og);

			/* Extract all available packets FIXME: EOS! */
			while (ogg_stream_packetout(&data->os, &data->op) == 1) {

				/* If first packet, process as Speex header */
				if (packet_count == 0) {
					data->st = process_header (data);

					if (!data->st) {
						ogg_stream_clear (&data->os);
						return 0;
					}

					data->rate = data->header->rate;
					data->nchannels
						= data->header->nb_channels;
					data->frames_per_packet
						= data->header->frames_per_packet;
					/*data->vbr = data->header->vbr; */

					if (!data->frames_per_packet)
						data->frames_per_packet=1;

					data->output = xmalloc (data->frame_size *
							data->nchannels *
							data->frames_per_packet *
							sizeof(int16_t));
					data->output_start = 0;
					data->output_left = 0;

					header_packets += data->header->extra_headers;
				}
				else if (packet_count == 1) {
					data->comment_packet_len
						= data->op.bytes;
					data->comment_packet = xmalloc (
							sizeof(char) *
							data->comment_packet_len);
					memcpy (data->comment_packet,
							data->op.packet,
							data->comment_packet_len);
				}

				packet_count++;
			}
		}
	}

	return 1;
}

static struct spx_data *spx_open_internal (struct io_stream *stream)
{
	struct spx_data *data;
	SpeexStereoState stereo = SPEEX_STEREO_STATE_INIT;

	data = (struct spx_data *)xmalloc (sizeof(struct spx_data));

	decoder_error_init (&data->error);
	data->stream = stream;

	data->st = NULL;
	data->stereo = stereo;
	data->header = NULL;
	data->output = NULL;
	data->comment_packet = NULL;
	data->bitrate = -1;
	ogg_sync_init (&data->oy);
	speex_bits_init (&data->bits);

	if (!read_speex_header(data)) {
		ogg_sync_clear (&data->oy);
		speex_bits_destroy (&data->bits);
		data->ok = 0;
	}
	else
		data->ok = 1;

	return data;
}

static void *spx_open (const char *file)
{
	struct io_stream *stream;
	struct spx_data *data;

	stream = io_open (file, 1);
	if (io_ok (stream))
		data = spx_open_internal (stream);
	else {
		data = (struct spx_data *)xmalloc (sizeof(struct spx_data));
		data->stream = stream;
		data->header = NULL;
		decoder_error_init (&data->error);
		decoder_error (&data->error, ERROR_STREAM, 0,
				"Can't open file: %s", io_strerror(stream));
		data->ok = 0;
	}

	return data;
}

static void *spx_open_stream (struct io_stream *stream)
{
	return spx_open_internal (stream);
}

static int spx_can_decode (struct io_stream *stream)
{
	char buf[36];

	if (io_peek(stream, buf, 36) == 36 && !memcmp(buf, "OggS", 4)
			&& !memcmp(buf + 28, "Speex   ", 8))
		return 1;

	return 0;
}

static void spx_close (void *prv_data)
{
	struct spx_data *data = (struct spx_data *)prv_data;

	if (data->ok) {
		if (data->st)
			speex_decoder_destroy (data->st);
		if (data->comment_packet)
			free (data->comment_packet);
		if (data->output)
			free (data->output);
		speex_bits_destroy (&data->bits);
		ogg_stream_clear (&data->os);
		ogg_sync_clear (&data->oy);
	}

	io_close (data->stream);
	decoder_error_clear (&data->error);

	free (data->header);
	free (data);
}

#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
                           ((buf[base+2]<<16)&0xff0000)| \
                           ((buf[base+1]<<8)&0xff00)| \
  	           	    (buf[base]&0xff))

static void parse_comment (const char *str, struct file_tags *tags)
{
	if (!strncasecmp(str, "title=", strlen ("title=")))
		tags->title = xstrdup(str + strlen ("title="));
	else if (!strncasecmp(str, "artist=", strlen ("artist=")))
		tags->artist = xstrdup (str + strlen ("artist="));
	else if (!strncasecmp(str, "album=", strlen ("album=")))
		tags->album = xstrdup (str + strlen ("album="));
	else if (!strncasecmp(str, "tracknumber=", strlen ("tracknumber=")))
		tags->track = atoi (str	+ strlen ("tracknumber="));
	else if (!strncasecmp(str, "track=", strlen ("track=")))
		tags->track = atoi (str	+ strlen ("track="));
}

static void get_comments (struct spx_data *data, struct file_tags *tags)
{
	if (data->comment_packet && data->comment_packet_len >= 8) {
		char *c = data->comment_packet;
		int len, i, nb_fields;
		char *end;
		char *temp = NULL;
		int temp_len = 0;

		/* Parse out vendor string */
		end = c + data->comment_packet_len;
		len = readint(c, 0);
		c += 4;

		if (c + len > end) {
			logit ("Broken comment");
			return;
		}

		c += len;
		if (c + 4 > end) {
			logit ("Broken comment");
			return;
		}

		nb_fields = readint (c, 0);
		c += 4;

		for (i = 0; i < nb_fields; i++) {
			if (c + 4 > end) {
				if (temp)
					free (temp);
				logit ("Broken comment");
				return;
			}

			len = readint (c, 0);
			c += 4;
			if (c + len > end) {
				logit ("Broken comment");
				if (temp)
					free (temp);
				return;
			}

			if (temp_len < len + 1) {
				temp_len = len + 1;
				if (temp)
					temp = xrealloc (temp, sizeof(char) *
							temp_len);
				else
					temp = xmalloc (sizeof(char) *
							temp_len);
			}

			strncpy (temp, c, len);
			temp[len] = '\0';
			debug ("COMMENT: '%s'", temp);
			parse_comment (temp, tags);

			c += len;
		}

		if (temp)
			free(temp);
	}
}

static void get_more_data (struct spx_data *data)
{
	char *buf;
	ssize_t nb_read;

	buf = ogg_sync_buffer (&data->oy, 200);
	nb_read = io_read (data->stream, buf, 200);
	ogg_sync_wrote (&data->oy, nb_read);
}

static int count_time (struct spx_data *data)
{
	ogg_int64_t last_granulepos = 0;

	/* Seek to somewhere near the last page */
	if (io_file_size(data->stream) > 10000) {
		debug ("Seeking near the end");
		if (io_seek(data->stream, -10000, SEEK_END) == -1)
			logit ("Seeking failed, scanning whole file");
		ogg_sync_reset (&data->oy);
	}

	/* Read granulepos from the last packet */
	while (!io_eof(data->stream)) {

		/* Sync to page and read it */
		while (!io_eof(data->stream)) {
			if (ogg_sync_pageout(&data->oy, &data->og) == 1) {
				debug ("Sync");
				break;
			}

			if (!io_eof(data->stream)) {
				debug ("Need more data");
				get_more_data (data);
			}
		}

		/* We have last packet */
		if (io_eof(data->stream))
			break;

		last_granulepos = ogg_page_granulepos (&data->og);
	}

	return last_granulepos / data->rate;
}

/* Fill info structure with data from spx comments */
static void spx_info (const char *file_name, struct file_tags *tags,
		const int tags_sel)
{
	struct io_stream *s;

	s = io_open (file_name, 0);
	if (io_ok (s)) {
		struct spx_data *data = spx_open_internal (s);

		if (data->ok) {
			if (tags_sel & TAGS_COMMENTS)
				get_comments (data, tags);
			if (tags_sel & TAGS_TIME)
				tags->time = count_time (data);
		}

		spx_close (data);
	}
	else
		io_close (s);
}

static int spx_seek (void *prv_data, int sec)
{
	struct spx_data *data = (struct spx_data *)prv_data;
	off_t begin = 0, end, old_pos;

	assert (sec >= 0);

	end = io_file_size (data->stream);
	if (end == -1)
		return -1;
	old_pos = io_tell (data->stream);

	debug ("Seek request to %ds", sec);

	while (1) {
		off_t middle = (end + begin) / 2;
		ogg_int64_t granule_pos;
		int position_seconds;

		debug ("Seek to %"PRId64, middle);

		if (io_seek(data->stream, middle, SEEK_SET) == -1) {
			io_seek (data->stream, old_pos, SEEK_SET);
			ogg_stream_reset (&data->os);
			ogg_sync_reset (&data->oy);
			return -1;
		}

		debug ("Syncing...");

		/* Sync to page and read it */
		ogg_sync_reset (&data->oy);
		while (!io_eof(data->stream)) {
			if (ogg_sync_pageout(&data->oy, &data->og) == 1) {
				debug ("Sync");
				break;
			}

			if (!io_eof(data->stream)) {
				debug ("Need more data");
				get_more_data (data);
			}
		}

		if (io_eof(data->stream)) {
			debug ("EOF when syncing");
			return -1;
		}

		granule_pos = ogg_page_granulepos(&data->og);
		position_seconds = granule_pos / data->rate;

		debug ("We are at %ds", position_seconds);

		if (position_seconds == sec) {
			ogg_stream_pagein (&data->os, &data->og);
			debug ("We have it at granulepos %"PRId64, granule_pos);
			break;
		}
		else if (sec < position_seconds) {
			end = middle;
			debug ("going back");
		}
		else {
			begin = middle;
			debug ("going forward");
		}

		debug ("begin - end %"PRId64" - %"PRId64, begin, end);

		if (end - begin <= 200) {

			/* Can't find the exact position. */
			sec = position_seconds;
			break;
		}
	}

	ogg_sync_reset (&data->oy);
	ogg_stream_reset (&data->os);

	return sec;
}

static int spx_decode (void *prv_data, char *sound_buf, int nbytes,
		struct sound_params *sound_params)
{
	struct spx_data *data = (struct spx_data *)prv_data;
  	int bytes_requested = nbytes;
	int16_t *out = (int16_t *)sound_buf;

	sound_params->channels = data->nchannels;
	sound_params->rate = data->rate;
	sound_params->fmt = SFMT_S16 | SFMT_NE;

	while (nbytes) {
		int j;

		/* First see if there is anything left in the output buffer and
		 * empty it out */
		if (data->output_left > 0) {
			int to_copy = nbytes / sizeof(int16_t);

			to_copy = MIN(data->output_left, to_copy);

			memcpy (out, data->output + data->output_start,
					to_copy * sizeof(int16_t));

			out += to_copy;
			data->output_start += to_copy;
			data->output_left -= to_copy;

			nbytes -= to_copy * sizeof(int16_t);
		}
		else if (ogg_stream_packetout (&data->os, &data->op) == 1) {
			int16_t *temp_output = data->output;

			/* Decode some more samples */

			/* Copy Ogg packet to Speex bitstream */
			speex_bits_read_from (&data->bits,
					(char*)data->op.packet, data->op.bytes);

			for (j = 0; j < data->frames_per_packet; j++) {

				/* Decode frame */
				speex_decode_int (data->st, &data->bits,
						temp_output);
				if (data->nchannels == 2)
					speex_decode_stereo_int (temp_output,
							data->frame_size,
							&data->stereo);

				speex_decoder_ctl (data->st, SPEEX_GET_BITRATE,
						&data->bitrate);
				/*data->samples_decoded += data->frame_size;*/

				temp_output += data->frame_size *
					data->nchannels;
			}

			/*logit ("Read %d bytes from page", data->frame_size *
					data->nchannels *
					data->frames_per_packet);*/

			data->output_start = 0;
			data->output_left = data->frame_size *
				data->nchannels * data->frames_per_packet;
		}
		else if (ogg_sync_pageout(&data->oy, &data->og) == 1) {

			/* Read in another ogg page */
			ogg_stream_pagein (&data->os, &data->og);
			debug ("Granulepos: %"PRId64, ogg_page_granulepos(&data->og));

		}
		else if (!io_eof(data->stream)) {
			/* Finally, pull in some more data and try again on the next pass */
			get_more_data (data);
		}
		else
			break;
	}

	return bytes_requested - nbytes;
}

#if 0
static int spx_current_tags (void *prv_data, struct file_tags *tags)
{
	struct spx_data *data = (struct spx_data *)prv_data;

	return 0;
}
#endif

static int spx_get_bitrate (void *prv_data)
{
	struct spx_data *data = (struct spx_data *)prv_data;

	return data->bitrate / 1000;
}

static int spx_get_duration (void *unused ATTR_UNUSED)
{
	/*struct spx_data *data = (struct spx_data *)prv_data;*/

	return -1;
}

static struct io_stream *spx_get_stream (void *prv_data)
{
	struct spx_data *data = (struct spx_data *)prv_data;

	return data->stream;
}

static void spx_get_name (const char *unused ATTR_UNUSED, char buf[4])
{
	strcpy (buf, "SPX");
}

static int spx_our_format_ext (const char *ext)
{
	return !strcasecmp (ext, "spx");
}

static void spx_get_error (void *prv_data, struct decoder_error *error)
{
	struct spx_data *data = (struct spx_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static int spx_our_mime (const char *mime)
{
	return !strcasecmp (mime, "audio/x-speex")
		|| !strncasecmp (mime, "audio/x-speex;", 14)
		|| !strcasecmp (mime, "audio/speex")
		|| !strncasecmp (mime, "audio/speex;", 12);
}

static struct decoder spx_decoder = {
	DECODER_API_VERSION,
	NULL,
	NULL,
	spx_open,
	spx_open_stream,
	spx_can_decode,
	spx_close,
	spx_decode,
	spx_seek,
	spx_info,
	spx_get_bitrate,
	spx_get_duration,
	spx_get_error,
	spx_our_format_ext,
	spx_our_mime,
	spx_get_name,
	NULL /*spx_current_tags*/,
	spx_get_stream,
	NULL
};

struct decoder *plugin_init ()
{
	return &spx_decoder;
}
