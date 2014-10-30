/* Jack plugin for moc by Alex Norman <alex@neisis.net> 2005
 * moc by Copyright (C) 2004 Damian Pietras <daper@daper.net>
 * use at your own risk
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <unistd.h>
#include <stdio.h>
#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define DEBUG

#include "common.h"
#include "audio.h"
#include "log.h"
#include "options.h"

#define RINGBUF_SZ 32768

/* the client */
static jack_client_t *client;
/* an array of output ports */
static jack_port_t **output_port;
/* the ring buffer, used to store the sound data before jack takes it */
static jack_ringbuffer_t *ringbuffer[2];
/* volume */
static jack_default_audio_sample_t volume = 1.0;
/* volume as an integer - needed to avoid cast errors on set/read */
static int volume_integer = 100;
/* indicates if we should be playing or not */
static int play;
/* current sample rate */
static int rate;
/* flag set if xrun occurred that was our fault (the ringbuffer doesn't
 * contain enough data in the process callback) */
static volatile int our_xrun = 0;
/* set to 1 if jack client thread exits */
static volatile int jack_shutdown = 0;

/* this is the function that jack calls to get audio samples from us */
static int process_cb(jack_nframes_t nframes, void *unused ATTR_UNUSED)
{
	jack_default_audio_sample_t *out[2];

	if (nframes <= 0)
		return 0;

	/* get the jack output ports */
	out[0] = (jack_default_audio_sample_t *) jack_port_get_buffer (
			output_port[0], nframes);
	out[1] = (jack_default_audio_sample_t *) jack_port_get_buffer (
			output_port[1], nframes);

	if (play) {
		size_t i;

		/* ringbuffer[1] is filled later, so we only need to check
		 * it's space. */
		size_t avail_data = jack_ringbuffer_read_space(ringbuffer[1]);
		size_t avail_frames = avail_data
			/ sizeof(jack_default_audio_sample_t);

		if (avail_frames > nframes) {
			avail_frames = nframes;
			avail_data = nframes
				* sizeof(jack_default_audio_sample_t);
		}

		jack_ringbuffer_read (ringbuffer[0], (char *)out[0],
				avail_data);
		jack_ringbuffer_read (ringbuffer[1], (char *)out[1],
				avail_data);


		/* we must provide nframes data, so fill with silence
		 * the remaining space. */
		if (avail_frames < nframes) {
			our_xrun = 1;

			for (i = avail_frames; i < nframes; i++)
				out[0][i] = out[1][i] = 0.0;
		}
	}
	else {
		size_t i;
		size_t size;

		/* consume the input */
		size = jack_ringbuffer_read_space(ringbuffer[1]);
		jack_ringbuffer_read_advance (ringbuffer[0], size);
		jack_ringbuffer_read_advance (ringbuffer[1], size);

		for (i = 0; i < nframes; i++) {
			out[0][i] = 0.0;
			out[1][i] = 0.0;
		}
	}

	return 0;
}

/* this is called if jack changes its sample rate */
static int update_sample_rate_cb(jack_nframes_t new_rate,
		void *unused ATTR_UNUSED)
{
	rate = new_rate;
	return 0;
}

/* callback for jack's error messages */
static void error_cb (const char *msg)
{
	error ("JACK: %s", msg);
}

static void shutdown_cb (void *unused ATTR_UNUSED)
{
	jack_shutdown = 1;
}

static int moc_jack_init (struct output_driver_caps *caps)
{
	const char *client_name;

	client_name = options_get_str ("JackClientName");

	jack_set_error_function (error_cb);

#ifdef HAVE_JACK_CLIENT_OPEN

	jack_status_t status;
	jack_options_t options;

	/* open a client connection to the JACK server */
	options = JackNullOption;
	if (!options_get_bool ("JackStartServer"))
		options |= JackNoStartServer;
	client = jack_client_open (client_name, options, &status, NULL);
	if (client == NULL) {
		error ("jack_client_open() failed, status = 0x%2.0x", status);
		if (status & JackServerFailed)
			error ("Unable to connect to JACK server");
		return 0;
	}

	if (status & JackServerStarted)
		printf ("JACK server started\n");

#else

	/* try to become a client of the JACK server */
	client = jack_client_new (client_name);
	if (client == NULL) {
		error ("Cannot create client; JACK server not running?");
		return 0;
	}

#endif

	jack_shutdown = 0;
	jack_on_shutdown (client, shutdown_cb, NULL);

	/* allocate memory for an array of 2 output ports */
	output_port = xmalloc(2 * sizeof(jack_port_t *));
	output_port[0] = jack_port_register (client, "output0", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	output_port[1] = jack_port_register (client, "output1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	/* create the ring buffers */
	ringbuffer[0] = jack_ringbuffer_create(RINGBUF_SZ);
	ringbuffer[1] = jack_ringbuffer_create(RINGBUF_SZ);

	/* set the call back functions, activate the client */
	jack_set_process_callback (client, process_cb, NULL);
	jack_set_sample_rate_callback(client, update_sample_rate_cb, NULL);
	if (jack_activate (client)) {
		error ("cannot activate client");
		return 0;
	}

	/* connect ports
	 * a value of NULL in JackOut* gives no connection
	 * */
	if(strcmp(options_get_str("JackOutLeft"),"NULL")){
		if(jack_connect(client,"moc:output0", options_get_str("JackOutLeft")))
			fprintf(stderr,"%s is not a valid Jack Client / Port", options_get_str("JackOutLeft"));
	}
	if(strcmp(options_get_str("JackOutRight"),"NULL")){
		if(jack_connect(client,"moc:output1", options_get_str("JackOutRight")))
			fprintf(stderr,"%s is not a valid Jack Client / Port", options_get_str("JackOutRight"));
	}

	caps->formats = SFMT_FLOAT;
	rate = jack_get_sample_rate (client);
	caps->max_channels = caps->min_channels = 2;

	logit ("jack init");

	return 1;
}

static int moc_jack_open (struct sound_params *sound_params)
{
	if (sound_params->fmt != SFMT_FLOAT) {
		char fmt_name[SFMT_STR_MAX];

		error ("Unsupported sound format: %s.",
				sfmt_str(sound_params->fmt, fmt_name, sizeof(fmt_name)));
		return 0;
	}
	if (sound_params->channels != 2) {
		error ("Unsupported number of channels");
		return 0;
	}

	logit ("jack open");
	play = 1;

	return 1;
}

static void moc_jack_close ()
{
	logit ("jack close");
	play = 0;
}

static int moc_jack_play (const char *buff, const size_t size)
{
	size_t remain = size;
	size_t pos = 0;

	if (jack_shutdown) {
		logit ("Refusing to play, because there is no client thread.");
		return -1;
	}

	debug ("Playing %zu bytes", size);

	if (our_xrun) {
		logit ("xrun");
		our_xrun = 0;
	}

	while (remain && !jack_shutdown) {
		size_t space;

		/* check if some space is available only in the second
		 * ringbuffer, because it is read later than the first. */
		if ((space = jack_ringbuffer_write_space(ringbuffer[1]))
				> sizeof(jack_default_audio_sample_t)) {
			size_t to_write;

			space *= 2; /* we have 2 channels */
			debug ("Space in the ringbuffer: %zu bytes", space);

			to_write = MIN (space, remain);

			to_write /= sizeof(jack_default_audio_sample_t) * 2;
			remain -= to_write * sizeof(float) * 2;
			while (to_write--) {
				jack_default_audio_sample_t sample;

				sample = *(jack_default_audio_sample_t *)
					(buff + pos) * volume;
				pos += sizeof (jack_default_audio_sample_t);
				jack_ringbuffer_write (ringbuffer[0],
						(char *)&sample,
						sizeof(sample));

				sample = *(jack_default_audio_sample_t *)
					(buff + pos) * volume;
				pos += sizeof (jack_default_audio_sample_t);
				jack_ringbuffer_write (ringbuffer[1],
						(char *)&sample,
						sizeof(sample));
			}
		}
		else {
			debug ("Sleeping for %uus", (unsigned int)(RINGBUF_SZ
					/ (float)(audio_get_bps()) * 100000.0));
			xsleep (RINGBUF_SZ, audio_get_bps ());
		}
	}

	if (jack_shutdown)
		return -1;

	return size;
}

static int moc_jack_read_mixer ()
{
	return volume_integer;
}

static void moc_jack_set_mixer (int vol)
{
	volume_integer = vol;
	volume = (jack_default_audio_sample_t)((exp((double)vol / 100.0) - 1)
			/ (M_E - 1));
}

static int moc_jack_get_buff_fill ()
{
	/* FIXME: should we also use jack_port_get_latency() here? */
	return sizeof(float) * (jack_ringbuffer_read_space(ringbuffer[0])
			+ jack_ringbuffer_read_space(ringbuffer[1]))
		/ sizeof(jack_default_audio_sample_t);
}

static int moc_jack_reset ()
{
	//jack_ringbuffer_reset(ringbuffer); /*this is not threadsafe!*/
	return 1;
}

/* do any cleanup that needs to be done */
static void moc_jack_shutdown(){
	jack_port_unregister(client,output_port[0]);
	jack_port_unregister(client,output_port[1]);
	free(output_port);
	jack_client_close(client);
	jack_ringbuffer_free(ringbuffer[0]);
	jack_ringbuffer_free(ringbuffer[1]);
}

static int moc_jack_get_rate ()
{
	return rate;
}

static char *moc_jack_get_mixer_channel_name ()
{
	return xstrdup ("soft mixer");
}

static void moc_jack_toggle_mixer_channel ()
{
}

void moc_jack_funcs (struct hw_funcs *funcs)
{
	funcs->init = moc_jack_init;
	funcs->open = moc_jack_open;
	funcs->close = moc_jack_close;
	funcs->play = moc_jack_play;
	funcs->read_mixer = moc_jack_read_mixer;
	funcs->set_mixer = moc_jack_set_mixer;
	funcs->get_buff_fill = moc_jack_get_buff_fill;
	funcs->reset = moc_jack_reset;
	funcs->shutdown = moc_jack_shutdown;
	funcs->get_rate = moc_jack_get_rate;
	funcs->get_mixer_channel_name = moc_jack_get_mixer_channel_name;
	funcs->toggle_mixer_channel = moc_jack_toggle_mixer_channel;
}
