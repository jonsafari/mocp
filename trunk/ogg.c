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
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <vorbis/vorbisfile.h>
#include <vorbis/codec.h>

#include "server.h"
#include "main.h"
#include "log.h"
#include "audio.h"
#include "buf.h"
#include "options.h"
#include "file_types.h"

struct ogg_data
{
	FILE *file;
	OggVorbis_File vf;
	int last_section;
	int bitrate;
};

/* Fill info structure with data from ogg comments */
static void ogg_info (const char *file_name, struct file_tags *info)
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

static void *ogg_open (const char *file)
{
	struct ogg_data *data;
	vorbis_info *info;

	data = (struct ogg_data *)xmalloc (sizeof(struct ogg_data));

	if (!(data->file = fopen (file, "r"))) {
		error ("Can't load OGG: %s", strerror(errno));
		free (data);
		return NULL;
	}

	if (ov_open(data->file, &data->vf, NULL, 0) < 0) {
		error ("ov_open() failed!");
		fclose (data->file);
		free (data);
		return NULL;
	}

	data->last_section = -1;

	info = ov_info (&data->vf, -1);
	
	set_info_time (ov_time_total(&data->vf, -1));
	set_info_bitrate (ov_bitrate(&data->vf, -1) / 1000);

	return data;
}

static void ogg_close (void *void_data)
{
	struct ogg_data *data = (struct ogg_data *)void_data;

	ov_clear (&data->vf);
	free (data);
}

static int ogg_seek (void *void_data, int sec)
{
	struct ogg_data *data = (struct ogg_data *)void_data;

	return ov_time_seek (&data->vf, sec) ? -1 : sec;
}

static int ogg_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct ogg_data *data = (struct ogg_data *)void_data;
	int ret;
	int current_section;
	int bitrate;

	while (1) {
		ret = ov_read(&data->vf, buf, buf_len, 0, 2, 1,
				&current_section);
		if (ret == 0)
			return 0;
		if (ret < 0) {
			if (options_get_int("ShowStreamErrors")) 
				error ("Error in the stream!");
			continue;
		}
		
		if (current_section != data->last_section) {
			vorbis_info *info;

			logit ("section change or first section");
			
			data->last_section = current_section;
			info = ov_info (&data->vf, -1);
			assert (info != NULL);
			sound_params->channels = info->channels;
			sound_params->rate = info->rate;
			sound_params->format = 2;
		}

		/* Update the bitrate information */
		bitrate = ov_bitrate_instant (&data->vf);
		if (bitrate > 0)
			set_info_bitrate (bitrate / 1000);

		break;
	}

	return ret;
}

static struct decoder_funcs decoder_funcs = {
	ogg_open,
	ogg_close,
	ogg_decode,
	ogg_seek,
	ogg_info
};

struct decoder_funcs *ogg_get_funcs ()
{
	return &decoder_funcs;
}
