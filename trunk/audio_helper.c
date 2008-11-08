#include "audio_helper.h"

#include "audio.h"

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

void swap_endianess_32(int32_t *buf, size_t size)
{
  size_t i;
  for(i=0; i<size; i++)
  {
    buf[i] = swap_32bit_endianess(buf[i]);
  }
}

void swap_endianess_16(int16_t *buf, size_t size)
{
  size_t i;
  for(i=0; i<size; i++)
  {
    buf[i] = swap_16bit_endianess(buf[i]);
  }
}

