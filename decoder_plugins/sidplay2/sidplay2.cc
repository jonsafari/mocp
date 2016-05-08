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
# include "config.h"
#endif

#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <strings.h>

#include "common.h"
#include "sidplay2.h"
#include "log.h"
#include "options.h"

static SID_EXTERN::sidplay2 *players [POOL_SIZE];

static ReSIDBuilder *builders [POOL_SIZE];

static int playerIndex;

static SID_EXTERN::SidDatabase *database;

static int init_db;

static pthread_mutex_t db_mtx, player_select_mtx;

static int defaultLength;

static int minLength;

static bool startAtStart;

static bool playSubTunes;

static sidplay2_data * make_data()
{
  pthread_mutex_lock(&player_select_mtx);

  playerIndex = (playerIndex+1)%POOL_SIZE;

  struct sidplay2_data *s2d = (struct sidplay2_data *)xmalloc(sizeof(sidplay2_data));

  if(players[playerIndex]==NULL)
    players[playerIndex] = new SID_EXTERN::sidplay2();

  s2d->player = players[playerIndex];

  s2d->cfg = s2d->player->config();

  s2d->cfg.frequency = options_get_int(OPT_FREQ);

  s2d->cfg.precision = options_get_int(OPT_PREC);

  s2d->cfg.optimisation = options_get_int(OPT_OPTI);

  switch(options_get_symb(OPT_PMODE)[0])
  {
    case 'M':
      s2d->cfg.playback = sid2_mono;
      break;
    case 'S':
      s2d->cfg.playback = sid2_stereo;
      break;
    case 'L':
      s2d->cfg.playback = sid2_left;
      break;
    case 'R':
      s2d->cfg.playback = sid2_right;
      break;
    default :
      s2d->cfg.playback = sid2_mono;
  }

  s2d->player->config(s2d->cfg);

  s2d->cfg = s2d->player->config();

  if(builders[playerIndex]==NULL)
    builders[playerIndex] = new ReSIDBuilder( RESID_ID );

  s2d->builder = builders[playerIndex];

  pthread_mutex_unlock(&player_select_mtx);

  if(!(*s2d->builder))
    fatal("sidplay2: Cannot create ReSID-Builder!");

  s2d->builder->create(s2d->player->info().maxsids);

  s2d->builder->sampling(s2d->cfg.frequency);

  s2d->cfg.sidEmulation = s2d->builder;

  s2d->player->config(s2d->cfg);

  s2d->cfg = s2d->player->config();

  s2d->channels = s2d->player->info().channels;

  s2d->frequency = s2d->cfg.frequency;

#ifdef WORDS_BIGENDIAN
  s2d->cfg.sampleFormat = SID2_BIG_SIGNED;
#else
  s2d->cfg.sampleFormat = SID2_LITTLE_SIGNED;
#endif

  s2d->player->config(s2d->cfg);

  s2d->cfg = s2d->player->config();

  switch(s2d->cfg.sampleFormat)
  {
    case SID2_LITTLE_SIGNED:
      switch(s2d->cfg.precision)
      {
        case 8:
          s2d->sample_format = SFMT_S8 | SFMT_LE;
          break;
        case 16:
          s2d->sample_format = SFMT_S16 | SFMT_LE;
          break;
        case 32:
          s2d->sample_format = SFMT_S32 | SFMT_LE;
          break;
        default:
          fatal("sidplay2: Unsupported precision: %i", s2d->cfg.precision);
      }
      break;
    case SID2_LITTLE_UNSIGNED:
      switch(s2d->cfg.precision)
      {
        case 8:
          s2d->sample_format = SFMT_U8 | SFMT_LE;
          break;
        case 16:
          s2d->sample_format = SFMT_U16 | SFMT_LE;
          break;
        case 32:
          s2d->sample_format = SFMT_U32 | SFMT_LE;
          break;
        default:
          fatal("sidplay2: Unsupported precision: %i", s2d->cfg.precision);
      }
      break;
    case SID2_BIG_SIGNED:
      switch(s2d->cfg.precision)
      {
        case 8:
          s2d->sample_format = SFMT_S8 | SFMT_BE;
          break;
        case 16:
          s2d->sample_format = SFMT_S16 | SFMT_BE;
          break;
        case 32:
          s2d->sample_format = SFMT_S32 | SFMT_BE;
          break;
        default:
          fatal("sidplay2: Unsupported precision: %i", s2d->cfg.precision);
      }
      break;
    case SID2_BIG_UNSIGNED:
      switch(s2d->cfg.precision)
      {
        case 8:
          s2d->sample_format = SFMT_U8 | SFMT_BE;
          break;
        case 16:
          s2d->sample_format = SFMT_U16 | SFMT_BE;
          break;
        case 32:
          s2d->sample_format = SFMT_U32 | SFMT_BE;
          break;
        default:
          fatal("sidplay2: Unsupported precision: %i", s2d->cfg.precision);
      }
      break;
    default:
      fatal("sidplay2: Unknown Audio-Format!");
  }

  return s2d;
}

static void init_database()
{
  int cancel = 0;

  pthread_mutex_lock(&db_mtx);

  if(init_db==0)
    cancel = 1;

  init_db = 0;

  pthread_mutex_unlock(&db_mtx);

  if(cancel)
    return;

  char * dbfile = options_get_str(OPT_DATABASE);

  if(dbfile!=NULL && dbfile[0]!='\0')
  {
    database = new SidDatabase();

    if(database->open(dbfile)<0)
    {
      logit("Unable to open SidDatabase %s", dbfile);
      database = NULL;
    }
  }
}

extern "C" void *sidplay2_open(const char *file)
{
  if(init_db)
    init_database();

  struct sidplay2_data *s2d = make_data();

  decoder_error_init(&s2d->error);
  s2d->tune=NULL;
  s2d->length = 0;

  SidTuneMod *st = new SidTuneMod(file);

  if(!(*st))
  {
    decoder_error(&s2d->error, ERROR_FATAL, 0, "Unable to open %s...", file);
    delete st;
    return s2d;
  }

  s2d->songs = st->getInfo().songs;

  s2d->sublengths = new int [s2d->songs];

  s2d->startSong = st->getInfo().startSong;

  s2d->timeStart = 1;

  s2d->timeEnd = s2d->songs;

  if(startAtStart)
    s2d->timeStart = s2d->startSong;

  if(!playSubTunes)
    s2d->timeEnd = s2d->timeStart;

  for(int s=s2d->timeStart; s <= s2d->timeEnd; s++)
  {
    st->selectSong(s);

    if(!(*st))
    {
      decoder_error(&s2d->error, ERROR_FATAL, 0,
                                 "Error determining length of %s", file);
      delete st;
      return s2d;
    }

    if(database!=NULL)
    {
      int dl = database->length(*st);

      if(dl<1)
        dl = defaultLength;

      if(dl<minLength)
        dl = minLength;

      s2d->length += dl;
      s2d->sublengths[s-1] = dl;
    }
    else
    {
      s2d->length += defaultLength;
      s2d->sublengths[s-1] = defaultLength;
    }
  }

  // this should not happen normally...
  if(s2d->length==0)
    s2d->length = defaultLength;

  s2d->currentSong = s2d->timeStart;

  st->selectSong(s2d->currentSong);

  if(!(*st))
  {
    decoder_error(&s2d->error, ERROR_FATAL, 0,
                  "Cannot select first song in %s", file);
    delete st;
    return s2d;
  }

  s2d->tune = st;

  s2d->player->load(st);

  if(!(*s2d->player))
  {
    decoder_error(&s2d->error, ERROR_FATAL, 0, "%s", s2d->player->error());
  }

  s2d->player->fastForward(100);

  return ((void *)s2d);
}

extern "C" void sidplay2_close(void *void_data)
{
  struct sidplay2_data *data = (struct sidplay2_data *)void_data;

  if(data->player!=NULL)
     data->player->load(NULL);

  if(data->tune!=NULL)
    delete data->tune;

  if(data->sublengths!=NULL)
    delete data->sublengths;

  decoder_error_clear (&data->error);
  free(data);
}

extern "C" void sidplay2_get_error (void *prv_data, struct decoder_error *error)
{
  struct sidplay2_data *data = (struct sidplay2_data *)prv_data;

  decoder_error_copy (error, &data->error);
}

extern "C" void sidplay2_info (const char *file_name, struct file_tags *info,
		const int)
{
  if(init_db)
    init_database();

  SidTuneMod *st = new SidTuneMod(file_name);

  if(!(*st))
  {
    delete st;
    return;
  }

  const SidTuneInfo sti = st->getInfo();

  if
  (
    sti.numberOfInfoStrings>0
    && sti.infoString[STITLE]!=NULL
    && strlen(sti.infoString[STITLE])>0
  )
  {
    info->title = trim(sti.infoString[STITLE],strlen(sti.infoString[STITLE]));
    if (info->title)
      info->filled |= TAGS_COMMENTS;
  }

  if
  (
    sti.numberOfInfoStrings>1
    && sti.infoString[SAUTHOR]!=NULL
    && strlen(sti.infoString[SAUTHOR])>0
  )
  {
    info->artist = trim(sti.infoString[SAUTHOR],strlen(sti.infoString[SAUTHOR]));
    if (info->artist)
      info->filled |= TAGS_COMMENTS;
  }

  // Not really album - but close...
  if
  (
    sti.numberOfInfoStrings>2
    && sti.infoString[SCOPY]!=NULL
    && strlen(sti.infoString[SCOPY])>0
  )
  {
    info->album = trim(sti.infoString[SCOPY],strlen(sti.infoString[SCOPY]));
    if (info->album)
      info->filled |= TAGS_COMMENTS;
  }

  info->time = 0;

  int songs = st->getInfo().songs;

  int countStart = 1;

  int countEnd = songs;

  if(startAtStart)
    countStart = st->getInfo().startSong;

  if(!playSubTunes)
    countEnd = countStart;

  for(int s=countStart; s <= countEnd; s++)
  {
    st->selectSong(s);

    if(database!=NULL)
    {
      int dl = database->length(*st);

      if(dl<1)
        dl = defaultLength;

      if(dl<minLength)
        dl = minLength;

      info->time += dl;
    }
    else
    {
     info->time += defaultLength;
    }
  }

  info->filled |= TAGS_TIME;

  delete st;
}

/* Seeking is not reliable because I don't know how to keep track of the
 * difference between the time which MOC is currently playing and how much
 * time has been precached... :-|
 *
 * Generic seeking can't be done because the whole audio would have to be
 * replayed until the position is reached (which would introduce a delay).
 * */
extern "C" int sidplay2_seek (void *, int)
{
  return -1;
}

extern "C" int sidplay2_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
  struct sidplay2_data *data = (struct sidplay2_data *)void_data;

  int seconds = data->player->time() / data->player->timebase();

  int currentLength = data->sublengths[data->currentSong-1];

  if(seconds >= currentLength)
  {
    if(data->currentSong >= data->timeEnd)
      return 0;

    data->player->stop();
    data->currentSong++;
    data->tune->selectSong(data->currentSong);
    data->player->load(data->tune);

    currentLength = data->sublengths[data->currentSong-1];
    seconds = 0;
  }

  sound_params->channels = data->channels;
  sound_params->rate = data->frequency;
  sound_params->fmt = data->sample_format;

  return data->player->play((void *)buf, buf_len);
}

extern "C" int sidplay2_get_bitrate (void *)
{
  return -1;
}

extern "C" int sidplay2_get_duration (void *void_data)
{
  struct sidplay2_data *data = (struct sidplay2_data *)void_data;

  return data->length;
}

extern "C" int sidplay2_our_format_ext(const char *ext)
{
  return
    !strcasecmp (ext, "SID") ||
    !strcasecmp (ext, "MUS");
}

extern "C" void init()
{
  defaultLength = options_get_int(OPT_DEFLEN);

  minLength = options_get_int(OPT_MINLEN);

  startAtStart = options_get_bool(OPT_START);

  playSubTunes = options_get_bool(OPT_SUBTUNES);

  database = NULL;
  init_db = 1;

  playerIndex = POOL_SIZE-1; /* turns to 0 at first use */
}

extern "C" void destroy()
{
  pthread_mutex_destroy(&db_mtx);

  pthread_mutex_destroy(&player_select_mtx);

  if(database!=NULL)
    delete database;

  for(int i=0; i < POOL_SIZE; i++)
  {
    if(players[i]!=NULL)
      delete players[i];
    if(builders[i]!=NULL)
      delete builders[i];
  }
}

static struct decoder sidplay2_decoder =
{
  DECODER_API_VERSION,
  init,
  destroy,
  sidplay2_open,
  NULL,
  NULL,
  sidplay2_close,
  sidplay2_decode,
  sidplay2_seek,
  sidplay2_info,
  sidplay2_get_bitrate,
  sidplay2_get_duration,
  sidplay2_get_error,
  sidplay2_our_format_ext,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
};

extern "C" struct decoder *plugin_init ()
{
  pthread_mutex_init(&db_mtx, NULL);
  pthread_mutex_init(&player_select_mtx, NULL);
  return &sidplay2_decoder;
}
