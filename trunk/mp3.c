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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <mad.h>
#include <id3tag.h>
#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif
#include <pthread.h>

#include "server.h"
#include "main.h"
#include "log.h"
#include "xing.h"
#include "playlist.h"
#include "audio.h"
#include "buf.h"
#include "options.h"

/* Used only if mmap() is not available */
#define INPUT_BUFFER	(64 * 1024)

/* Information about the file */
struct {
	int infile; /* fd on the mp3 file */
	unsigned long bitrate;
	unsigned int freq;
	short channels;
	signed long duration; /* Total time of the file in seconds*/
	off_t size; /* Size of the file */
	struct xing xing;
	short has_xing;
#ifdef HAVE_MMAP
	char *mapped;
	int mapped_size;
#endif
} info;

static unsigned char in_buff[INPUT_BUFFER];

/* Fill in the mad buffer, return number of bytes read, 0 on eof, < 0 on error */
static size_t fill_buff (struct mad_stream *stream)
{
	size_t remaining;
	ssize_t read_size;
	unsigned char *read_start;
	
	if (stream->next_frame != NULL) {
		remaining = stream->bufend - stream->next_frame;
		memmove (in_buff, stream->next_frame, remaining);
		read_start = in_buff + remaining;
		read_size = INPUT_BUFFER - remaining;
	}
	else {
		read_start = in_buff;
		read_size = INPUT_BUFFER;
		remaining = 0;
	}

	read_size = read (info.infile, read_start, read_size);
	if (read_size < 0) {
		error ("read() failed: %s\n", strerror (errno));
		return -1;
	}
	else if (read_size == 0)
		return 0;

	mad_stream_buffer(stream, in_buff, read_size + remaining);
	stream->error = 0;

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

/* Fill info structure with data from the id3 tag */
void mp3_info (const char *file_name, struct file_tags *info)
{
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
			if (!*end)
				info->track = -1;
			free (track);
		}
	}
	id3_file_close (id3file);
}


static int load_mp3 (const char *file_name)
{
	struct stat stat;

	/* Reset information about the file */
	info.bitrate = 0;
	info.freq = 0;
	info.channels = 0;
	info.duration = 0;
	info.has_xing = 0;

	/* Open the file */
	if ((info.infile = open(file_name, O_RDONLY)) == -1) {
		error ("open() failed: %s\n", strerror (errno));
		return 0;
	}

	if (fstat(info.infile, &stat) == -1) {
		error ("Can't stat() file: %s\n", strerror(errno));
		close (info.infile);
		return 0;
	}

	info.size = stat.st_size;

#ifdef HAVE_MMAP
	info.mapped_size = info.size;
	info.mapped = mmap (0, info.mapped_size, PROT_READ, MAP_SHARED,
			info.infile, 0);
	if (info.mapped == MAP_FAILED) {
		logit ("mmap() failed: %s, using standard read()",
				strerror(errno));
		info.mapped = NULL;
	}
	else
		logit ("mmapped() %ld bytes of file", info.size);
#endif

	/*seek = 0;*/

	return 1;
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

static int write_output (struct mad_pcm *pcm, struct mad_header *header)
{
	unsigned int nsamples;
	mad_fixed_t const *left_ch, *right_ch;
	char *output;
	int olen = 0;
	int pos = 0;
	int sent;

	nsamples = pcm->length;
	left_ch = pcm->samples[0];
	right_ch = pcm->samples[1];
	olen = nsamples * MAD_NCHANNELS (header) * 2;
	output = (char *) xmalloc (olen * sizeof (char));

	while (nsamples--) {
		signed int sample;

		/* output sample(s) in 16-bit signed little-endian PCM */
		sample = scale (*left_ch++);
		output[pos++] = (sample >> 0) & 0xff;
		output[pos++] = (sample >> 8) & 0xff;

		if (MAD_NCHANNELS (header) == 2) {
			sample = scale (*right_ch++);
			output[pos++] = (sample >> 0) & 0xff;
			output[pos++] = (sample >> 8) & 0xff;
		}
	}
	sent = audio_send_buf (output, olen);
	free (output);
	return sent;
}

void mp3_play (const char *file, struct buf *out_buf)
{
	int first_frame = 1;
	int seek_skip = 0, seek = -1;
	int written;
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	if (!load_mp3(file))
		return;

	mad_stream_init (&stream);
	mad_frame_init (&frame);
	mad_synth_init (&synth);
	
#ifdef HAVE_MMAP
	if (info.mapped) {
		mad_stream_buffer (&stream, info.mapped, info.mapped_size);
		stream.error = 0;
	}
#endif
	
	while (1) {
		enum play_request request;

		/* Fill the input buffer */
		if (stream.buffer == NULL ||
			stream.error == MAD_ERROR_BUFLEN) {
#ifdef HAVE_MMAP
			if (info.mapped)
				break; /* FIXME: check if the size of file
					  has't changed */
			else
#endif
			if (fill_buff(&stream) <= 0)
				break;
		}
		
		/* Skip 2 frames after seeking */
		if (seek_skip) {
			int skip = 2;

			do {
				if (mad_frame_decode(&frame, &stream) == 0) {
					if (--skip == 0)
						mad_synth_frame(&synth, &frame);
				}
				else if (!MAD_RECOVERABLE(stream.error)) {
					logit ("unrecoverable error after seeking: %s",
							mad_stream_errorstr(&stream));
					break;
				}
			} while (skip);

			seek_skip = 0;
		}
		
		if (seek != -1) {
			int new_position;

			if (seek >= info.duration)
				break;
			else if (seek < 0)
				seek = 0;

			new_position = ((double) seek /
					(double) info.duration) * info.size;

			if (new_position < 0)
				new_position = 0;
			else if (new_position >= info.size)
				break;
			
#ifdef HAVE_MMAP
			if (info.mapped) {
				mad_stream_buffer (&stream,
						info.mapped + new_position,
						info.mapped_size - new_position);
				stream.error = 0;
			}
			else {
#endif
				if (lseek (info.infile, new_position,
							SEEK_SET) == -1) {
					error ("Failed to seek to: %d",
							new_position);
					continue;
				}
				stream.error = MAD_ERROR_BUFLEN;

				mad_frame_mute (&frame);
				mad_synth_mute (&synth);

				stream.sync = 0;
				stream.next_frame = NULL;

#ifdef HAVE_MMAP
			}
#endif

			buf_time_set (out_buf, seek);

			seek_skip = 1;
			seek = -1;

			continue;
		}

		if (mad_frame_decode (&frame, &stream)) {
			if (MAD_RECOVERABLE(stream.error)) {

				/* Ignore LOSTSYNC */
				if (stream.error == MAD_ERROR_LOSTSYNC)
					continue;

				if (options_get_int("ShowStreamErrors"))
					error ("Broken frame: %s",
							mad_stream_errorstr(&stream));
				continue;
			}
			else if (stream.error == MAD_ERROR_BUFLEN)
				continue;
			else {
				if (options_get_int("ShowStreamErrors"))
					error ("Broken frame: %s",
							mad_stream_errorstr (&stream));
				break;
			}
		}

		/* Bitrate and number or channels can change */
		if (info.channels != MAD_NCHANNELS(&frame.header) ||
				info.freq != frame.header.samplerate) {
			if (!first_frame)
				audio_close ();
			
			if ((info.freq = frame.header.samplerate) == 0) {
				if (options_get_int("ShowStreamErrors"))
					error ("Broken file: information "
							"about the frequency "
							"couldn't be read.");
				break;
			}
			info.channels = MAD_NCHANNELS(&frame.header);
			
			if (!audio_open(16, info.channels, info.freq))
				break;

			set_info_channels (info.channels);
			set_info_rate (info.freq / 1000);
		}

		/* Change of the bitrate? */
		if (frame.header.bitrate != info.bitrate) {
			if ((info.bitrate = frame.header.bitrate) == 0) {
				error ("Broken file: information "
						"about the bitrate couldn't "
						"be read.\n");
				break;
			}

			set_info_bitrate (info.bitrate / 1000);
		}

		/* Read some information about the strean from the first frame */
		if (first_frame) {
			mad_timer_t duration = frame.header.duration;

			if (xing_parse(&info.xing, stream.anc_ptr,
						stream.anc_bitlen) != -1) {
				xing_init (&info.xing);
				mad_timer_multiply (&duration,
						info.xing.frames);
				info.has_xing = 1;
			}
			else {
				mad_timer_multiply (&duration, info.size /
					(stream.next_frame - stream.this_frame));
			}

			info.duration = mad_timer_count (duration,
					MAD_UNITS_SECONDS);
			set_info_time (info.duration);
			
			first_frame = 0;
		}
		
		mad_synth_frame (&synth, &frame);
		mad_stream_sync (&stream);

		written = write_output (&synth.pcm, &frame.header);
				
		if ((request = get_request()) != PR_NOTHING) {
			buf_stop (out_buf);
			buf_reset (out_buf);

			if (request == PR_STOP) {
				logit ("MP3: stopping");
				break;
			}
			else if (request == PR_SEEK_FORWARD) {
				logit ("MP3: seek forward");
				seek = audio_get_time() + 1;
			}
			else if (request == PR_SEEK_BACKWARD) {
				logit ("MP3: seek backward");
				seek = audio_get_time() - 1;
			}
		}
		else if (!written) {
			logit ("MP3: write refused, exiting");
			break;
		}
	}
		
	mad_stream_finish (&stream);
	mad_frame_finish (&frame);
	mad_synth_finish (&synth);

#ifdef HAVE_MMAP
	if (info.mapped && munmap(info.mapped, info.size) == -1)
		logit ("munmap() failed: %s", strerror(errno));
#endif

	close (info.infile);
}
