#ifndef AUDIO_H
#define AUDIO_H

struct sound_params
{
	int channels;
	int rate;
	int format; /* in bytes */
};

/* Functions of sound driver. */
struct hw_funcs
{
	void (*init) ();
	void (*shutdown) ();
	int (*open) (struct sound_params *sound_params);
	void (*close) ();
	int (*play) (const char *buff, const size_t size);
	int (*read_mixer) ();
	void (*set_mixer) (int vol);
	int (*get_buff_fill) ();
	int (*reset) ();
	int (*get_format) ();
	int (*get_rate) ();
	int (*get_channels) ();
};

void audio_stop ();
void audio_play (const char *fname);
void audio_next ();
void audio_pause ();
void audio_unpause ();
void audio_init (const char *sound_driver);
void audio_exit ();
void audio_seek (const int sec);

int audio_open (struct sound_params *sound_params);
int audio_send_buf (const char *buf, const size_t size);
int audio_send_pcm (const char *buf, const size_t size);
void audio_reset ();
int audio_get_bps ();
int audio_get_buf_fill ();
void audio_close ();
int audio_get_time ();
int audio_get_state ();
void audio_plist_add (const char *file);
void audio_plist_clear ();
char *audio_get_sname ();
void audio_set_mixer (const int val);
int audio_get_mixer ();

#endif
