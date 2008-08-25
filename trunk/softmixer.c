/*
 * MOC - music on console
 * Copyright (C) 2004-2007 Damian Pietras <daper@daper.net>
 *
 * Softmixer-extension Copyright (C) 2007 Hendrik Iben <hiben@tzi.de>
 * Provides a software-mixer to regulate volume independent from
 * hardware.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#include "softmixer.h"
#include "options.h"
#include "common.h"
#include "files.h"
#include "log.h"

#define swap_32bit_endianess(i32) \
  ( ((i32&0x000000FF)<<24) | ((i32&0x0000FF00)<<8)| \
  ((i32&0x00FF0000)>>8) | ((i32&0xFF000000)>>24) ) 

#define swap_16bit_endianess(i16) \
  ( ((i16&0x00FF)<<8) | ((i16&0xFF00)>>8) )

/* #define DEBUG */

/* public code */

int active;

int mixer_val, mixer_amp, mixer_real;
float mixer_realf;

char *softmixer_name()
{
  return xstrdup((active)?SOFTMIXER_NAME:SOFTMIXER_NAME_OFF);
}

void softmixer_read_config();

void softmixer_init()
{
  active = 0;
  mixer_amp = 100;
  softmixer_set_value(100);
  softmixer_read_config();
  logit("Softmixer initialized");
}

void softmixer_write_config();

void softmixer_shutdown()
{
  if(options_get_int(SOFTMIXER_SAVE_OPTION))
    softmixer_write_config();
  logit("Softmixer stopped");
}

void softmixer_set_value(const int val)
{
  mixer_val = val;

  if(mixer_val<0)
    mixer_val = 0;
  else
    if(mixer_val>100)
      mixer_val = 100;

  mixer_real = (mixer_val * mixer_amp) / 100;

  if(mixer_real < SOFTMIXER_MIN)
    mixer_real = SOFTMIXER_MIN;

  if(mixer_real > SOFTMIXER_MAX)
    mixer_real=SOFTMIXER_MAX;

  mixer_realf = ((float)mixer_real)/100.0f;
}

int softmixer_get_value()
{
  return mixer_val;
}

void softmixer_set_active(int act)
{
  if(act)
    active = 1;
  else
    active = 0;
}

int softmixer_is_active()
{
  return active;
}

/* private code */

int sample_size(long sfmt);

void process_buffer_u8(uint8_t *buf, size_t size);
void process_buffer_s8(int8_t *buf, size_t size);
void process_buffer_u16(uint16_t *buf, size_t size);
void process_buffer_s16(int16_t *buf, size_t size);
void process_buffer_u32(uint32_t *buf, size_t size);
void process_buffer_s32(int32_t *buf, size_t size);
void process_buffer_float(float *buf, size_t size);

void swap_endianess_32(int32_t *buf, size_t size);
void swap_endianess_16(int16_t *buf, size_t size);

void softmixer_read_config()
{
  char *cfname = create_file_name(SOFTMIXER_SAVE_FILE);

  FILE *cf = fopen(cfname, "r");

  if(cf==NULL)
  {
    logit ("Unable to read softmixer configuration");
    return;
  }

  char *linebuffer=NULL;

  int tmp;

  while((linebuffer=read_line(cf)))
  {
    if(
      strncasecmp
      (
          linebuffer
        , SOFTMIXER_CFG_ACTIVE
        , strlen(SOFTMIXER_CFG_ACTIVE)
      ) == 0
    )
    {
      if(sscanf(linebuffer, "%*s %i", &tmp)>0)
        {
          if(tmp>0)
          {
            active = 1;
          }
          else
          {
            active = 0;
          }
        }
    }
    if(
      strncasecmp
      (
          linebuffer
        , SOFTMIXER_CFG_AMP
        , strlen(SOFTMIXER_CFG_AMP)
      ) == 0
    )
    {
      if(sscanf(linebuffer, "%*s %i", &tmp)>0)
        {
          if(tmp>=SOFTMIXER_MIN && tmp<=SOFTMIXER_MAX)
          {
            mixer_amp = tmp;
          }
          else
          {
            logit("Tried to set softmixer amplification out of range.");
          }
        }
    }
    if(
      strncasecmp
      (
          linebuffer
        , SOFTMIXER_CFG_VALUE
        , strlen(SOFTMIXER_CFG_VALUE)
      ) == 0
    )
    {
      if(sscanf(linebuffer, "%*s %i", &tmp)>0)
        {
          if(tmp>=0 && tmp<=100)
          {
            softmixer_set_value(tmp);
          }
          else
          {
            logit("Tried to set softmixer value out of range.");
          }
        }
    }
    free(linebuffer);
  }


  fclose(cf);
}

void softmixer_write_config()
{
  char *cfname = create_file_name(SOFTMIXER_SAVE_FILE);

  FILE *cf = fopen(cfname, "w");

  if(cf==NULL)
  {
    logit ("Unable to write softmixer configuration");
    return;
  }

  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_ACTIVE, active);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_AMP, mixer_amp);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_VALUE, mixer_val);

  fclose(cf);

  logit("Softmixer configuration written");
}

void softmixer_process_buffer(char *buf, size_t size, const struct sound_params *sound_params)
{
#ifdef DEBUG
  logit("Processing %u bytes...", size);
#endif
  if(mixer_real==100)
    return;

  long sound_endianess = sound_params->fmt & SFMT_MASK_ENDIANES;
  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;

  int samplesize = sample_size(sound_format);
  int is_float = (sound_params->fmt & SFMT_MASK_FORMAT) == SFMT_FLOAT;

  int need_endianess_swap = 0;

  if((sound_endianess != SFMT_NE) && (samplesize > 1) && (!is_float))
  {
    need_endianess_swap = 1;
  }

  /* setup samples to perform arithmetic */
  if(need_endianess_swap)
  {
#ifdef DEBUG
    logit("Converting endianess before mixing");
#endif
    if(samplesize == 4)
      swap_endianess_32((int32_t *)buf, size>>2);
    else
      swap_endianess_16((int16_t *)buf, size>>1);
  }

  switch(sound_format)
  {
    case SFMT_U8:
      process_buffer_u8((uint8_t *)buf, size);
      break;
    case SFMT_S8:
      process_buffer_s8((int8_t *)buf, size);
      break;
    case SFMT_U16:
      process_buffer_u16((uint16_t *)buf, size >> 1);
      break;
    case SFMT_S16:
      process_buffer_s16((int16_t *)buf, size >> 1);
      break;
    case SFMT_U32:
      process_buffer_u32((uint32_t *)buf, size >> 2);
      break;
    case SFMT_S32:
      process_buffer_s32((int32_t *)buf, size >> 2);
      break;
    case SFMT_FLOAT:
      process_buffer_float((float *)buf, size >> 1);
      break;
  }
  
  /* restore sample-endianess */
  if(need_endianess_swap)
  {
#ifdef DEBUG
    logit("Restoring endianess after mixing");
#endif
    if(samplesize == 4)
      swap_endianess_32((int32_t *)buf, size>>2);
    else
      swap_endianess_16((int16_t *)buf, size>>1);
  }
}

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


void process_buffer_u8(uint8_t *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    int16_t tmp = buf[i];
    tmp -= (UINT8_MAX>>1);
    tmp *= mixer_real;
    tmp /= 100;
    tmp += (UINT8_MAX>>1);
    if(tmp < 0)
      tmp = 0;
    if(tmp > UINT8_MAX)
      tmp = UINT8_MAX;
    buf[i] = (uint8_t)tmp;
  }
}

void process_buffer_s8(int8_t *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    int16_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 100;
    if( (tmp > INT8_MAX) )
      tmp = INT8_MAX;
    else
      if( (tmp < INT8_MIN ) )
        tmp = INT8_MIN;
    buf[i] = (int8_t)tmp;
  }
}

void process_buffer_u16(uint16_t *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    int32_t tmp = buf[i];
    tmp -= (UINT16_MAX>>1);
    tmp *= mixer_real;
    tmp /= 100;
    tmp += (UINT16_MAX>>1);
    if(tmp < 0)
      tmp = 0;
    if(tmp > UINT16_MAX)
      tmp = UINT16_MAX;
    buf[i] = (uint16_t)tmp;
  }
}

void process_buffer_s16(int16_t *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    int32_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 100;
    if(tmp > INT16_MAX)
      tmp = INT16_MAX;
    else
      if(tmp < INT16_MIN)
        tmp = INT16_MIN;
    buf[i] = (int16_t)tmp;
  }
}

void process_buffer_u32(uint32_t *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    int64_t tmp = buf[i];
    tmp -= (UINT32_MAX>>1);
    tmp *= mixer_real;
    tmp /= 100;
    tmp += (UINT32_MAX>>1);

    if(tmp < 0)
      tmp = 0;
    if(tmp > UINT32_MAX)
      tmp = UINT32_MAX;
    buf[i] = (uint32_t)tmp;
  }
}

void process_buffer_s32(int32_t *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    int64_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 100;
    if(tmp > INT32_MAX)
      tmp = INT32_MAX;
    else
      if(tmp < INT32_MIN)
        tmp = INT32_MIN;
    buf[i] = (int32_t)tmp;
  }
}

void process_buffer_float(float *buf, size_t size)
{
#ifdef DEBUG
  logit("mixing");
#endif
  size_t i;
  for(i=0; i<size; i++)
  {
    float tmp = buf[i];
    tmp *= mixer_realf;
    if(tmp > 1.0f)
      tmp = 1.0f;
    else
      if(tmp < -1.0f)
        tmp = -1.0f;
    buf[i] = tmp;
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
