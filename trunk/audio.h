#ifndef AUDIO_H
#define AUDIO_H

#include <stdlib.h>

/** Sound parameters.
 *
 * A structure describing sound parameters. The format is always PCM signed,
 * native endian for this machine.
 */
struct sound_params
{
	int channels; /*!< Number of channels: 1 or 2 */
	int rate; /*!< Rate in Hz */
	int format; /*!< Nubmer of bytes in the data word (1 - 8 bits
		      or 2 - 16 bits). */
};

/** Functions to control the audio "driver".
 *
 * The structure holds pointers to functions that must be provided by the audio
 * "driver". All functions are executed only by one thread, so you don't need
 * to worry if they are thread safe.
 */
struct hw_funcs
{
	/** Initialize the driver.
	 *
	 * This function is invoked only once when the MOC server starts.
	 */
	void (*init) ();

	/** Clean up at exit.
	 *
	 * This dunction is invoked only once when the MOC server exits. The
	 * audio device is not in use at this moment. The function shoul close
	 * any opened devices and free any resources the driver allocated.
	 * After this function was used, no other functions will be invoked.
	 */
	void (*shutdown) ();

	/** Open the sound device.
	 *
	 * This function should open the sound device with the proper
	 * parameters. The function should return 1 on success and 0 otherwise.
	 * After returning 1 functions like play(), get_buff_fill() can be used.
	 * 
	 * \param sound_params Pointer to the sound_params structure holding
	 * the required poarameters.
	 * \return 1 on success and 0 otherwise.
	 */
	int (*open) (struct sound_params *sound_params);

	/** Close the device.
	 *
	 * Request for closing the device.
	 */
	void (*close) ();

	/** Play sound.
	 *
	 * Play sound provided in the buffer. The sound is in the format
	 * requested when the open() function was invoked. The function should
	 * play all sound in the buffer.
	 * 
	 * \param buff Pointer to the buffer with the sound.
	 * \param size Size (in bytes) of the buffer.
	 * 
	 * \return The number of bytes played or a value less than zero on
	 * error.
	 */
	int (*play) (const char *buff, const size_t size);

	/** Read the volume setting.
	 *
	 * Read the current volume setting. This must work regardless if the
	 * functions open()/close() where used.
	 *
	 * \return Volume value from 0% to 100%.
	 */
	int (*read_mixer) ();

	/** Set the volume setting.
	 *
	 * Set the volume. This must work regardless if the functions
	 * open()/close() where used.
	 *
	 * \param vol Volume from 0% to 100%.
	 */
	void (*set_mixer) (int vol);

	/** Read the hardware/internal buffer fill.
	 *
	 * The function should return the number of bytes of any
	 * hardware or internal buffers are filled. For example: if we play()
	 * 4KB, but only 1KB was really played (could be heared by the user),
	 * the function should return 3072 (3KB).
	 *
	 * \return Current hardware/internal buffer fill in bytes.
	 */
	int (*get_buff_fill) ();

	/** Stop playing immediately.
	 *
	 * Request that the sound should not be played. This should involve
	 * flushing any internal buffer filled with data sent by the play()
	 * function and resetting the device to flush its buffer (if possible).
	 *
	 * \return 1 on success or 0 otherwise.
	 */
	int (*reset) ();

	/** \name Get the sound format.
	 * Those functions should return values like fields from the
	 * sound_params structure. They can be slightly different from what was
	 * provided to open(), but the must by actual parameters (It is
	 * necessary to count time).
	 */
	/*@{*/
	
	/** Get the number of bytes in the word (1 or 2). */
	int (*get_format) ();

	/** Get the rate in Hz. */
	int (*get_rate) ();

	/** Get the number of channels (1 or 2). */
	int (*get_channels) ();
	
	/*@}*/
};

/* Does the parameters p1 and p2 are equal? */
#define sound_params_eq(p1, p2) ((p1).format == (p2).format \
		&& (p1).channels == (p2).channels && (p1).rate == (p2).rate)

void audio_stop ();
void audio_play (const char *fname);
void audio_next ();
void audio_prev ();
void audio_pause ();
void audio_unpause ();
void audio_init ();
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
void audio_plist_delete (const char *file);
int audio_get_ftime (const char *file);
void audio_plist_set_time (const char *file, const int time);
void audio_state_started_playing ();
int audio_plist_get_serial ();
void audio_plist_set_serial (const int serial);
struct file_tags *audio_get_curr_tags ();

#endif
