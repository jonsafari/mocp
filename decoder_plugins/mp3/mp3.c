/*
 * MOC - music on console
 * Copyright (C) 2002 - 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* This code was based on madlld.c (C) by Bertrand Petit including code
 * from xmms-mad (C) by Sam Clegg and winamp plugin for madlib (C) by
 * Robert Leslie. */

/* FIXME: there can be a bit of silence in mp3 at the end or at the
 * beginning. If you hear gaps between files, it's the file's fault.
 * Can we strip this silence? */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <mad.h>
#include <id3tag.h>
#include <assert.h>
#ifdef HAVE_ICONV
# include <iconv.h>
#endif

#define DEBUG

#include "common.h"
#include "log.h"
#include "xing.h"
#include "audio.h"
#include "decoder.h"
#include "io.h"
#include "options.h"
#include "files.h"
#include "utf8.h"
#include "rcc.h"

#define INPUT_BUFFER	(32 * 1024)

static iconv_t iconv_id3_fix;

struct mp3_data
{
	struct io_stream *io_stream;
	unsigned long bitrate;
	long avg_bitrate;

	unsigned int freq;
	short channels;
	signed long duration;	/* Total time of the file in seconds
	                           (used for seeking). */
	off_t size;				/* Size of the file */

	unsigned char in_buff[INPUT_BUFFER + MAD_BUFFER_GUARD];

	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	int skip_frames; /* how many frames to skip (after seeking) */

	int ok; /* was this stream successfully opened? */
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

	if (io_eof (data->io_stream)) {
		memset (read_start + read_size, 0, MAD_BUFFER_GUARD);
		read_size += MAD_BUFFER_GUARD;
	}

	mad_stream_buffer(&data->stream, data->in_buff, read_size + remaining);
	data->stream.error = 0;

	return read_size;
}

static char *id3v1_fix (const char *str)
{
	if (iconv_id3_fix != (iconv_t)-1)
		return iconv_str (iconv_id3_fix, str);
	return xstrdup (str);
}

int __unique_frame (struct id3_tag *tag, struct id3_frame *frame)
{
    unsigned int i;

    for (i = 0; i < tag->nframes; i++) {
        if (tag->frames[i] == frame) {
            break;
        }
    }

    for (; i < tag->nframes; i++) {
        if (strcmp(tag->frames[i]->id, frame->id) == 0) {
            return 0;
        }
    }

    return 1;
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
		if (ucs4) {
			/* Workaround for ID3 tags v1/v1.1 where the encoding
			 * is latin1. */
			union id3_field *encoding_field = &frame->fields[0];
			if (((id3_tag_options(tag, 0, 0) & ID3_TAG_OPTION_ID3V1) &&
						__unique_frame(tag, frame))
					|| ((options_get_bool ("EnforceTagsEncoding") &&
							(id3_field_gettextencoding((encoding_field))
							 == ID3_FIELD_TEXTENCODING_ISO_8859_1))))
			{
				char *t;

				comm = (char *)id3_ucs4_latin1duplicate (ucs4);

#ifdef HAVE_RCC
				if (options_get_bool("UseRCC"))
					comm = rcc_reencode (comm);
				else {
#endif /* HAVE_RCC */
					t = comm;
					comm = id3v1_fix (comm);
					free (t);
#ifdef HAVE_RCC
				}
#endif /* HAVE_RCC */
			}
			else
				comm = (char *)id3_ucs4_utf8duplicate (ucs4);
		}
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
	  3) All: Count up the frames and duration of each frame
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
				        mad_stream_errorstr(&data->stream));
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
				debug ("XING header doesn't contain number of frames.");
			}
		}

		/* Test the first n frames to see if this is a VBR file */
		if (!is_vbr && !(num_frames > 20)) {
			if (bitrate && header.bitrate != bitrate) {
				debug ("Detected VBR after %d frames", num_frames);
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

	if (data->size == -1) {
		mad_header_finish(&header);
		return -1;
	}

	if (!is_vbr) {
		/* time in seconds */
		double time = (data->size * 8.0) / (header.bitrate);

		double timefrac = (double)time - ((long)(time));

		/* samples per frame */
		long nsamples = 32 * MAD_NSBSAMPLES(&header);

		/* samplerate is a constant */
		num_frames = (long) (time * header.samplerate / nsamples);

		/* the average bitrate is the constant bitrate */
		data->avg_bitrate = bitrate;

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
		debug ("Counted duration by counting frames durations in VBR file.");
	}

	if (data->avg_bitrate == -1
			&& mad_timer_count(duration, MAD_UNITS_SECONDS) > 0) {
		data->avg_bitrate = data->size
				/ mad_timer_count(duration, MAD_UNITS_SECONDS) * 8;
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
	data->avg_bitrate = -1;

	/* Open the file */
	data->io_stream = io_open (file, buffered);
	if (io_ok(data->io_stream)) {
		data->ok = 1;

		data->size = io_file_size (data->io_stream);

		mad_stream_init (&data->stream);
		mad_frame_init (&data->frame);
		mad_synth_init (&data->synth);

		if (options_get_bool ("MP3IgnoreCRCErrors"))
				mad_stream_options (&data->stream,
					MAD_OPTION_IGNORECRC);

		data->duration = count_time_internal (data);
		mad_frame_mute (&data->frame);
		data->stream.next_frame = NULL;
		data->stream.sync = 0;
		data->stream.error = MAD_ERROR_NONE;

		if (io_seek(data->io_stream, 0, SEEK_SET) == -1) {
			decoder_error (&data->error, ERROR_FATAL, 0, "seek failed");
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
	}

	return data;
}

static void *mp3_open (const char *file)
{
	return mp3_open_internal (file, 1);
}

static void *mp3_open_stream (struct io_stream *stream)
{
	struct mp3_data *data;

	data = (struct mp3_data *)xmalloc (sizeof(struct mp3_data));
	data->ok = 1;
	decoder_error_init (&data->error);

	/* Reset information about the file */
	data->freq = 0;
	data->channels = 0;
	data->skip_frames = 0;
	data->bitrate = -1;
	data->io_stream = stream;
	data->duration = -1;
	data->size = -1;

	mad_stream_init (&data->stream);
	mad_frame_init (&data->frame);
	mad_synth_init (&data->synth);

	if (options_get_bool ("MP3IgnoreCRCErrors"))
			mad_stream_options (&data->stream,
				MAD_OPTION_IGNORECRC);

	return data;
}

static void mp3_close (void *void_data)
{
	struct mp3_data *data = (struct mp3_data *)void_data;

	if (data->ok) {
		mad_stream_finish (&data->stream);
		mad_frame_finish (&data->frame);
		mad_synth_finish (&data->synth);
	}
	io_close (data->io_stream);
	decoder_error_clear (&data->error);
	free (data);
}

/* Get the time for mp3 file, return -1 on error.
 * Adapted from mpg321. */
static int count_time (const char *file)
{
	struct mp3_data *data;
	int time;

	debug ("Processing file %s", file);

	data = mp3_open_internal (file, 0);

	if (!data->ok)
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

static inline int32_t round_sample (mad_fixed_t sample)
{
	sample += 1L << (MAD_F_FRACBITS - 24);

	sample = CLAMP(-MAD_F_ONE, sample, MAD_F_ONE - 1);

	return sample >> (MAD_F_FRACBITS + 1 - 24);
}

static int put_output (char *buf, int buf_len, struct mad_pcm *pcm,
		struct mad_header *header)
{
	unsigned int nsamples;
	mad_fixed_t const *left_ch, *right_ch;
	int olen;

	nsamples = pcm->length;
	left_ch = pcm->samples[0];
	right_ch = pcm->samples[1];
	olen = nsamples * MAD_NCHANNELS (header) * 4;

	if (olen > buf_len) {
		logit ("PCM buffer to small!");
		return 0;
	}

	while (nsamples--) {
		long sample0 = round_sample (*left_ch++);

		buf[0] = 0;
		buf[1] = sample0;
		buf[2] = sample0 >> 8;
		buf[3] = sample0 >> 16;
		buf += 4;

		if (MAD_NCHANNELS(header) == 2) {
			long sample1;

			sample1 = round_sample (*right_ch++);

			buf[0] = 0;
			buf[1] = sample1;
			buf[2] = sample1 >> 8;
			buf[3] = sample1 >> 16;

			buf += 4;
		}
	}

	return olen;
}

/* If the current frame in the stream is an ID3 tag, then swallow it. */
static ssize_t flush_id3_tag (struct mp3_data *data)
{
	size_t remaining;
	ssize_t tag_size;

	remaining = data->stream.bufend - data->stream.next_frame;
	tag_size = id3_tag_query (data->stream.this_frame, remaining);
	if (tag_size > 0) {
		mad_stream_skip (&data->stream, tag_size);
		mad_stream_sync (&data->stream);
	}

	return tag_size;
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
			if (flush_id3_tag (data))
				continue;
			else if (MAD_RECOVERABLE(data->stream.error)) {

				/* Ignore LOSTSYNC */
				if (data->stream.error == MAD_ERROR_LOSTSYNC)
					continue;

				if (!data->skip_frames)
					decoder_error (&data->error, ERROR_STREAM, 0,
							"Broken frame: %s",
							mad_stream_errorstr(&data->stream));
				continue;
			}
			else if (data->stream.error == MAD_ERROR_BUFLEN)
				continue;
			else {
				decoder_error (&data->error, ERROR_FATAL, 0,
						"Broken frame: %s",
						mad_stream_errorstr(&data->stream));
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
		sound_params->fmt = SFMT_S32 | SFMT_LE;

		/* Change of the bitrate? */
		if (data->frame.header.bitrate != data->bitrate) {
			if ((data->bitrate = data->frame.header.bitrate) == 0) {
				decoder_error (&data->error, ERROR_FATAL, 0,
						"Broken file: information about the"
						" bitrate couldn't be read.");
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
	off_t new_position;

	assert (sec >= 0);

	if (data->size == -1)
		return -1;

	if (sec >= data->duration)
		return -1;

	new_position = ((double) sec /
			(double) data->duration) * data->size;

	debug ("Seeking to %d (byte %"PRId64")", sec, new_position);

	if (new_position < 0)
		new_position = 0;
	else if (new_position >= data->size)
		return -1;

	if (io_seek(data->io_stream, new_position, SEEK_SET) == -1) {
		logit ("seek to %"PRId64" failed", new_position);
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

static int mp3_get_avg_bitrate (void *void_data)
{
	struct mp3_data *data = (struct mp3_data *)void_data;

	return data->avg_bitrate / 1000;
}

static int mp3_get_duration (void *void_data)
{
	struct mp3_data *data = (struct mp3_data *)void_data;

	return data->duration;
}

static void mp3_get_name (const char *file, char buf[4])
{
	char *ext;

	strcpy (buf, "MPx");

	ext = ext_pos (file);
	if (ext) {
		if (!strcasecmp (ext, "mp3"))
			strcpy (buf, "MP3");
		else if (!strcasecmp (ext, "mp2"))
			strcpy (buf, "MP2");
		else if (!strcasecmp (ext, "mp1"))
			strcpy (buf, "MP1");
		else if (!strcasecmp (ext, "mpga"))
			strcpy (buf, "MPG");
	}
}

static int mp3_our_format_ext (const char *ext)
{
	return !strcasecmp (ext, "mp3")
		|| !strcasecmp (ext, "mpga")
		|| !strcasecmp (ext, "mp2")
		|| !strcasecmp (ext, "mp1");
}

static void mp3_get_error (void *prv_data, struct decoder_error *error)
{
	struct mp3_data *data = (struct mp3_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct io_stream *mp3_get_stream (void *prv_data)
{
	struct mp3_data *data = (struct mp3_data *)prv_data;

	return data->io_stream;
}

static int mp3_our_mime (const char *mime)
{
	return !strcasecmp (mime, "audio/mpeg")
		|| !strncasecmp (mime, "audio/mpeg;", 11);
}

static int mp3_can_decode (struct io_stream *stream)
{
	unsigned char buf[16 * 1024];

	/* We must use such a sophisticated test, because there are Shoutcast
	 * servers that can start broadcasting in the middle of a frame, so we
	 * can't use any fewer bytes for magic values. */
	if (io_peek(stream, buf, sizeof(buf)) == sizeof(buf)) {
		struct mad_stream stream;
		struct mad_header header;
		int dec_res;

		mad_stream_init (&stream);
		mad_header_init (&header);

		mad_stream_buffer (&stream, buf, sizeof(buf));
		stream.error = 0;

		while ((dec_res = mad_header_decode(&header, &stream)) == -1
				&& MAD_RECOVERABLE(stream.error))
			;

		return dec_res != -1 ? 1 : 0;
	}

	return 0;
}

static void mp3_init ()
{
	iconv_id3_fix = iconv_open ("UTF-8",
			options_get_str("ID3v1TagsEncoding"));
	if (iconv_id3_fix == (iconv_t)(-1))
		log_errno ("iconv_open() failed", errno);
}

static void mp3_destroy ()
{
	if (iconv_close(iconv_id3_fix) == -1)
		log_errno ("iconv_close() failed", errno);
}

static struct decoder mp3_decoder = {
	DECODER_API_VERSION,
	mp3_init,
	mp3_destroy,
	mp3_open,
	mp3_open_stream,
	mp3_can_decode,
	mp3_close,
	mp3_decode,
	mp3_seek,
	mp3_info,
	mp3_get_bitrate,
	mp3_get_duration,
	mp3_get_error,
	mp3_our_format_ext,
	mp3_our_mime,
	mp3_get_name,
	NULL,
	mp3_get_stream,
	mp3_get_avg_bitrate
};

struct decoder *plugin_init ()
{
	return &mp3_decoder;
}
