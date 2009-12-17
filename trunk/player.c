/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define DEBUG

#include "common.h"
#include "log.h"
#include "decoder.h"
#include "audio.h"
#include "out_buf.h"
#include "server.h"
#include "options.h"
#include "player.h"
#include "files.h"
#include "playlist.h"

#define PCM_BUF_SIZE		(32 * 1024)
#define PREBUFFER_THRESHOLD	(16 * 1024)

enum request
{
	REQ_NOTHING,
	REQ_SEEK,
	REQ_STOP,
	REQ_PAUSE,
	REQ_UNPAUSE
};

struct bitrate_list_node
{
	struct bitrate_list_node *next;
	int time;
	int bitrate;
};

/* List of points where bitrate has changed. We use it to show bitrate at the
 * right time when playing, because the output buffer may be big and decoding
 * may be many seconds ahead of what the user can hear. */
struct bitrate_list
{
	struct bitrate_list_node *head;
	struct bitrate_list_node *tail;
	pthread_mutex_t mutex;
};

struct precache
{
	char *file; /* the file to precache */
	char buf[2 * PCM_BUF_SIZE]; /* PCM buffer with precached data */
	int buf_fill;
	int ok; /* 1 if precache succeed */
	struct sound_params sound_params; /* of the sound in the buffer */
	struct decoder *f; /* decoder functions for precached file */
	void *decoder_data;
	int running; /* if the precache thread is running */
	pthread_t tid; /* tid of the precache thread */
	struct bitrate_list bitrate_list;
	int decoded_time; /* how much sound we decoded in seconds */
};

struct precache precache;

/* Request conditional and mutex. */
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t request_cond_mutex = PTHREAD_MUTEX_INITIALIZER;

static enum request request = REQ_NOTHING;
static int req_seek;

/* Source of the played stream tags. */
static enum
{
	TAGS_SOURCE_DECODER,	/* tags from the stream (e.g., id3tags, vorbis comments) */
	TAGS_SOURCE_METADATA	/* tags from icecast metadata */
} tags_source;

/* Tags of the currently played file. */
static struct file_tags *curr_tags = NULL;

/* Mutex for curr_tags and tags_source. */
static pthread_mutex_t curr_tags_mut = PTHREAD_MUTEX_INITIALIZER;

/* Stream associated with the currently playing decoder. */
static struct io_stream *decoder_stream = NULL;
static pthread_mutex_t decoder_stream_mut = PTHREAD_MUTEX_INITIALIZER;

static int prebuffering = 0; /* are we prebuffering now? */

static struct bitrate_list bitrate_list;

static void bitrate_list_init (struct bitrate_list *b)
{
	assert (b != NULL);
	
	b->head = NULL;
	b->tail = NULL;
	pthread_mutex_init (&b->mutex, NULL);
}

static void bitrate_list_empty (struct bitrate_list *b)
{
	assert (b != NULL);

	LOCK (b->mutex);
	if (b->head) {
		while (b->head) {
			struct bitrate_list_node *t = b->head->next;
			
			free (b->head);
			b->head = t;
		}

		b->tail = NULL;
	}

	debug ("Bitrate list elements removed.");
	
	UNLOCK (b->mutex);
}

static void bitrate_list_destroy (struct bitrate_list *b)
{
	assert (b != NULL);

	bitrate_list_empty (b);

	if (pthread_mutex_destroy(&b->mutex))
		logit ("Can't destroy bitrate list mutex: %s", strerror(errno));
}

static void bitrate_list_add (struct bitrate_list *b, const int time,
		const int bitrate)
{
	assert (b != NULL);
	
	LOCK (b->mutex);
	if (!b->tail) {
		b->head = b->tail = (struct bitrate_list_node *)xmalloc (
				sizeof(struct bitrate_list_node));
		b->tail->next = NULL;
		b->tail->time = time;
		b->tail->bitrate = bitrate;

		debug ("Adding bitrate %d at time %d", bitrate, time);
	}
	else if (b->tail->bitrate != bitrate && b->tail->time != time) {
		assert (b->tail->time < time);
		
		b->tail->next = (struct bitrate_list_node *)xmalloc (
				sizeof(struct bitrate_list_node));
		b->tail = b->tail->next;
		b->tail->next = NULL;
		b->tail->time = time;
		b->tail->bitrate = bitrate;

		debug ("Appending bitrate %d at time %d", bitrate, time);
	}
	else if (b->tail->bitrate == bitrate)
		debug ("Not adding bitrate %d at time %d because the bitrate"
				" hasn't changed", bitrate, time);
	else
		debug ("Not adding bitrate %d at time %d because it is for"
				" the same time as the last bitrate", bitrate,
				time);
	UNLOCK (b->mutex);
}

static int bitrate_list_get (struct bitrate_list *b, const int time)
{
	int bitrate = -1;
	
	assert (b != NULL);

	LOCK (b->mutex);
	if (b->head) {
		while (b->head->next && b->head->next->time <= time) {
			struct bitrate_list_node *o = b->head;
		
			b->head = o->next;
			debug ("Removing old bitrate %d for time %d",
					o->bitrate, o->time);
			free (o);
		}

		bitrate = b->head->bitrate /*b->head->time + 1000*/;
		debug ("Getting bitrate for time %d (%d)", time, bitrate);
	}
	else {
		debug ("Getting bitrate for time %d (no bitrate information)",
				time);
		bitrate = -1;
	}
	UNLOCK (b->mutex);

	return bitrate;
}

static void update_time ()
{
	static int last_time = 0;
	int ctime = audio_get_time ();

	if (ctime != last_time) {
		last_time = ctime;
		ctime_change ();
		set_info_bitrate (bitrate_list_get(&bitrate_list,
					audio_get_time()));
	}
}

static void *precache_thread (void *data)
{
	struct precache *precache = (struct precache *)data;
	int decoded;
	struct sound_params new_sound_params;
	struct decoder_error err;

	precache->buf_fill = 0;
	precache->sound_params.channels = 0; /* mark that sound_params were not
						yet filled. */
	precache->decoded_time = 0.0;
	precache->f = get_decoder (precache->file);
	assert (precache->f != NULL);

	precache->decoder_data = precache->f->open(precache->file);
	precache->f->get_error(precache->decoder_data, &err);
	if (err.type != ERROR_OK) {
		logit ("Failed to open the file for precache: %s", err.err);
		decoder_error_clear (&err);
		precache->f->close (precache->decoder_data);
		return NULL;
	}

	audio_plist_set_time (precache->file,
			precache->f->get_duration(precache->decoder_data));

	/* Stop at PCM_BUF_SIZE, because when we decode too much, there is no
	 * place when we can put the data that don't fit into the buffer. */
	while (precache->buf_fill < PCM_BUF_SIZE) {
		decoded = precache->f->decode (precache->decoder_data,
				precache->buf + precache->buf_fill,
				PCM_BUF_SIZE, &new_sound_params);

		if (!decoded) {
			
			/* EOF so fast? we can't pass this information
			 * in precache, so give up */
			logit ("EOF when precaching.");
			precache->f->close (precache->decoder_data);
			return NULL;
		}

		precache->f->get_error (precache->decoder_data, &err);
		
		if (err.type == ERROR_FATAL) {
			precache->f->close (precache->decoder_data);
			return NULL;
		}
		
		if (!precache->sound_params.channels)
			precache->sound_params = new_sound_params;
		else if (!sound_params_eq(precache->sound_params,
					new_sound_params)) {

			/* there is no way to store sound with two different
			 * parameters in the buffer, give up with
			 * precacheing. (this should never happen) */
			logit ("Sound parameters has changed when precaching.");
			precache->f->close (precache->decoder_data);
			return NULL;
		}

		bitrate_list_add (&precache->bitrate_list,
				precache->decoded_time,
				precache->f->get_bitrate(
					precache->decoder_data));

		precache->buf_fill += decoded;
		precache->decoded_time += decoded / (float)(sfmt_Bps(
					new_sound_params.fmt) *
				new_sound_params.rate *
				new_sound_params.channels);

		if (err.type != ERROR_OK)
			break; /* Don't lose the error message */
	}
	
	precache->ok = 1;
	logit ("Successfully precached file (%d bytes)", precache->buf_fill);
	return NULL;
}

static void start_precache (struct precache *precache, const char *file)
{
	assert (!precache->running);
	assert (file != NULL);

	precache->file = xstrdup (file);
	bitrate_list_init (&precache->bitrate_list);
	logit ("Precaching file %s", file);
	precache->ok = 0;
	if (pthread_create(&precache->tid, NULL, precache_thread, precache))
		logit ("Could not run precache thread");
	else
		precache->running = 1;
}

static void precache_wait (struct precache *precache)
{
	if (precache->running) {
		debug ("Waiting for precache thread...");
		if (pthread_join(precache->tid, NULL))
			fatal ("pthread_join() for precache thread failed");
		precache->running = 0;
		debug ("done");
	}
	else
		debug ("Precache thread is not running");
}

static void precache_reset (struct precache *precache)
{
	assert (!precache->running);
	precache->ok = 0;
	if (precache->file) {
		free (precache->file);
		precache->file = NULL;
		bitrate_list_destroy (&precache->bitrate_list);
	}
}

void player_init ()
{
	precache.file = NULL;
	precache.running = 0;
	precache.ok =  0;
}

static void show_tags (const struct file_tags *tags)
{
	
	debug ("TAG[title]: %s", tags->title ? tags->title : "N/A");
	debug ("TAG[album]: %s", tags->album ? tags->album : "N/A");
	debug ("TAG[artist]: %s", tags->artist ? tags->artist : "N/A");
	debug ("TAG[track]: %d", tags->track);
}

/* Update tags if tags from the decoder or the stream are available. */
static void update_tags (const struct decoder *f, void *decoder_data,
		struct io_stream *s)
{
	char *stream_title = NULL;
	int tags_changed = 0;
	struct file_tags *new_tags;

	new_tags = tags_new ();

	LOCK (curr_tags_mut);
	if (f->current_tags && f->current_tags(decoder_data, new_tags)
			&& new_tags->title) {
		tags_changed = 1;
		tags_copy (curr_tags, new_tags);
		logit ("Tags change from the decoder");
		tags_source = TAGS_SOURCE_DECODER;
		show_tags (curr_tags);
	}
	else if (s && (stream_title = io_get_metadata_title(s))) {
		if (curr_tags && curr_tags->title
				&& tags_source == TAGS_SOURCE_DECODER) {
			logit ("New io stream tags, ignored because there are "
					" decoder tags present.");
			free (stream_title);
		}
		else {
			tags_clear (curr_tags);
			curr_tags->title = stream_title;
			show_tags (curr_tags);
			tags_changed = 1;
			logit ("New IO stream tags");
			tags_source = TAGS_SOURCE_METADATA;
		}
	}

	if (tags_changed)
		tags_change ();

	tags_free (new_tags);
	
	UNLOCK (curr_tags_mut);
}

/* Called when some free space in the output buffer appears. */
static void buf_free_callback ()
{
	LOCK (request_cond_mutex);
	pthread_cond_broadcast (&request_cond);
	UNLOCK (request_cond_mutex);

	update_time ();
}

/* Decoder loop for already opened and probably running for some time decoder.
 * next_file will be precached at eof. */
static void decode_loop (const struct decoder *f, void *decoder_data,
		const char *next_file, struct out_buf *out_buf,
		struct sound_params sound_params,
		const float already_decoded_sec)
{
	int eof = 0;
	char buf[PCM_BUF_SIZE];
	int decoded = 0;
	struct sound_params new_sound_params;
	int sound_params_change = 0;
	float decode_time = already_decoded_sec; /* the position of the decoder
						    (in seconds) */

	out_buf_set_free_callback (out_buf, buf_free_callback);
	
	LOCK (curr_tags_mut);
	curr_tags = tags_new ();
	UNLOCK (curr_tags_mut);

	if (f->get_stream) {
		LOCK (decoder_stream_mut);
		decoder_stream = f->get_stream (decoder_data);
		UNLOCK (decoder_stream_mut);
	}
	else
		logit ("No get_stream() function");

	status_msg ("Playing...");

	while (1) {
		debug ("loop...");
		
		LOCK (request_cond_mutex);
		if (!eof && !decoded) {
			struct decoder_error err;
			
			UNLOCK (request_cond_mutex);

			if (decoder_stream && out_buf_get_fill(out_buf)
					< PREBUFFER_THRESHOLD) {
				prebuffering = 1;
				io_prebuffer (decoder_stream,
						options_get_int("Prebuffering")
						* 1024);
				prebuffering = 0;
				status_msg ("Playing...");
			}
			
			decoded = f->decode (decoder_data, buf, sizeof(buf),
					&new_sound_params);

			if (decoded)
				decode_time += decoded / (float)(sfmt_Bps(
							new_sound_params.fmt) *
						new_sound_params.rate *
						new_sound_params.channels);
			
			f->get_error (decoder_data, &err);
			if (err.type != ERROR_OK) {
				if (err.type != ERROR_STREAM
						|| options_get_int(
							"ShowStreamErrors"))
					error ("%s", err.err);
				decoder_error_clear (&err);
			}
			
			if (!decoded) {
				eof = 1;
				logit ("EOF from decoder");
			}
			else {
				debug ("decoded %d bytes", decoded);
				if (!sound_params_eq(new_sound_params,
							sound_params))
					sound_params_change = 1;

				bitrate_list_add (&bitrate_list, decode_time,
						f->get_bitrate(decoder_data));
				update_tags (f, decoder_data, decoder_stream);
			}
		}

		/* Wait, if there is no space in the buffer to put the decoded
		 * data or EOF occured and there is something in the buffer. */
		else if (decoded > out_buf_get_free(out_buf)
					|| (eof && out_buf_get_fill(out_buf))) {
			debug ("waiting...");
			if (eof && !precache.file && next_file
					&& file_type(next_file) == F_SOUND
					&& options_get_int("Precache"))
				start_precache (&precache, next_file);
			pthread_cond_wait (&request_cond, &request_cond_mutex);
			UNLOCK (request_cond_mutex);
		}
		else
			UNLOCK (request_cond_mutex);

		/* When clearing request, we must make sure, that another
		 * request will not arrive at the moment, so we check if
		 * the request has changed. */
		if (request == REQ_STOP) {
			logit ("stop");
			out_buf_stop (out_buf);
			
			LOCK (request_cond_mutex);
			if (request == REQ_STOP)
				request = REQ_NOTHING;
			UNLOCK (request_cond_mutex);
			
			break;
		}
		else if (request == REQ_SEEK) {
			int decoder_seek;
			
			logit ("seeking");
			if ((decoder_seek = f->seek(decoder_data, req_seek))
					== -1)
				logit ("error when seeking");
			else {
				out_buf_stop (out_buf);
				out_buf_reset (out_buf);
				out_buf_time_set (out_buf, decoder_seek);
				bitrate_list_empty (&bitrate_list);
				decode_time = decoder_seek;
				eof = 0;
				decoded = 0;
			}

			LOCK (request_cond_mutex);
			if (request == REQ_SEEK)
				request = REQ_NOTHING;
			UNLOCK (request_cond_mutex);

		}
		else if (!eof && decoded <= out_buf_get_free(out_buf)
				&& !sound_params_change) {
			debug ("putting into the buffer %d bytes", decoded);
			audio_send_buf (buf, decoded);
			decoded = 0;
		}
		else if (!eof && sound_params_change
				&& out_buf_get_fill(out_buf) == 0) {
			logit ("sound parameters has changed.");
			sound_params = new_sound_params;
			sound_params_change = 0;
			set_info_channels (sound_params.channels);
			set_info_rate (sound_params.rate / 1000);
			out_buf_wait (out_buf);
			if (!audio_open(&sound_params))
				break;
		}
		else if (eof && out_buf_get_fill(out_buf) == 0) {
			logit ("played everything");
			break;
		}
	}

	status_msg ("");

	LOCK (decoder_stream_mut);
	decoder_stream = NULL;
	f->close (decoder_data);
	UNLOCK (decoder_stream_mut);

	bitrate_list_destroy (&bitrate_list);

	LOCK (curr_tags_mut);
	if (curr_tags) {
		tags_free (curr_tags);
		curr_tags = NULL;
	}
	UNLOCK (curr_tags_mut);

	out_buf_wait (out_buf);
}

/* Play a file (disk file) using the given decoder. next_file is precached. */
static void play_file (const char *file, const struct decoder *f,
		const char *next_file, struct out_buf *out_buf)
{
	void *decoder_data;
	struct sound_params sound_params = { 0, 0, 0 };
	float already_decoded_time;
	
	out_buf_reset (out_buf);
	
	precache_wait (&precache);

	if (precache.ok && strcmp(precache.file, file)) {
		logit ("The precached file is not the file we want.");
		precache.f->close (precache.decoder_data);
		precache_reset (&precache);
	}

	if (precache.ok && !strcmp(precache.file, file)) {
		struct decoder_error err;

		logit ("Using precached file");

		assert (f == precache.f);
		
		sound_params = precache.sound_params;
		decoder_data = precache.decoder_data;
		set_info_channels (sound_params.channels);
		set_info_rate (sound_params.rate / 1000);

		if (!audio_open(&sound_params))
			return;
		audio_send_buf (precache.buf, precache.buf_fill);

		precache.f->get_error (precache.decoder_data, &err);
		if (err.type != ERROR_OK) {
			if (err.type != ERROR_STREAM
					|| options_get_int(
						"ShowStreamErrors"))
				error ("%s", err.err);
			decoder_error_clear (&err);
		}

		already_decoded_time = precache.decoded_time;

		if(f->get_avg_bitrate)
			set_info_avg_bitrate (f->get_avg_bitrate(decoder_data));
		else
			set_info_avg_bitrate (0);

		bitrate_list_init (&bitrate_list);
		bitrate_list.head = precache.bitrate_list.head;
		bitrate_list.tail = precache.bitrate_list.tail;

		/* don't free list elements when reseting precache */
		precache.bitrate_list.head = NULL;
		precache.bitrate_list.tail = NULL;
	}
	else {
		struct decoder_error err;

		status_msg ("Opening...");
		decoder_data = f->open(file);
		f->get_error (decoder_data, &err);
		if (err.type != ERROR_OK) {
			f->close (decoder_data);
			error ("%s", err.err);
			decoder_error_clear (&err);
			logit ("Can't open file, exiting");
			return;
		}

		already_decoded_time = 0.0;
		if(f->get_avg_bitrate)
			set_info_avg_bitrate (f->get_avg_bitrate(decoder_data));
		bitrate_list_init (&bitrate_list);
	}

	audio_plist_set_time (file, f->get_duration(decoder_data));
	audio_state_started_playing ();
	precache_reset (&precache);

	decode_loop (f, decoder_data, next_file, out_buf, sound_params,
			already_decoded_time);
}

/* Play the stream (global decoder_stream) using the given decoder. */
static void play_stream (const struct decoder *f, struct out_buf *out_buf)
{
	void *decoder_data;
	struct sound_params sound_params = { 0, 0, 0 };
	struct decoder_error err;

	out_buf_reset (out_buf);
	
	assert (f->open_stream != NULL);

	decoder_data = f->open_stream (decoder_stream);
	f->get_error (decoder_data, &err);
	if (err.type != ERROR_OK) {
		LOCK (decoder_stream_mut);
		decoder_stream = NULL;
		UNLOCK (decoder_stream_mut);

		f->close (decoder_data);
		error ("%s", err.err);
		decoder_error_clear (&err);
		logit ("Can't open file");
	}
	else {
		audio_state_started_playing ();
		bitrate_list_init (&bitrate_list);
		decode_loop (f, decoder_data, NULL, out_buf, sound_params,
				0.0);
	}
}

/* Callback for io buffer fill - show the prebuffering state. */
static void fill_callback (struct io_stream *s ATTR_UNUSED, size_t fill,
		size_t buf_size ATTR_UNUSED, void *data_ptr ATTR_UNUSED)
{
	if (prebuffering) {
		char msg[32];

		sprintf (msg, "Prebuffering %d/%d KB", (int)(fill / 1024),
				options_get_int("Prebuffering"));
		status_msg (msg);
	}
}

/* Open a file, decode it and put output into the buffer. At the end, start
 * precaching next_file. */
void player (const char *file, const char *next_file, struct out_buf *out_buf)
{
	struct decoder *f;

	if (file_type(file) == F_URL) {
		status_msg ("Connecting...");
		
		LOCK (decoder_stream_mut);
		decoder_stream = io_open (file, 1);
		if (!io_ok(decoder_stream)) {
			error ("Could not open URL: %s",
					io_strerror(decoder_stream));
			io_close (decoder_stream);
			decoder_stream = NULL;
			UNLOCK (decoder_stream_mut);
			return;
		}
		UNLOCK (decoder_stream_mut);

		f = get_decoder_by_content (decoder_stream);
		if (!f) {
			LOCK (decoder_stream_mut);
			io_close (decoder_stream);
			status_msg ("");
			decoder_stream = NULL;
			UNLOCK (decoder_stream_mut);
			return;
		}
		
		status_msg ("Prebuffering...");
		prebuffering = 1;
		io_set_buf_fill_callback (decoder_stream, fill_callback, NULL);
		io_prebuffer (decoder_stream,
				options_get_int("Prebuffering") * 1024);
		prebuffering = 0;

		status_msg ("Playing...");
		ev_audio_start ();
		play_stream (f, out_buf);
		ev_audio_stop ();
	}
	else {
		f = get_decoder (file);
		LOCK (decoder_stream_mut);
		decoder_stream = NULL;
		UNLOCK (decoder_stream_mut);

		if (!f) {
			error ("Can't get decoder for %s", file);
			return;
		}

		ev_audio_start ();
		play_file (file, f, next_file, out_buf);
		ev_audio_stop ();
	}

	logit ("exiting");
}

void player_cleanup ()
{
	if (pthread_mutex_destroy(&request_cond_mutex))
		logit ("Can't destroy request mutex: %s", strerror(errno));
	if (pthread_mutex_destroy(&curr_tags_mut))
		logit ("Can't destroy tags mutex: %s", strerror(errno));
	if (pthread_mutex_destroy(&decoder_stream_mut))
		logit ("Can't destroy decoder_stream mutex: %s", strerror(errno));
	if (pthread_cond_destroy(&request_cond))
		logit ("Can't destroy request condition: %s", strerror(errno));

	precache_wait (&precache);
	precache_reset (&precache);
}

void player_reset ()
{
	request = REQ_NOTHING;
}

void player_stop ()
{
	logit ("requesting stop");
	request = REQ_STOP;
	
	LOCK (decoder_stream_mut);
	if (decoder_stream) {
		logit ("decoder_stream present, aborting...");
		io_abort (decoder_stream);
	}
	UNLOCK (decoder_stream_mut);

	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

void player_seek (const int sec)
{
	request = REQ_SEEK;
	req_seek = sec + audio_get_time();
	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

void player_jump_to (const int sec)
{
	request = REQ_SEEK;
	req_seek = sec;
	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

/* Stop playing, clear the output buffer, but allow to unpause by starting
 * playing the same stream. This is usefull for internet streams that can't
 * be really paused. */
void player_pause ()
{
	request = REQ_PAUSE;
	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

void player_unpause ()
{
	request = REQ_UNPAUSE;
	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

/* Return tags for the currently played file or NULL if there are no tags.
 * Tags are duplicated. */
struct file_tags *player_get_curr_tags ()
{
	struct file_tags *tags;

	LOCK (curr_tags_mut);
	if (curr_tags)
		tags = tags_dup (curr_tags);
	else
		tags = NULL;
	UNLOCK (curr_tags_mut);

	return tags;
}
