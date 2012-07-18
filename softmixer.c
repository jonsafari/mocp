/*
 * MOC - music on console
 * Copyright (C) 2004-2008 Damian Pietras <daper@daper.net>
 *
 * Softmixer-extension Copyright (C) 2007-2008 Hendrik Iben <hiben@tzi.de>
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

#include "common.h"
#include "softmixer.h"
#include "audio_helper.h"
#include "options.h"
#include "files.h"
#include "log.h"

/* #define DEBUG */

static int active;
static int mix_mono;
static int mixer_val, mixer_amp, mixer_real;
static float mixer_realf;

static void softmixer_read_config();
static void softmixer_write_config();

/* public code */

char *softmixer_name()
{
  return xstrdup((active)?SOFTMIXER_NAME:SOFTMIXER_NAME_OFF);
}

void softmixer_init()
{
  active = 0;
  mix_mono = 0;
  mixer_amp = 100;
  softmixer_set_value(100);
  softmixer_read_config();
  logit ("Softmixer initialized");
}

void softmixer_shutdown()
{
  if(options_get_int(SOFTMIXER_SAVE_OPTION))
    softmixer_write_config();
  logit ("Softmixer stopped");
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

void softmixer_set_mono(int mono)
{
  if(mono)
    mix_mono = 1;
  else
    mix_mono = 0;
}

int softmixer_is_mono()
{
  return mix_mono;
}

/* private code */

static void process_buffer_u8(uint8_t *buf, size_t size);
static void process_buffer_s8(int8_t *buf, size_t size);
static void process_buffer_u16(uint16_t *buf, size_t size);
static void process_buffer_s16(int16_t *buf, size_t size);
static void process_buffer_u32(uint32_t *buf, size_t size);
static void process_buffer_s32(int32_t *buf, size_t size);
static void process_buffer_float(float *buf, size_t size);
static void mix_mono_u8(uint8_t *buf, int channels, size_t size);
static void mix_mono_s8(int8_t *buf, int channels, size_t size);
static void mix_mono_u16(uint16_t *buf, int channels, size_t size);
static void mix_mono_s16(int16_t *buf, int channels, size_t size);
static void mix_mono_u32(uint32_t *buf, int channels, size_t size);
static void mix_mono_s32(int32_t *buf, int channels, size_t size);
static void mix_mono_float(float *buf, int channels, size_t size);

static void softmixer_read_config()
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
            logit ("Tried to set softmixer amplification out of range.");
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
            logit ("Tried to set softmixer value out of range.");
          }
        }
    }
    if(
      strncasecmp
      (
          linebuffer
        , SOFTMIXER_CFG_MONO
        , strlen(SOFTMIXER_CFG_MONO)
      ) == 0
    )
    {
      if(sscanf(linebuffer, "%*s %i", &tmp)>0)
        {
          if(tmp>0)
          {
            mix_mono = 1;
          }
          else
          {
            mix_mono = 0;
          }
        }
    }

    free(linebuffer);
  }


  fclose(cf);
}

static void softmixer_write_config()
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
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_MONO, mix_mono);

  fclose(cf);

  logit ("Softmixer configuration written");
}

void softmixer_process_buffer(char *buf, size_t size, const struct sound_params *sound_params)
{
  debug ("Processing %u bytes...", size);

  if(mixer_real==100 && !mix_mono)
    return;

  int do_softmix = mixer_real != 100;

  long sound_endianness = sound_params->fmt & SFMT_MASK_ENDIANNESS;
  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;

  int samplesize = sample_size(sound_format);
  int is_float = (sound_params->fmt & SFMT_MASK_FORMAT) == SFMT_FLOAT;

  int need_endianness_swap = 0;

  if((sound_endianness != SFMT_NE) && (samplesize > 1) && (!is_float))
  {
    need_endianness_swap = 1;
  }

  /* setup samples to perform arithmetic */
  if(need_endianness_swap)
  {
    debug ("Converting endianness before mixing");

    if(samplesize == 4)
      swap_endianness_32((int32_t *)buf, size / sizeof(int32_t));
    else
      swap_endianness_16((int16_t *)buf, size / sizeof(int16_t));
  }

  switch(sound_format)
  {
    case SFMT_U8:
      if(do_softmix)
        process_buffer_u8((uint8_t *)buf, size);
      if(mix_mono)
        mix_mono_u8((uint8_t *)buf, sound_params->channels, size);
      break;
    case SFMT_S8:
      if(do_softmix)
        process_buffer_s8((int8_t *)buf, size);
      if(mix_mono)
        mix_mono_s8((int8_t *)buf, sound_params->channels, size);
      break;
    case SFMT_U16:
      if(do_softmix)
        process_buffer_u16((uint16_t *)buf, size / sizeof(uint16_t));
      if(mix_mono)
        mix_mono_u16((uint16_t *)buf, sound_params->channels, size / sizeof(uint16_t));
      break;
    case SFMT_S16:
      if(do_softmix)
        process_buffer_s16((int16_t *)buf, size / sizeof(int16_t));
      if(mix_mono)
        mix_mono_s16((int16_t *)buf, sound_params->channels, size / sizeof(int16_t));
      break;
    case SFMT_U32:
      if(do_softmix)
        process_buffer_u32((uint32_t *)buf, size / sizeof(uint32_t));
      if(mix_mono)
        mix_mono_u32((uint32_t *)buf, sound_params->channels, size / sizeof(uint32_t));
      break;
    case SFMT_S32:
      if(do_softmix)
        process_buffer_s32((int32_t *)buf, size / sizeof(int32_t));
      if(mix_mono)
        mix_mono_s32((int32_t *)buf, sound_params->channels, size / sizeof(int32_t));
      break;
    case SFMT_FLOAT:
      if(do_softmix)
        process_buffer_float((float *)buf, size / sizeof(float));
      if(mix_mono)
        mix_mono_float((float *)buf, sound_params->channels, size / sizeof(float));
      break;
  }

  /* restore sample-endianness */
  if(need_endianness_swap)
  {
    debug ("Restoring endianness after mixing");

    if(samplesize == 4)
      swap_endianness_32((int32_t *)buf, size / sizeof(int32_t));
    else
      swap_endianness_16((int16_t *)buf, size / sizeof(int16_t));
  }
}

static void process_buffer_u8(uint8_t *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

static void process_buffer_s8(int8_t *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

static void process_buffer_u16(uint16_t *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

static void process_buffer_s16(int16_t *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

static void process_buffer_u32(uint32_t *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

static void process_buffer_s32(int32_t *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

static void process_buffer_float(float *buf, size_t size)
{
  size_t i;

  debug ("mixing");

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

// Mono-Mixing
static void mix_mono_u8(uint8_t *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    int16_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > UINT8_MAX)
      mono = UINT8_MAX;
    // can't be negative

    for(c=0; c<channels; c++)
      *buf++ = (uint8_t)mono;

    i+=channels;
  }
}

static void mix_mono_s8(int8_t *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    int16_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > INT8_MAX)
      mono = INT8_MAX;
    else
      if(mono < INT8_MIN)
        mono = INT8_MIN;

    for(c=0; c<channels; c++)
      *buf++ = (int8_t)mono;

    i+=channels;
  }
}

static void mix_mono_u16(uint16_t *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    int32_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > UINT16_MAX)
      mono = UINT16_MAX;
    // can't be negative

    for(c=0; c<channels; c++)
      *buf++ = (uint16_t)mono;

    i+=channels;
  }
}

static void mix_mono_s16(int16_t *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    int32_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > INT16_MAX)
      mono = INT16_MAX;
    else
      if(mono < INT16_MIN)
        mono = INT16_MIN;

    for(c=0; c<channels; c++)
      *buf++ = (int16_t)mono;

    i+=channels;
  }
}

static void mix_mono_u32(uint32_t *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    int64_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > UINT32_MAX)
      mono = UINT32_MAX;
    // can't be negative

    for(c=0; c<channels; c++)
      *buf++ = (uint32_t)mono;

    i+=channels;
  }
}

static void mix_mono_s32(int32_t *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    int64_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > INT32_MAX)
      mono = INT32_MAX;
    else
      if(mono < INT32_MIN)
        mono = INT32_MIN;

    for(c=0; c<channels; c++)
      *buf++ = (int32_t)mono;

    i+=channels;
  }
}

static void mix_mono_float(float *buf, int channels, size_t size)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  if(channels < 2)
    return;

  while(i < size)
  {
    float mono = 0.0f;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;

    if(mono > 1.0f)
      mono = 1.0f;
    else
      if(mono < -1.0f)
        mono = -1.0f;

    for(c=0; c<channels; c++)
      *buf++ = mono;

    i+=channels;
  }
}
