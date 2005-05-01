/* Jack plugin for moc by Alex Norman <alex@neisis.net> 2005
 * moc by Copyright (C) 2004 Damian Pietras <daper@daper.net>
 * use at your own risk
 *
 * Currently only supports 2 channels.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define _XOPEN_SOURCE	500
#include <unistd.h>

#include "audio.h"
#include "main.h"
#include "log.h"
#include "options.h"
#include <stdio.h>
#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>
#include <string.h>
#include <assert.h>

#define RINGBUF_SZ 32768

static struct sound_params params = { 0, 0, 0 };

/* the client */
static jack_client_t *client;
/* an array of output ports */
static jack_port_t **output_port;
/* the ring buffer, used to store the sound data before jack takes it */
static jack_ringbuffer_t *ringbuffer[2];
/* volume */
static jack_default_audio_sample_t volume;
/* indicates if we should be playing or not */
static int play;
/* a value used to scale between integers (-2**15 - 1 to 2**15) and floats (-1 to 1) */
static jack_default_audio_sample_t audio_convert_scalar;

/* this is the function that jack calls to get audio samples from us */
static int moc_jack_process(jack_nframes_t nframes, void *arg ATTR_UNUSED)
{
	unsigned int i;
	jack_default_audio_sample_t myvol;
	jack_default_audio_sample_t *out[2];
	jack_default_audio_sample_t sample[2];

	if (nframes <= 0)
		return 0;
	myvol = play * volume;

	/* get the jack output ports */
	out[0] = (jack_default_audio_sample_t *) jack_port_get_buffer (output_port[0], nframes);
	out[1] = (jack_default_audio_sample_t *) jack_port_get_buffer (output_port[1], nframes);

	/* if we have enough frames then write them out */
	if(((jack_ringbuffer_read_space(ringbuffer[0]) / sizeof(jack_default_audio_sample_t)) >= nframes) &&
			((jack_ringbuffer_read_space(ringbuffer[1]) / sizeof(jack_default_audio_sample_t)) >= nframes)){
		for(i = 0; i < nframes; i++){
			jack_ringbuffer_read(ringbuffer[0], (void *)&sample[0], sizeof(jack_default_audio_sample_t));
			jack_ringbuffer_read(ringbuffer[1], (void *)&sample[1], sizeof(jack_default_audio_sample_t));
			*(out[0] + i) = sample[0] * myvol;
			*(out[1] + i) = sample[1] * myvol;
		}
	/* otherwise write out as many as we have */
	} else{
		i = 0; //init the count
		//so while we haven't emptied the buffers and while we haven't exceeded the nframes
		while(jack_ringbuffer_read_space(ringbuffer[0]) >= sizeof(jack_default_audio_sample_t) && 
					jack_ringbuffer_read_space(ringbuffer[1]) >= sizeof(jack_default_audio_sample_t) &&
					i < nframes){
			//increment the count
			i++;
			jack_ringbuffer_read(ringbuffer[0], (void *)&sample[0], sizeof(jack_default_audio_sample_t));
			jack_ringbuffer_read(ringbuffer[1], (void *)&sample[1], sizeof(jack_default_audio_sample_t));
			*(out[0] + i) = sample[0] * myvol;
			*(out[1] + i) = sample[1] * myvol;
		}
		//finish of by filling with zeros
		for(/*start from where we left off*/; i < nframes; i++)
			*(out[0] + i) = *(out[1] + i) = 0;
	}
	return 0;
}

/* this is called if jack changes its sample rate */
static int moc_jack_update_sample_rate(jack_nframes_t rate, void *arg ATTR_UNUSED)
{
	params.rate = rate;
	return 0;
}

/* callback for jack's error messages */
static void error_callback (const char *msg)
{
	error ("JACK: %s", msg);
}

static void moc_jack_init (struct output_driver_caps *caps)
{
	jack_set_error_function (error_callback);
	
	/* try to become a client of the JACK server */
	if ((client = jack_client_new ("moc")) == 0) {
		fatal ("cannot create client jack server not running?");
		return;
	}
	
	/* allocate memory for an array of 2 output ports */
	output_port = xmalloc(2 * sizeof(jack_port_t *));
	output_port[0] = jack_port_register (client, "output0", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	output_port[1] = jack_port_register (client, "output1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	
	/* do this division so that we don't have to do it in the call back
	 * this number is used to convert between shorts and floats
	 */
	audio_convert_scalar = 1.0 / 32768;
	
	volume = 0.9;
	play = 0;

	/* create the ring buffers */
	ringbuffer[0] = jack_ringbuffer_create(RINGBUF_SZ);
	ringbuffer[1] = jack_ringbuffer_create(RINGBUF_SZ);

	/* set the call back functions, activate the client */
	jack_set_process_callback (client, moc_jack_process, NULL);
	jack_set_sample_rate_callback(client, moc_jack_update_sample_rate, NULL);
	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
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

	caps->max.format = caps->min.format = 2;
	caps->max.rate = caps->min.rate = jack_get_sample_rate (client);
	caps->max.channels = caps->min.channels = 2;
	
	logit ("jack init");
}

static int moc_jack_open (struct sound_params *sound_params)
{
	if (sound_params->format != 2) {
		error ("Unsupported sound format.");
		return 0;
	}
	if (sound_params->channels != 2) {
		error ("Unsupported number of channels");
		return 0;
	}
	
	params = *sound_params;
	logit ("jack open");
	play = 1;

	return 1;
}

static void moc_jack_close ()
{
	params.channels = 0;
	params.rate = 0;
	params.format = 0;
	logit ("jack close");
	play = 0;
}

static int moc_jack_play (const char *buff, const size_t size)
{
	unsigned int i;
	jack_default_audio_sample_t sample[2];
	unsigned int written = 0;
	int temp;
	/* the size will be different in the ring buffer because we convert to 
	 * jack_default_audio_sample_t (floats) 
	 */
	unsigned int size_in_buf = size * sizeof(jack_default_audio_sample_t) / (sizeof(short) * 2);

	assert (size_in_buf <= RINGBUF_SZ);
	
	if (size_in_buf == 0)
		return 0;
	/* wait until there is enough room in the buffer */
	while (jack_ringbuffer_write_space(ringbuffer[0]) < size_in_buf 
			&& jack_ringbuffer_write_space(ringbuffer[1]) < size_in_buf)
		usleep (RINGBUF_SZ / (float)(params.format * params.rate
					* params.channels)/10);
	/* write all the data into the ringbuffers.. convert to jack_default_audio_sample_t first */
	for  (i = 0; i < (size / sizeof(short)); i = i + 2){
		sample[0] = audio_convert_scalar * (jack_default_audio_sample_t)*((short *)buff + i);
		sample[1] = audio_convert_scalar * (jack_default_audio_sample_t)*((short *)buff + i + 1);
		temp = jack_ringbuffer_write(ringbuffer[0], (void *)&sample[0], sizeof(jack_default_audio_sample_t));
		temp += jack_ringbuffer_write(ringbuffer[1], (void *)&sample[1], sizeof(jack_default_audio_sample_t));
		/* since the number of bytes differs between the types we have to do
		 * this conversion to get an accurate number of the input bytes written
		 */
		written += sizeof(short) * temp / sizeof(jack_default_audio_sample_t);
	}
	return written;
}

static int moc_jack_read_mixer ()
{
	return (int)(volume * 100);
}

static void moc_jack_set_mixer (int vol)
{
	volume = (jack_default_audio_sample_t)vol / 100;
}

static int moc_jack_get_buff_fill ()
{
	/* FIXME: should we also use jack_port_get_latency() here? */
	return sizeof(short) * (jack_ringbuffer_read_space(ringbuffer[0]) + jack_ringbuffer_read_space(ringbuffer[1])) / sizeof(jack_default_audio_sample_t);
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
	return jack_get_sample_rate(client);
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
}
