/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * libmodplug-plugin Copyright (C) 2006 Hendrik Iben <hiben@tzi.de>
 * Enables MOC to play modules via libmodplug (actually just a wrapper around
 * libmodplug's C-wrapper... :-)).
 *
 * Based on ideas from G"urkan Seng"un's modplugplay. A command line
 * interface to the modplugxmms library.
 * Structure of this plugin is an adaption of the libsndfile-plugin from
 * moc.
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

#include <strings.h>
#include <limits.h>
#include <assert.h>
#include <libmodplug/modplug.h>

#define DEBUG

#include "common.h"
#include "io.h"
#include "decoder.h"
#include "log.h"
#include "files.h"
#include "options.h"

// Limiting maximum size for loading a module was suggested by Damian.
// I've never seen such a large module so this should be a safe limit...
#ifndef MAXMODSIZE
#define MAXMODSIZE 1024*1024*42
#endif

ModPlug_Settings settings;

struct modplug_data
{
  ModPlugFile *modplugfile;
  int length;
  char *filedata;
  struct decoder_error error;
};

#if !defined(NDEBUG) && defined(DEBUG)
// this is needed because debugging in plugin_init gets lost
// The alternative is to debug settings when opening a file
// but settings never change so I need a flag to check if it
// has been done...
static int doDebugSettings=1;

static void debugSettings(void)
{
  debug("\n\
ModPlug-Settings:\n\
Oversampling : %s\n\
NoiseReduction : %s\n\
Reverb : %s\n\
MegaBass : %s\n\
Surround : %s\n\
ResamplingMode : %s\n\
Channels : %d\n\
Bits : %d\n\
Frequency : %d\n\
ReverbDepth : %d\n\
ReverbDelay : %d\n\
BassAmount : %d\n\
BassRange : %d\n\
SurroundDepth : %d\n\
SurroundDelay : %d\n\
LoopCount : %d",
   (settings.mFlags & MODPLUG_ENABLE_OVERSAMPLING)?"yes":"no"
  ,(settings.mFlags & MODPLUG_ENABLE_NOISE_REDUCTION)?"yes":"no"
  ,(settings.mFlags & MODPLUG_ENABLE_REVERB)?"yes":"no"
  ,(settings.mFlags & MODPLUG_ENABLE_MEGABASS)?"yes":"no"
  ,(settings.mFlags & MODPLUG_ENABLE_SURROUND)?"yes":"no"
  ,(settings.mResamplingMode == MODPLUG_RESAMPLE_FIR)?"8-tap fir":
    (settings.mResamplingMode == MODPLUG_RESAMPLE_SPLINE)?"spline":
    (settings.mResamplingMode == MODPLUG_RESAMPLE_LINEAR)?"linear":
    (settings.mResamplingMode == MODPLUG_RESAMPLE_NEAREST)?"nearest":"?"
  ,settings.mChannels
  ,settings.mBits
  ,settings.mFrequency
  ,settings.mReverbDepth
  ,settings.mReverbDelay
  ,settings.mBassAmount
  ,settings.mBassRange
  ,settings.mSurroundDepth
  ,settings.mSurroundDelay
  ,settings.mLoopCount
  );
}
#endif

static struct modplug_data *make_modplug_data(const char *file) {
  struct modplug_data *data;

  data = (struct modplug_data *)xmalloc (sizeof(struct modplug_data));

  data->modplugfile = NULL;
  data->filedata = NULL;
  decoder_error_init (&data->error);

  struct io_stream *s = io_open(file, 0);
  if(!io_ok(s)) {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s", file);
    io_close(s);
    return data;
  }

  off_t size = io_file_size(s);

  if (!RANGE(1, size, INT_MAX)) {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Module size unsuitable for loading: %s", file);
    io_close(s);
    return data;
  }

//  if(size>MAXMODSIZE) {
//    io_close(s);
//    decoder_error(&data->error, ERROR_FATAL, 0, "Module to big! 42M ain't enough ? (%s)", file);
//    return data;
//  }

  data->filedata = (char *)xmalloc((size_t)size);

  io_read(s, data->filedata, (size_t)size);
  io_close(s);

  data->modplugfile=ModPlug_Load(data->filedata, (int)size);

  if(data->modplugfile==NULL) {
    free(data->filedata);
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't load module: %s", file);
    return data;
  }

  return data;
}

static void *modplug_open (const char *file)
{
// this is not really needed but without it the calls would still be made
// and thus time gets wasted...
#if !defined(NDEBUG) && defined(DEBUG)
  if(doDebugSettings) {
    doDebugSettings=0;
    debugSettings();
  }
#endif
  struct modplug_data *data = make_modplug_data(file);

  if(data->modplugfile) {
    data->length = ModPlug_GetLength(data->modplugfile);
  }

  if(data->modplugfile) {
    debug ("Opened file %s", file);
  }

  return data;
}

static void modplug_close (void *void_data)
{
  struct modplug_data *data = (struct modplug_data *)void_data;

  if (data->modplugfile) {
    ModPlug_Unload(data->modplugfile);
    free(data->filedata);
  }

  decoder_error_clear (&data->error);
  free (data);
}

static void modplug_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
  struct modplug_data *data = make_modplug_data(file_name);

  if(data->modplugfile==NULL)
    return;

  if(tags_sel & TAGS_TIME) {
    info->time = ModPlug_GetLength(data->modplugfile) / 1000;
    info->filled |= TAGS_TIME;
  }

  if(tags_sel & TAGS_COMMENTS) {
    info->title = xstrdup(ModPlug_GetName(data->modplugfile));
    info->filled |= TAGS_COMMENTS;
  }

  modplug_close(data);
}

static int modplug_seek (void *void_data, int sec)
{
  struct modplug_data *data = (struct modplug_data *)void_data;

  assert (sec >= 0);

  int ms = sec*1000;

  ms = MIN(ms,data->length);

  ModPlug_Seek(data->modplugfile, ms);

  return ms/1000;
}

static int modplug_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
  struct modplug_data *data = (struct modplug_data *)void_data;

  sound_params->channels = settings.mChannels;
  sound_params->rate = settings.mFrequency;
  sound_params->fmt = ((settings.mBits==16)?SFMT_S16:(settings.mBits==8)?SFMT_S8:SFMT_S32) | SFMT_NE;

  return ModPlug_Read(data->modplugfile, buf, buf_len);
}

static int modplug_get_bitrate (void *unused ATTR_UNUSED)
{
  return -1;
}

static int modplug_get_duration (void *void_data)
{
  struct modplug_data *data = (struct modplug_data *)void_data;
  return data->length/1000;
}

static int modplug_our_format_ext(const char *ext)
{
  // Do not include non-module formats in this list (even if
  // ModPlug supports them).  Doing so may cause memory exhaustion
  // in make_modplug_data().
  return
    !strcasecmp (ext, "NONE") ||
    !strcasecmp (ext, "MOD") ||
    !strcasecmp (ext, "S3M") ||
    !strcasecmp (ext, "XM") ||
    !strcasecmp (ext, "MED") ||
    !strcasecmp (ext, "MTM") ||
    !strcasecmp (ext, "IT") ||
    !strcasecmp (ext, "669") ||
    !strcasecmp (ext, "ULT") ||
    !strcasecmp (ext, "STM") ||
    !strcasecmp (ext, "FAR") ||
    !strcasecmp (ext, "AMF") ||
    !strcasecmp (ext, "AMS") ||
    !strcasecmp (ext, "DSM") ||
    !strcasecmp (ext, "MDL") ||
    !strcasecmp (ext, "OKT") ||
    // modplug can do MIDI but not in this form...
    //!strcasecmp (ext, "MID") ||
    !strcasecmp (ext, "DMF") ||
    !strcasecmp (ext, "PTM") ||
    !strcasecmp (ext, "DBM") ||
    !strcasecmp (ext, "MT2") ||
    !strcasecmp (ext, "AMF0") ||
    !strcasecmp (ext, "PSM") ||
    !strcasecmp (ext, "J2B") ||
    !strcasecmp (ext, "UMX");
}

static void modplug_get_error (void *prv_data, struct decoder_error *error)
{
  struct modplug_data *data = (struct modplug_data *)prv_data;

  decoder_error_copy (error, &data->error);
}

static struct decoder modplug_decoder =
{
  DECODER_API_VERSION,
  NULL,
  NULL,
  modplug_open,
  NULL,
  NULL,
  modplug_close,
  modplug_decode,
  modplug_seek,
  modplug_info,
  modplug_get_bitrate,
  modplug_get_duration,
  modplug_get_error,
  modplug_our_format_ext,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
};

struct decoder *plugin_init ()
{
  ModPlug_GetSettings(&settings);
  settings.mFlags = 0;
  settings.mFlags |= options_get_bool("ModPlug_Oversampling")
    ?MODPLUG_ENABLE_OVERSAMPLING:0;
  settings.mFlags |= options_get_bool("ModPlug_NoiseReduction")
    ?MODPLUG_ENABLE_NOISE_REDUCTION:0;
  settings.mFlags |= options_get_bool("ModPlug_Reverb")
    ?MODPLUG_ENABLE_REVERB:0;
  settings.mFlags |= options_get_bool("ModPlug_MegaBass")
    ?MODPLUG_ENABLE_MEGABASS:0;
  settings.mFlags |= options_get_bool("ModPlug_Surround")
    ?MODPLUG_ENABLE_SURROUND:0;
  if(!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "FIR"))
    settings.mResamplingMode = MODPLUG_RESAMPLE_FIR;
  if(!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "SPLINE"))
    settings.mResamplingMode = MODPLUG_RESAMPLE_SPLINE;
  if(!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "LINEAR"))
    settings.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;
  if(!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "NEAREST"))
    settings.mResamplingMode = MODPLUG_RESAMPLE_NEAREST;
  settings.mChannels = options_get_int("ModPlug_Channels");
  settings.mBits = options_get_int("ModPlug_Bits");
  settings.mFrequency = options_get_int("ModPlug_Frequency");
  settings.mReverbDepth = options_get_int("ModPlug_ReverbDepth");
  settings.mReverbDelay = options_get_int("ModPlug_ReverbDelay");
  settings.mBassAmount = options_get_int("ModPlug_BassAmount");
  settings.mBassRange = options_get_int("ModPlug_BassRange");
  settings.mSurroundDepth = options_get_int("ModPlug_SurroundDepth");
  settings.mSurroundDelay = options_get_int("ModPlug_SurroundDelay");
  settings.mLoopCount = options_get_int("ModPlug_LoopCount");
  ModPlug_SetSettings(&settings);
  return &modplug_decoder;
}
