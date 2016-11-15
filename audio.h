#ifndef AUDIO_H
#define AUDIO_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sound formats.
 *
 * Sound format bits. Only one can be set in the format, the exception is
 * when we want to hold a list of supported formats - they can be bitwise-or'd.
 */
enum sfmt_fmt
{
	SFMT_S8 =    0x00000001, /*!< signed 8-bit */
	SFMT_U8	=    0x00000002, /*!< unsigned 8-bit */
	SFMT_S16 =   0x00000004, /*!< signed 16-bit */
	SFMT_U16 =   0x00000008, /*!< unsigned 16-bit */
	SFMT_S32 =   0x00000010, /*!< signed 24-bit (LSB is 0) */
	SFMT_U32 =   0x00000020, /*!< unsigned 24-bit (LSB set to 0) */
	SFMT_FLOAT = 0x00000040  /*!< float in range -1.0 to 1.0 */
};

/** Sample endianness.
 *
 * Sample endianness - one of them must be set for 16-bit and 24-bit formats.
 */
enum sfmt_endianness
{
	SFMT_LE = 0x00001000, /*!< little-endian */
	SFMT_BE = 0x00002000, /*!< big-endian */

/** Define native endianness to SFMT_LE or SFMT_BE. */
#ifdef WORDS_BIGENDIAN
	SFMT_NE = SFMT_BE
#else
	SFMT_NE = SFMT_LE
#endif
};

/** @name Masks for the sample format.
 *
 * Masks used to extract only one type of information from the sound format.
 */
/*@{*/
#define SFMT_MASK_FORMAT     0x00000fff /*!< sample format */
#define SFMT_MASK_ENDIANNESS 0x00003000 /*!< sample endianness */
/*@}*/

/** Return a value other than 0 if the sound format seems to be proper. */
#define sound_format_ok(f) (((f) & SFMT_MASK_FORMAT) \
		&& (((f) & (SFMT_S8 | SFMT_U8 | SFMT_FLOAT)) \
			|| (f) & SFMT_MASK_ENDIANNESS))

/** Change the sample format to new_fmt (without endianness). */
#define sfmt_set_fmt(f, new_fmt) (((f) & ~SFMT_MASK_FORMAT) | (new_fmt))

/** Change the sample format endianness to endian. */
#define sfmt_set_endian(f, endian) (((f) & ~SFMT_MASK_ENDIANNESS) | (endian))

/** Sound parameters.
 *
 * A structure describing sound parameters. The format is always PCM signed,
 * native endian for this machine.
 */
struct sound_params
{
	int channels; /*!< Number of channels: 1 or 2 */
	int rate; /*!< Rate in Hz */
	long fmt; /*!< Format of the samples (SFMT_* bits) */
};

/** Output driver capabilities.
 *
 * A structure describing the output driver capabilities.
 */
struct output_driver_caps
{
	int min_channels; /*!< Minimum number of channels */
	int max_channels; /*!< Maximum number of channels */
	long formats; /*!< Supported sample formats (or'd sfmt_fmt mask
			with endianness') */
};

/** \struct hw_funcs
 * Functions to control the audio "driver".
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
	 *
	 * \param caps Capabilities of the driver which must be filled by the
	 * function.
	 * \return 1 on success and 0 otherwise.
	 */
	int (*init) (struct output_driver_caps *caps);

	/** Clean up at exit.
	 *
	 * This function is invoked only once when the MOC server exits. The
	 * audio device is not in use at this moment. The function should close
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
	 * The sample rate of the driver can differ from the requested rate.
	 * If so, get_rate() should return the actual rate.
	 *
	 * \param sound_params Pointer to the sound_params structure holding
	 * the required parameters.
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
	 * 4KB, but only 1KB was really played (could be heard by the user),
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

	/** Get the current sample rate setting.
	 *
	 * Get the actual sample rate setting of the audio driver.
	 *
	 * \return Sample rate in Hz.
	 */
	int (*get_rate) ();

	/** Toggle the mixer channel.
	 *
	 * Toggle between the first and the second mixer channel.
	 */
	void (*toggle_mixer_channel) ();

	/** Get the mixer channel's name.
	 *
	 * Get the currently used mixer channel's name.
	 *
	 * \return malloc()ed channel's name.
	 */
	char * (*get_mixer_channel_name) ();
};

/* Are the parameters p1 and p2 equal? */
#define sound_params_eq(p1, p2) ((p1).fmt == (p2).fmt \
		&& (p1).channels == (p2).channels && (p1).rate == (p2).rate)

/* Maximum size of a string needed to hold the value returned by sfmt_str(). */
#define SFMT_STR_MAX	265

char *sfmt_str (const long format, char *msg, const size_t buf_size);
int sfmt_Bps (const long format);
int sfmt_same_bps (const long fmt1, const long fmt2);

void audio_stop ();
void audio_play (const char *fname);
void audio_next ();
void audio_prev ();
void audio_pause ();
void audio_unpause ();
void audio_initialize ();
void audio_exit ();
void audio_seek (const int sec);
void audio_jump_to (const int sec);

int audio_open (struct sound_params *sound_params);
int audio_send_buf (const char *buf, const size_t size);
int audio_send_pcm (const char *buf, const size_t size);
void audio_reset ();
int audio_get_bpf ();
int audio_get_bps ();
int audio_get_buf_fill ();
void audio_close ();
int audio_get_time ();
int audio_get_state ();
int audio_get_prev_state ();
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
char *audio_get_mixer_channel_name ();
void audio_toggle_mixer_channel ();
void audio_plist_move (const char *file1, const char *file2);
void audio_queue_add (const char *file);
void audio_queue_delete (const char *file);
void audio_queue_clear ();
void audio_queue_move (const char *file1, const char *file2);
struct plist* audio_queue_get_contents ();

#ifdef __cplusplus
}
#endif

#endif
