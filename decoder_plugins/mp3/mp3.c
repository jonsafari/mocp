/*
 * MOC - music on console
 * Copyright (C) 2002-2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* This code was writen od the basis of madlld.c (C) by Bertrand Petit 
 * including code from xmms-mad (C) by Sam Clegg and winamp plugin for madlib
 * (C) by Robert Leslie.
 */

/* FIXME: there can be a bit of silence in mp3 at the end or at the
 * beginning. If you hear gaps between files, it's the file's fault.
 * Can we strip this silence? */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <mad.h>
#include <id3tag.h>
#include <pthread.h>

#define DEBUG

#include "main.h"
#include "log.h"
#include "xing.h"
#include "audio.h"
#include "decoder.h"
#include "io.h"

#define INPUT_BUFFER	(32 * 1024)

struct mp3_data
{
	struct io_stream *io_stream;
	unsigned long bitrate;
	unsigned int freq;
	short channels;
	signed long duration; /* Total time of the file in seconds.
				 used for seeking. */
	off_t size; /* Size of the file */

	unsigned char in_buff[INPUT_BUFFER];

	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	int skip_frames; /* how much frames to skip (after seeking) */
	
	int ok; /* was this stream successfuly opened? */
	struct decoder_error error;
};

/* Fill in the mad buffer, return number of bytes read, 0 on eof or error */
static size_t fill_buff (struct mp3_data *data)
{
	size_t remaining;
	ssize_t read_size;
	unsigned char *read_start;
	
	if (data->stream.next_frame != NULL) {
		remaining = data->stream.bufend - data->stream.next_frame;
		memmove (data->in_buff, data->stream.next_frame, remaining);
		read_start = data->in_buff + remaining;
		read_size = INPUT_BUFFER - remaining;
	}
	else {
		read_start = data->in_buff;
		read_size = INPUT_BUFFER;
		remaining = 0;
	}

	read_size = io_read (data->io_stream, read_start, read_size);
	if (read_size < 0) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"read error: %s", io_strerror(data->io_stream));
		return 0;
	}
	else if (read_size == 0)
		return 0;

	mad_stream_buffer(&data->stream, data->in_buff, read_size + remaining);
	data->stream.error = 0;

	return read_size;
}

static char *get_tag (struct id3_tag *tag, const char *what)
{
	struct id3_frame *frame;
	union id3_field *field;
	const id3_ucs4_t *ucs4;
	char *comm = NULL;

	frame = id3_tag_findframe (tag, what, 0);
	if (frame && (field = &frame->fields[1])) {
		ucs4 = id3_field_getstrings (field, 0);
		if (ucs4)
			comm = id3_ucs4_latin1duplicate (ucs4);
	}

	return comm;
}

static int count_time_internal (struct mp3_data *data)
{
	struct xing xing;
	unsigned long bitrate = 0;
	int has_xing = 0;
	int is_vbr = 0;
	int num_frames = 0;
	mad_timer_t duration = mad_timer_zero;
	struct mad_header header;
	int good_header = 0; /* Have we decoded any header? */

	mad_header_init (&header);
	xing_init (&xing);

	/* There are three ways of calculating the length of an mp3:
	  1) Constant bitrate: One frame can provide the information
		 needed: # of frames and duration. Just see how long it
		 is and do the division.
	  2) Variable bitrate: Xing tag. It provides the number of 
		 frames. Each frame has the same number of samples, so
		 just use that.
	  3) All: Count up the frames and duration of each frames
		 by decoding each one. We do this if we've no other
		 choice, i.e. if it's a VBR file with no Xing tag.
	*/

	while (1) {
		
		/* Fill the input buffer if needed */
		if (data->stream.buffer == NULL ||
			data->stream.error == MAD_ERROR_BUFLEN) {
			if (!fill_buff(data))
				break;
		}

		if (mad_header_decode(&header, &data->stream) == -1) {
			if (MAD_RECOVERABLE(data->stream.error))
				continue;
			else if (data->stream.error == MAD_ERROR_BUFLEN)
				continue;
			else {
				debug ("Can't decode header: %s",
						mad_stream_errorstr(
							&data->stream));
				break;
			}
		}

		good_header = 1;

		/* Limit xing testing to the first frame header */
		if (!num_frames++) {
			if (xing_parse(&xing, data->stream.anc_ptr,
						data->stream.anc_bitlen)
					!= -1) {
				is_vbr = 1;

				debug ("Has XING header");
				
				if (xing.flags & XING_FRAMES) {
					has_xing = 1;
					num_frames = xing.frames;
					break;
				}
				debug ("XING header doesn't contain number of "
						"frames.");
			}
		}				

		/* Test the first n frames to see if this is a VBR file */
		if (!is_vbr && !(num_frames > 20)) {
			if (bitrate && header.bitrate != bitrate) {
				debug ("Detected VBR after %d frames",
						num_frames);
				is_vbr = 1;
			}
			else
				bitrate = header.bitrate;
		}
		
		/* We have to assume it's not a VBR file if it hasn't already
		 * been marked as one and we've checked n frames for different
		 * bitrates */
		else if (!is_vbr) {
			debug ("Fixed rate MP3");
			break;
		}
			
		mad_timer_add (&duration, header.duration);
	}

	if (!good_header)
		return -1;

	if (!is_vbr) {
		/* time in seconds */
		double time = (data->size * 8.0) / (header.bitrate);
		
		double timefrac = (double)time - ((long)(time));

		/* samples per frame */
		long nsamples = 32 * MAD_NSBSAMPLES(&header);

		/* samplerate is a constant */
		num_frames = (long) (time * header.samplerate / nsamples);

		mad_timer_set(&duration, (long)time, (long)(timefrac*100),
				100);
	}
		
	else if (has_xing) {
		mad_timer_multiply (&header.duration, num_frames);
		duration = header.duration;
	}
	else {
		/* the durations have been added up, and the number of frames
		   counted. We do nothing here. */
		debug ("Counted duration by counting frames durations in "
				"VBR file.");
	}

	mad_header_finish(&header);

	debug ("MP3 time: %ld", mad_timer_count (duration, MAD_UNITS_SECONDS));

	return mad_timer_count (duration, MAD_UNITS_SECONDS);
}

static struct mp3_data *mp3_open_internal (const char *file,
		const int buffered)
{
	struct mp3_data *data;

	data = (struct mp3_data *)xmalloc (sizeof(struct mp3_data));
	data->ok = 0;
	decoder_error_init (&data->error);

	/* Reset information about the file */
	data->freq = 0;
	data->channels = 0;
	data->skip_frames = 0;
	data->bitrate = -1;

	/* Open the file */
	data->io_stream = io_open (file, buffered);
	if (io_ok(data->io_stream)) {
		data->ok = 1;
		
		data->size = io_file_size (data->io_stream);

		mad_stream_init (&data->stream);
		mad_frame_init (&data->frame);
		mad_synth_init (&data->synth);
		
		data->duration = count_time_internal (data);
		mad_frame_mute (&data->frame);
		data->stream.next_frame = NULL;
		data->stream.sync = 0;
		data->stream.error = MAD_ERROR_NONE;

		if (io_seek(data->io_stream, SEEK_SET, 0) == (off_t)-1) {
			decoder_error (&data->error, ERROR_FATAL, 0,
						"seek failed");
			io_close (data->io_stream);
			mad_stream_finish (&data->stream);
			mad_frame_finish (&data->frame);
			mad_synth_finish (&data->synth);
			data->ok = 0;
		}

		data->stream.error = MAD_ERROR_BUFLEN;
	}
	else {
		decoder_error (&data->error, ERROR_FATAL, 0, "Can't open: %s",
				io_strerror(data->io_stream));
		io_close (data->io_stream);
	}

	return data;
}

static void *mp3_open (const char *file)
{
	return mp3_open_internal(file, 1);
}

static void mp3_close (void *void_data)
{
	struct mp3_data *data = (struct mp3_data *)void_data;

	if (data->ok)
		io_close (data->io_stream);
	decoder_error_clear (&data->error);
	free (data);
}

/* Get the time for mp3 file, return -1 on error. */
/* Adapted from mpg321. */
static int count_time (const char *file)
{
	struct mp3_data *data;
	int time;
	
	debug ("Processing file %s", file);

	if (!(data = mp3_open_internal(file, 0)))
		time = -1;
	else
		time = data->duration;
	mp3_close (data);

	return time;
}

/* Fill info structure with data from the id3 tag */
static void mp3_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{

	if (tags_sel & TAGS_COMMENTS) {
		struct id3_tag *tag;
		struct id3_file *id3file;
		char *track = NULL;
		
		id3file = id3_file_open (file_name, ID3_FILE_MODE_READONLY);
		if (!id3file)
			return;
		tag = id3_file_tag (id3file);
		if (tag) {
			info->artist = get_tag (tag, ID3_FRAME_ARTIST);
			info->title = get_tag (tag, ID3_FRAME_TITLE);
			info->album = get_tag (tag, ID3_FRAME_ALBUM);
			track = get_tag (tag, ID3_FRAME_TRACK);
			
			if (track) {
				char *end;
				
				info->track = strtol (track, &end, 10);
				if (end == track)
					info->track = -1;
				free (track);
			}
		}
		id3_file_close (id3file);
	}

	if (tags_sel & TAGS_TIME)
		info->time = count_time (file_name);
}

/* Scale PCM data to 16 bit unsigned */
static inline signed int scale (mad_fixed_t sample)
{	
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));

	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;

	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static int put_output (char *buf, int buf_len, struct mad_pcm *pcm,
		struct mad_header *header)
{
	unsigned int nsamples;
	mad_fixed_t const *left_ch, *right_ch;
	int olen;
	int pos = 0;

	nsamples = pcm->length;
	left_ch = pcm->samples[0];
	right_ch = pcm->samples[1];
	olen = nsamples * MAD_NCHANNELS (header) * 2;

	if (olen > buf_len) {
		logit ("PCM buffer to small!");
		return 0;
	}
	
	while (nsamples--) {
		signed int sample;

		/* output sample(s) in 16-bit signed little-endian PCM */
		sample = scale (*left_ch++);
		
#ifdef WORDS_BIGENDIAN
		buf[pos++] = (sample >> 8) & 0xff;
		buf[pos++] = (sample >> 0) & 0xff;
#else
		buf[pos++] = (sample >> 0) & 0xff;
		buf[pos++] = (sample >> 8) & 0xff;
#endif

		if (MAD_NCHANNELS (header) == 2) {
			sample = scale (*right_ch++);
#ifdef WORDS_BIGENDIAN
			buf[pos++] = (sample >> 8) & 0xff;
			buf[pos++] = (sample >> 0) & 0xff;
#else
			buf[pos++] = (sample >> 0) & 0xff;
			buf[pos++] = (sample >> 8) & 0xff;
#endif
		}
	}

	return olen;
}

static int mp3_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct mp3_data *data = (struct mp3_data *)void_data;

	decoder_error_clear (&data->error);

	while (1) {

		/* Fill the input buffer if needed */
		if (data->stream.buffer == NULL ||
			data->stream.error == MAD_ERROR_BUFLEN) {
			if (!fill_buff(data))
				return 0;
		}
		
		if (mad_frame_decode (&data->frame, &data->stream)) {
			if (MAD_RECOVERABLE(data->stream.error)) {

				/* Ignore LOSTSYNC */
				if (data->stream.error == MAD_ERROR_LOSTSYNC)
					continue;

				if (!data->skip_frames)
					decoder_error (&data->error,
							ERROR_STREAM, 0,
							"Broken frame: %s",
							mad_stream_errorstr(
								&data->stream)
							);
				continue;
			}
			else if (data->stream.error == MAD_ERROR_BUFLEN)
				continue;
			else {
				decoder_error (&data->error, ERROR_FATAL, 0,
						"Broken frame: %s",
						mad_stream_errorstr(
							&data->stream)
						);
				return 0;
			}
		}

		if (data->skip_frames) {
			data->skip_frames--;
			continue;
		}

		/* Sound parameters. */
		if (!(sound_params->rate = data->frame.header.samplerate)) {
			decoder_error (&data->error, ERROR_FATAL, 0,
					"Broken file: information about the"
					" frequency couldn't be read.");
			return 0;
		}
		sound_params->channels = MAD_NCHANNELS(&data->frame.header);
		sound_params->format = 2;
		
		/* Change of the bitrate? */
		if (data->frame.header.bitrate != data->bitrate) {
			if ((data->bitrate = data->frame.header.bitrate) == 0) {
				decoder_error (&data->error, ERROR_FATAL, 0,
						"Broken file: information"
						" about the bitrate couldn't"
						" be read.");
				return 0;
			}
		}
		
		mad_synth_frame (&data->synth, &data->frame);
		mad_stream_sync (&data->stream);

		return put_output (buf, buf_len, &data->synth.pcm,
				&data->frame.header);
	}
}

static int mp3_seek (void *void_data, int sec)
{
	struct mp3_data *data = (struct mp3_data *)void_data;
	int new_position;

	if (sec >= data->duration)
		return -1;
	else if (sec < 0)
		sec = 0;

	new_position = ((double) sec /
			(double) data->duration) * data->size;

	debug ("Seeking to %d (%d byte)", sec, new_position);

	if (new_position < 0)
		new_position = 0;
	else if (new_position >= data->size)
		return -1;
	
	if (io_seek(data->io_stream, new_position, SEEK_SET) == -1) {
		logit ("seek to %d failed", new_position);
		return -1;
	}
	
	data->stream.error = MAD_ERROR_BUFLEN;

	mad_frame_mute (&data->frame);
	mad_synth_mute (&data->synth);

	data->stream.sync = 0;
	data->stream.next_frame = NULL;

	data->skip_frames = 2;

	return sec;
}

static int mp3_get_bitrate (void *void_data)
{
	struct mp3_data *data = (struct mp3_data *)void_data;

	return data->bitrate / 1000;
}

static int mp3_get_duration (void *void_data)
{
	struct mp3_data *data = (struct mp3_data *)void_data;
	
	return data->duration;
}

static void mp3_get_name (const char *file ATTR_UNUSED, char buf[4])
{
	strcpy (buf, "MP3");
}

static int mp3_our_format_ext (const char *ext)
{
	return !strcasecmp(ext, "mp3");
}

static void mp3_get_error (void *prv_data, struct decoder_error *error)
{
	struct mp3_data *data = (struct mp3_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct decoder mp3_decoder = {
	DECODER_API_VERSION,
	mp3_open,
	NULL,
	NULL,
	mp3_close,
	mp3_decode,
	mp3_seek,
	mp3_info,
	mp3_get_bitrate,
	mp3_get_duration,
	mp3_get_error,
	mp3_our_format_ext,
	mp3_get_name
};

struct decoder *plugin_init ()
{
	return &mp3_decoder;
}
