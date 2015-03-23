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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdint.h>

/* #define DEBUG */

#include "common.h"
#include "audio.h"
#include "audio_conversion.h"
#include "softmixer.h"
#include "options.h"
#include "files.h"
#include "log.h"

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
  if(options_get_bool(SOFTMIXER_SAVE_OPTION))
    softmixer_write_config();
  logit ("Softmixer stopped");
}

void softmixer_set_value(const int val)
{
  mixer_val = CLAMP(0, val, 100);
  mixer_real = (mixer_val * mixer_amp) / 100;
  mixer_real = CLAMP(SOFTMIXER_MIN, mixer_real, SOFTMIXER_MAX);
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

static void process_buffer_u8(uint8_t *buf, size_t samples);
static void process_buffer_s8(int8_t *buf, size_t samples);
static void process_buffer_u16(uint16_t *buf, size_t samples);
static void process_buffer_s16(int16_t *buf, size_t samples);
static void process_buffer_u32(uint32_t *buf, size_t samples);
static void process_buffer_s32(int32_t *buf, size_t samples);
static void process_buffer_float(float *buf, size_t samples);
static void mix_mono_u8(uint8_t *buf, int channels, size_t samples);
static void mix_mono_s8(int8_t *buf, int channels, size_t samples);
static void mix_mono_u16(uint16_t *buf, int channels, size_t samples);
static void mix_mono_s16(int16_t *buf, int channels, size_t samples);
static void mix_mono_u32(uint32_t *buf, int channels, size_t samples);
static void mix_mono_s32(int32_t *buf, int channels, size_t samples);
static void mix_mono_float(float *buf, int channels, size_t samples);

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
          if(RANGE(SOFTMIXER_MIN, tmp, SOFTMIXER_MAX))
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
          if(RANGE(0, tmp, 100))
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
  int do_softmix, do_monomix;

  debug ("Processing %zu bytes...", size);

  do_softmix = active && (mixer_real != 100);
  do_monomix = mix_mono && (sound_params->channels > 1);

  if(!do_softmix && !do_monomix)
    return;

  long sound_endianness = sound_params->fmt & SFMT_MASK_ENDIANNESS;
  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;

  int samplewidth = sfmt_Bps(sound_format);
  int is_float = (sound_params->fmt & SFMT_MASK_FORMAT) == SFMT_FLOAT;

  int need_endianness_swap = 0;

  if((sound_endianness != SFMT_NE) && (samplewidth > 1) && (!is_float))
  {
    need_endianness_swap = 1;
  }

  assert (size % (samplewidth * sound_params->channels) == 0);

  /* setup samples to perform arithmetic */
  if(need_endianness_swap)
  {
    debug ("Converting endianness before mixing");

    if(samplewidth == 4)
      audio_conv_bswap_32((int32_t *)buf, size / sizeof(int32_t));
    else
      audio_conv_bswap_16((int16_t *)buf, size / sizeof(int16_t));
  }

  switch(sound_format)
  {
    case SFMT_U8:
      if(do_softmix)
        process_buffer_u8((uint8_t *)buf, size);
      if(do_monomix)
        mix_mono_u8((uint8_t *)buf, sound_params->channels, size);
      break;
    case SFMT_S8:
      if(do_softmix)
        process_buffer_s8((int8_t *)buf, size);
      if(do_monomix)
        mix_mono_s8((int8_t *)buf, sound_params->channels, size);
      break;
    case SFMT_U16:
      if(do_softmix)
        process_buffer_u16((uint16_t *)buf, size / sizeof(uint16_t));
      if(do_monomix)
        mix_mono_u16((uint16_t *)buf, sound_params->channels, size / sizeof(uint16_t));
      break;
    case SFMT_S16:
      if(do_softmix)
        process_buffer_s16((int16_t *)buf, size / sizeof(int16_t));
      if(do_monomix)
        mix_mono_s16((int16_t *)buf, sound_params->channels, size / sizeof(int16_t));
      break;
    case SFMT_U32:
      if(do_softmix)
        process_buffer_u32((uint32_t *)buf, size / sizeof(uint32_t));
      if(do_monomix)
        mix_mono_u32((uint32_t *)buf, sound_params->channels, size / sizeof(uint32_t));
      break;
    case SFMT_S32:
      if(do_softmix)
        process_buffer_s32((int32_t *)buf, size / sizeof(int32_t));
      if(do_monomix)
        mix_mono_s32((int32_t *)buf, sound_params->channels, size / sizeof(int32_t));
      break;
    case SFMT_FLOAT:
      if(do_softmix)
        process_buffer_float((float *)buf, size / sizeof(float));
      if(do_monomix)
        mix_mono_float((float *)buf, sound_params->channels, size / sizeof(float));
      break;
  }

  /* restore sample-endianness */
  if(need_endianness_swap)
  {
    debug ("Restoring endianness after mixing");

    if(samplewidth == 4)
      audio_conv_bswap_32((int32_t *)buf, size / sizeof(int32_t));
    else
      audio_conv_bswap_16((int16_t *)buf, size / sizeof(int16_t));
  }
}

static void process_buffer_u8(uint8_t *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    int16_t tmp = buf[i];
    tmp -= (UINT8_MAX>>1);
    tmp *= mixer_real;
    tmp /= 100;
    tmp += (UINT8_MAX>>1);
    tmp = CLAMP(0, tmp, UINT8_MAX);
    buf[i] = (uint8_t)tmp;
  }
}

static void process_buffer_s8(int8_t *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    int16_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 100;
    tmp = CLAMP(INT8_MIN, tmp, INT8_MAX);
    buf[i] = (int8_t)tmp;
  }
}

static void process_buffer_u16(uint16_t *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    int32_t tmp = buf[i];
    tmp -= (UINT16_MAX>>1);
    tmp *= mixer_real;
    tmp /= 100;
    tmp += (UINT16_MAX>>1);
    tmp = CLAMP(0, tmp, UINT16_MAX);
    buf[i] = (uint16_t)tmp;
  }
}

static void process_buffer_s16(int16_t *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    int32_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 100;
    tmp = CLAMP(INT16_MIN, tmp, INT16_MAX);
    buf[i] = (int16_t)tmp;
  }
}

static void process_buffer_u32(uint32_t *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    int64_t tmp = buf[i];
    tmp -= (UINT32_MAX>>1);
    tmp *= mixer_real;
    tmp /= 100;
    tmp += (UINT32_MAX>>1);
    tmp = CLAMP(0, tmp, UINT32_MAX);
    buf[i] = (uint32_t)tmp;
  }
}

static void process_buffer_s32(int32_t *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    int64_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 100;
    tmp = CLAMP(INT32_MIN, tmp, INT32_MAX);
    buf[i] = (int32_t)tmp;
  }
}

static void process_buffer_float(float *buf, size_t samples)
{
  size_t i;

  debug ("mixing");

  for(i=0; i<samples; i++)
  {
    float tmp = buf[i];
    tmp *= mixer_realf;
    tmp = CLAMP(-1.0f, tmp, 1.0f);
    buf[i] = tmp;
  }
}

// Mono-Mixing
static void mix_mono_u8(uint8_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    int16_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = MIN(mono, UINT8_MAX);  // can't be negative

    for(c=0; c<channels; c++)
      *buf++ = (uint8_t)mono;

    i+=channels;
  }
}

static void mix_mono_s8(int8_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    int16_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = CLAMP(INT8_MIN, mono, INT8_MAX);

    for(c=0; c<channels; c++)
      *buf++ = (int8_t)mono;

    i+=channels;
  }
}

static void mix_mono_u16(uint16_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    int32_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = MIN(mono, UINT16_MAX);  // can't be negative

    for(c=0; c<channels; c++)
      *buf++ = (uint16_t)mono;

    i+=channels;
  }
}

static void mix_mono_s16(int16_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    int32_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = CLAMP(INT16_MIN, mono, INT16_MAX);

    for(c=0; c<channels; c++)
      *buf++ = (int16_t)mono;

    i+=channels;
  }
}

static void mix_mono_u32(uint32_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    int64_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = MIN(mono, UINT32_MAX);  // can't be negative

    for(c=0; c<channels; c++)
      *buf++ = (uint32_t)mono;

    i+=channels;
  }
}

static void mix_mono_s32(int32_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    int64_t mono = 0;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = CLAMP(INT32_MIN, mono, INT32_MAX);

    for(c=0; c<channels; c++)
      *buf++ = (int32_t)mono;

    i+=channels;
  }
}

static void mix_mono_float(float *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug ("making mono");

  assert (channels > 1);

  while(i < samples)
  {
    float mono = 0.0f;

    for(c=0; c<channels; c++)
      mono += *buf++;

    buf-=channels;

    mono /= channels;
    mono = CLAMP(-1.0f, mono, 1.0f);

    for(c=0; c<channels; c++)
      *buf++ = mono;

    i+=channels;
  }
}
