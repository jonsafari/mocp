#ifndef AUDIO_HELPER_H
#define AUDIO_HELPER_H

#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void swap_endianness_32(int32_t *buf, size_t size);
void swap_endianness_16(int16_t *buf, size_t size);

int sample_size(long sfmt);

#ifdef __cplusplus
}
#endif

#endif
