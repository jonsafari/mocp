/*
 * MOC - music on console
 * Copyright (C) 2002, 2003 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h> /* Only for debug! */
#include <errno.h>
#include <vorbis/vorbisfile.h>
#include <vorbis/codec.h>

#include "server.h"
#include "main.h"
#include "log.h"
#include "playlist.h"
#include "audio.h"
#include "buf.h"
#include "options.h"


#define PCM_BUFF_SIZE	4096

/* Fill info structure with data from ogg comments */
void ogg_info (const char *file_name, struct file_tags *info)
{
	int i;
	vorbis_comment *comments;
	OggVorbis_File vf;
	FILE *file;

	if (!(file = fopen (file_name, "r"))) {
		error ("Can't load OGG: %s", strerror(errno));
		return;
	}

	if (ov_test(file, &vf, NULL, 0) < 0) {
		error ("ov_test() failed!");
		return;
	}

	comments = ov_comment (&vf, -1);
	for (i = 0; i < comments->comments; i++) {
		if (!strncasecmp(comments->user_comments[i], "title=",
				 strlen ("title=")))
			info->title = xstrdup(comments->user_comments[i]
					+ strlen ("title="));
		else if (!strncasecmp(comments->user_comments[i], "artist=",
				      strlen ("artist=")))
			info->artist = xstrdup (comments->user_comments[i]
					+ strlen ("artiat="));
		else if (!strncasecmp(comments->user_comments[i], "album=",
				      strlen ("album=")))
			info->album = xstrdup (comments->user_comments[i]
					+ strlen ("album="));
		else if (!strncasecmp(comments->user_comments[i], "tracknumber=",
				      strlen ("tracknumber=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("tracknumber="));
		else if (!strncasecmp(comments->user_comments[i], "track=",
				      strlen ("track=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("track="));

	}

	ov_clear (&vf);
}

/* Decode and play stream */
void ogg_play (const char *file_name, struct buf *out_buf)
{
	FILE *file;
	OggVorbis_File vf;
	char pcm_buff[PCM_BUFF_SIZE];
	int current_section;
	int last_section = -1;
	long ret, bitrate;
	vorbis_info *info;
	enum play_request request;

	if (!(file = fopen (file_name, "r"))) {
		error ("Can't load OGG: %s", strerror(errno));
		return;
	}

	if (ov_open(file, &vf, NULL, 0) < 0) {
		error ("ov_open() failed!");
		return;
	}

	info = ov_info (&vf, -1);
	if (!audio_open(16, info->channels, info->rate))
		return;
	
	set_info_rate (info->rate / 1000);
	set_info_time (ov_time_total (&vf, -1));
	set_info_channels (info->channels);
	set_info_bitrate (ov_bitrate(&vf, -1) / 1000);

	while (1) {
		int written;
		
		ret = ov_read(&vf, pcm_buff, PCM_BUFF_SIZE, 0,
				2, 1, &current_section);
		if (ret == 0)
			break;
		if (ret < 0) {
			if (options_get_int("ShowStreamErrors")) 
				error ("Error in the stream!");
			continue;
		}
		
		if (last_section != current_section
				&& last_section >= 0) {
			audio_close ();
			info = ov_info (&vf, -1);
			if (!audio_open(16, info->channels, info->rate))
				break;
			set_info_rate (info->rate / 1000);
			set_info_channels (info->channels);
		}

		written = audio_send_buf (pcm_buff, ret);
		
		/* Update the bitrate information */
		bitrate = ov_bitrate_instant (&vf);
		if (bitrate > 0)
			set_info_bitrate (bitrate / 1000);
		last_section = current_section;

		if ((request = get_request()) != PR_NOTHING) {
			buf_reset (out_buf);

			if (request == PR_STOP) {
				logit ("OGG: stopping");
				break;
			}
			else if (request == PR_SEEK_FORWARD) {
				float time = ov_time_tell(&vf) + 1;
				
				logit ("OGG: seek forward");
				ov_time_seek (&vf, time);
				buf_time_set (out_buf, time);
			}
			else if (request == PR_SEEK_BACKWARD) {
				float time = ov_time_tell(&vf) - 1;
				
				logit ("OGG: seek backward");
				ov_time_seek (&vf, time);
				buf_time_set (out_buf, time);
			}
		}
		else if (!written) {
			logit ("OGG: write refused, exiting");
			break;
		}
	}
	
	ov_clear (&vf);
}
