/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * libsidplay2-plugin Copyright (C) 2007 Hendrik Iben <hiben@tzi.de>
 * Enables MOC to play sids via libsidplay2/libsidutils.
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

#ifdef __cplusplus
extern "C" {
#endif

#include "decoder.h"

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

// debug and error are used by this library too...
#undef debug
#undef error

#include <sidplay/sidplay2.h>
#include <sidplay/SidTune.h>
#include <sidplay/builders/resid.h>
#include <sidplay/utils/SidDatabase.h>

#define RESID_ID      "ReSID"
#define OPT_DEFLEN    "SidPlay2_DefaultSongLength"
#define OPT_MINLEN    "SidPlay2_MinimumSongLength"
#define OPT_DATABASE  "SidPlay2_Database"
#define OPT_FREQ      "SidPlay2_Frequency"
#define OPT_PREC      "SidPlay2_Bits"
#define OPT_PMODE     "SidPlay2_PlayMode"
#define OPT_OPTI      "SidPlay2_Optimisation"
#define OPT_START     "SidPlay2_StartAtStart"
#define OPT_SUBTUNES  "SidPlay2_PlaySubTunes"

#define STITLE 0
#define SAUTHOR 1
#define SCOPY 2

#define POOL_SIZE 2

struct sidplay2_data
{
  SidTuneMod * tune;
  SID_EXTERN::sidplay2 *player;
  sid2_config_t cfg;
  ReSIDBuilder *builder;
  int length;
  int *sublengths;
  int songs;
  int startSong;
  int currentSong;
  int timeStart;
  int timeEnd;
  struct decoder_error error;
  int sample_format, frequency, channels;
};

#endif

#ifdef __cplusplus
extern "C" {
#endif

void *sidplay2_open (const char *file);
void sidplay2_close (void *void_data);
void sidplay2_get_error (void *prv_data, struct decoder_error *error);
void sidplay2_info (const char *file_name, struct file_tags *info,
		const int tags_sel);
int sidplay2_seek (void *void_data, int sec);
int sidplay2_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params);
int sidplay2_get_bitrate (void *void_data);
int sidplay2_get_duration (void *void_data);
void sidplay2_get_name (const char *file, char buf[4]);
int sidplay2_our_format_ext (const char *ext);
void destroy ();
void init ();
decoder *plugin_init ();

#ifdef __cplusplus
}
#endif
