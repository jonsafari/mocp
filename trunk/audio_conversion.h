#ifndef AUDIO_CONVERSION_H
#define AUDIO_CONVERSION_H

#include <sys/types.h>

#ifdef HAVE_SAMPLERATE
# include <samplerate.h>
#endif

#include "audio.h"

#ifdef __cplusplus
extern "C" {
#endif

struct audio_conversion
{
	struct sound_params from;
	struct sound_params to;
	
#ifdef HAVE_SAMPLERATE
	SRC_STATE *src_state;
	float *resample_buf;
	size_t resample_buf_nsamples; /* in samples ( sizeof(float) ) */
#endif
	
};

int audio_conv_new (struct audio_conversion *conv,
		const struct sound_params *from,
		const struct sound_params *to);
char *audio_conv (struct audio_conversion *conv,
		const char *buf, const size_t size, size_t *conv_len);
void audio_conv_destroy (struct audio_conversion *conv);

#ifdef __cplusplus
}
#endif

#endif
