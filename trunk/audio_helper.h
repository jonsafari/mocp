#ifndef AUDIO_HELPER_H
#define AUDIO_HELPER_H

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define swap_32bit_endianess(i32) \
  ( ((i32&0x000000FF)<<24) | ((i32&0x0000FF00)<<8)| \
  ((i32&0x00FF0000)>>8) | ((i32&0xFF000000)>>24) ) 

#define swap_16bit_endianess(i16) \
  ( ((i16&0x00FF)<<8) | ((i16&0xFF00)>>8) )

void swap_endianess_32(int32_t *buf, size_t size);
void swap_endianess_16(int16_t *buf, size_t size);

int sample_size(long sfmt);

#ifdef __cplusplus
}
#endif

#endif
