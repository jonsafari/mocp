#!/bin/bash

#
# MOC - music on console
# Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
#
# maketests.sh Copyright (C) 2012 John Fitzgerald
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#

#
# TODO: - Add other supported formats.
#

AUDIO=false
VIDEO=false
FFMPEG="$(which avconv 2>/dev/null || which ffmpeg 2>/dev/null)"
SOX="$(which sox 2>/dev/null)"
SYNTH="synth 10 sine 440 vol 0.5"

# Clean error termination.
function die {
  echo $@ > /dev/stderr
  exit 1
}

[[ -x "$SOX" ]] || die This script requires the SoX package.

# Provide usage information.
function usage () {
  echo "Usage: ${0##*/} [-a] [--audio] [-h] [--video] [FORMAT ...]"
}

# Provide help information.
function help () {
  echo
  echo "MOC test file generation tool"
  echo
  usage
  echo
  echo "  -a|--all        Generate all formats"
  echo "     --audio      Generate all audio formats"
  echo "  -h|--help       This help information"
  echo "     --video      Generate all video formats"
  echo
  echo "Supported audio formats: flac mp3 vorbis wav"
  echo "Supported video formats: vob"
  echo
}

# Generate FLAC audio test files.
function flac {
  echo "Generating FLAC audio test files"
  for r in 8000 16000 24000 32000 48000 96000 192000 \
           11025 22050 44100 88200 176400
  do
    for b in 16 32
    do
      $SOX -b$b -c1 -r$r -e signed -n -L sinewave-s${b}le-1-$r.flac $SYNTH
      $SOX -b$b -c2 -r$r -e signed -n -L sinewave-s${b}le-2-$r.flac $SYNTH
    done
  done
}

# Generate MP3 audio test files.
function mp3 {
  echo "Generating MP3 audio test files"
  for r in 8000 16000 24000 32000 48000 11025 22050 44100
  do
    for c in 1 2
    do
      $SOX -b8 -c$c -r$r -n sinewave-u8-$c-$r.mp3 $SYNTH
      for b in 16 24 32
      do
        $SOX -b$b -c$c -r$r -n -L sinewave-s${b}le-$c-$r.mp3 $SYNTH
        $SOX -b$b -c$c -r$r -n -B sinewave-s${b}be-$c-$r.mp3 $SYNTH
      done
    done
  done
}

# Generate VOB video test files.
function vob {
  [[ -x "$FFMPEG" ]] || return
  echo "Generating VOB video test files"
  for r in 16000 22050 24000 32000 44100 48000
  do
    for c in 1 2
    do
      $FFMPEG -f rawvideo -pix_fmt yuv420p -s 320x240 -r 30000/1001 \
              -i /dev/zero \
              -f s16le -c pcm_s16le -ac 2 -ar 48000 \
              -i <($SOX -q -b16 -c2 -r 48000 -e signed -n -L -t s16 - $SYNTH) \
              -vcodec mpeg2video -acodec mp2 -shortest -ac $c -ar $r \
              -y sinewave-s16le-$c-$r.vob > /dev/null  2>&1
    done
  done
}

# Generate Ogg/Vorbis audio test files.
function vorbis {
  echo "Generating Ogg/Vorbis audio test files"
  for r in 8000 16000 24000 32000 48000 96000 192000 \
           11025 22050 44100 88200 176400
  do
    $SOX -b16 -c1 -r$r -e signed -n -L sinewave-s16le-1-$r.ogg $SYNTH
    $SOX -b16 -c2 -r$r -e signed -n -L sinewave-s16le-2-$r.ogg $SYNTH
  done
}

# Generate WAV audio test files.
function wav {
  echo "Generating WAV audio test files"
  for r in 8000 16000 24000 32000 48000 96000 192000 \
           11025 22050 44100 88200 176400
  do
    for c in 1 2
    do
      $SOX -b8 -c$c -r$r -n sinewave-u8-$c-$r.wav $SYNTH
      $SOX -b16 -c$c -r$r -n -B sinewave-s16be-$c-$r.wav $SYNTH
      for b in 16 24 32
      do
        $SOX -b$b -c$c -r$r -n -L sinewave-s${b}le-$c-$r.wav $SYNTH
      done
    done
  done
}

# Directory safety check.
ls sinewave-* > /dev/null 2>&1 && {
  echo
  echo "This script generates many filenames starting with 'sinewave-' in the"
  echo "current directory which already contains similarly named files."
  echo "Running it in this directory is a really, really bad idea.  (In fact,"
  echo "it's probably not wise to run this script in a non-empty directory at"
  echo "all.)  So we're aborting in the interests of safety."
  echo
  exit 1
} > /dev/stderr

# Process command line options.
for OPTS
do
  case $1 in
   -a|--all) AUDIO=true
             VIDEO=true
             ;;
    --audio) AUDIO=true
             ;;
  -h|--help) help
             exit 0
             ;;
    --video) VIDEO=true
             ;;
         -*) echo Unrecognised option: $1
             usage > /dev/stderr
             exit 1
             ;;
          *) break
             ;;
  esac
  shift
done

# Generate all audio formats.
$AUDIO && {
  flac
  mp3
  vorbis
  wav
}

# Generate all video formats.
$VIDEO && {
  vob
}

# Generate specified formats.
for ARGS
do
  case $1 in
  flac|mp3|vorbis|wav)
      $1
      ;;
  vob)
      $1
      ;;
  *)  echo "*** Unsupported format: $1"
      echo
      ;;
  esac
  shift
done

exit 0
