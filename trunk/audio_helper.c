#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "common.h"
#include "audio_helper.h"
#include "audio.h"

#define swap_32bit_endianness(i32) \
  ( ((i32&0x000000FF)<<24) | ((i32&0x0000FF00)<<8)| \
  ((i32&0x00FF0000)>>8) | ((i32&0xFF000000)>>24) )

#define swap_16bit_endianness(i16) \
  ( ((i16&0x00FF)<<8) | ((i16&0xFF00)>>8) )

int sample_size(long sfmt)
{
  long fmt = sfmt & SFMT_MASK_FORMAT;
  switch(fmt)
  {
    case SFMT_U8:
    case SFMT_S8:
      return 1;
    case SFMT_U16:
    case SFMT_S16:
      return 2;
    case SFMT_U32:
    case SFMT_S32:
      return 4;
    case SFMT_FLOAT:
      return 2;
    default:
      return -1;
  }
}

void swap_endianness_32(int32_t *buf, size_t size)
{
  size_t i;
  for(i=0; i<size; i++)
  {
    buf[i] = swap_32bit_endianness(buf[i]);
  }
}

void swap_endianness_16(int16_t *buf, size_t size)
{
  size_t i;
  for(i=0; i<size; i++)
  {
    buf[i] = swap_16bit_endianness(buf[i]);
  }
}

