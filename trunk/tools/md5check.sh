#!/bin/bash

#
# MOC - music on console
# Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
#
# md5check.sh Copyright (C) 2012 John Fitzgerald
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#

#
# TODO: - Make format, rate and channels explicit where possible.
#       - Add remaining decoder check functions.
#       - Fix decoders with genuine MD5 mismatches.
#       - Fix decoders with genuine format mismatches.
#       - Handle format mismatches which aren't genuine.
#       - Possibly rewrite in Perl for speed.
#

declare -A UNKNOWN
declare -A UNSUPPORTED

EXTRA=false
IGNORE=false
RC=0
SILENT=false
TREMOR=false
VERBOSE=false

# Clean error termination.
function die {
  echo '***' $@ > /dev/stderr
  exit 1
}

# Provide usage information.
function usage () {
  echo "Usage: ${0##*/} [-e] [-s|-v] [LOGFILE]"
  echo "       ${0##*/} -h"
}

# Provide help information.
function help () {
  echo
  echo "MOC MD5 sum checking tool"
  echo
  usage
  echo
  echo "  -e|--extra      Perform extra checks"
  echo "  -h|--help       This help information"
  echo "  -i|--ignore     Ignore known problems"
  echo "  -s|--silent     Output no results"
  echo "  -v|--verbose    Output all results"
  echo
  echo "  LOGFILE         MOC server log file name, or '-' for stdin"
  echo "                  (default: 'mocp_server_log' in the current directory)"
  echo
  echo "Exit codes:       0 - No errors or mismatches"
  echo "                  1 - Error occurred"
  echo "                  2 - Mismatch found"
  echo
}

# Check the FAAD decoder's samples.
FAAD=$(which faad 2>/dev/null)
function aac () {
  local ENDIAN OPTS

  [[ -x "$FAAD" ]] || die faad2 not installed

  [[ "${FMT:0:1}" = "f" ]] && ENDIAN=le
  OPTS="-w -q -f2"

  [[ "${FMT:1:2}" = "16" ]] && OPTS="$OPTS -b1"
  [[ "${FMT:1:2}" = "24" ]] && OPTS="$OPTS -b2"
  [[ "${FMT:1:2}" = "32" ]] && OPTS="$OPTS -b3"

  SUM2=$($FAAD $OPTS "$FILE" | md5sum)
  LEN2=$($FAAD $OPTS "$FILE" | wc -c)
}

# Check the FFmpeg decoder's samples.
FFMPEG=$(which avconv 2>/dev/null || which ffmpeg 2>/dev/null)
function ffmpeg () {
  local ENDIAN OPTS

  [[ -x "$FFMPEG" ]] || die ffmpeg/avconv not installed

  [[ "${FMT:0:1}" = "f" ]] && ENDIAN=le
  OPTS="-ac $CHANS -ar $RATE -f $FMT$ENDIAN"

  SUM2="$($FFMPEG -i "$FILE" $OPTS - </dev/null 2>/dev/null | md5sum)"
  LEN2=$($FFMPEG -i "$FILE" $OPTS - </dev/null 2>/dev/null | wc -c)

  [[ "$($FFMPEG -i "$FILE" </dev/null 2>&1)" =~ Audio:\ .*\ (mono|stereo) ]] || \
    IGNORE_SUM=$IGNORE
}

# Check the FLAC decoder's samples.
SOX=$(which sox 2>/dev/null)
function flac () {
  local OPTS

  [[ -x "$SOX" ]] || die "SoX (for flac) not installed"

  [[ "${FMT:0:1}" = "s" ]] && OPTS="-e signed" || OPTS="-e unsigned"
  [[ "${FMT:1:1}" = "8" ]] && OPTS="$OPTS -b8 -L"
  [[ "${FMT:1:2}" = "16" ]] && OPTS="$OPTS -b16"
  [[ "${FMT:1:2}" = "24" ]] && OPTS="$OPTS -b24"
  [[ "${FMT:1:2}" = "32" ]] && OPTS="$OPTS -b32"
  [[ "$FMT" =~ "le" ]] && OPTS="$OPTS -L"
  [[ "$FMT" =~ "be" ]] && OPTS="$OPTS -B"
  OPTS="$OPTS -r$RATE -c$CHANS"

  SUM2=$($SOX "$FILE" $OPTS -t raw - | md5sum)
  LEN2=$($SOX "$FILE" $OPTS -t raw - | wc -c)
}

# Check the Ogg/Vorbis decoder's samples.
OGGDEC=$(which oggdec 2>/dev/null)
function vorbis () {
  [[ -x "$OGGDEC" ]] || die oggdec not installed
  SUM2="$($OGGDEC -RQ -o - "$FILE" | md5sum)"
  LEN2=$($OGGDEC -RQ -o - "$FILE" | wc -c)
}

# Check the LibSndfile decoder's samples.
SOX=$(which sox 2>/dev/null)
function sndfile () {
  # LibSndfile doesn't have a decoder, use SoX.
  [[ -x "$SOX" ]] || die "sox (for sndfile) not installed"
  SUM2="$($SOX "$FILE" -t f32 - | md5sum)"
  LEN2=$($SOX "$FILE" -t f32 - | wc -c)
  [[ "$NAME" == *-s32le-* ]] && IGNORE_SUM=$IGNORE
}

# Check the MP3 decoder's samples.
SOX=$(which sox 2>/dev/null)
function mp3 () {
  # Lame's decoder only does 16-bit, use SoX.
  [[ -x "$SOX" ]] || die "sox (for mp3) not installed"
  SUM2="$($SOX "$FILE" -t s32 - | md5sum)"
  LEN2=$($SOX "$FILE" -t s32 - | wc -c)
  IGNORE_SUM=$IGNORE
  IGNORE_LEN=$IGNORE
}

# Check the Speex decoder's samples.
SPEEX=$(which speexdec 2>/dev/null)
function speex () {
  [[ -x "$SPEEX" ]] || die speexdec not installed
  SUM2="$($SPEEX "$FILE" - 2>/dev/null | md5sum)"
  LEN2=$($SPEEX "$FILE" - 2>/dev/null | wc -c)
  IGNORE_SUM=$IGNORE
  IGNORE_LEN=$IGNORE
}

# Process command line options.
for OPTS
do
  case $1 in
    -e|--extra) EXTRA=true
                ;;
   -i|--ignore) IGNORE=true
                ;;
  -v|--verbose) VERBOSE=true
                SILENT=false
                ;;
   -s|--silent) SILENT=true
                VERBOSE=false
                ;;
     -h|--help) help
                exit 0
                ;;
          --|-) break
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

# Allow for log file parameter.
LOG="${1:-mocp_server_log}"
[[ "$LOG" = "-" ]] && LOG=/dev/fd/0

# Output formatting.
$SILENT || echo

# Process server log file.
while read
do

  # Reject log file if circular logging has been used.
  [[ "$REPLY" =~ "Circular Log Starts" ]] && \
      die MD5 sums cannot be checked when circular logging was used

  # Extract MOC revision header.
  [[ "$REPLY" =~ "This is Music On Console" ]] && \
      REVN="$(echo "$REPLY" | sed 's/^.*Music/Music/')"

  # Check for Tremor decoder.
  [[ "$REPLY" =~ Loaded\ [0-9]+\ decoders:.*vorbis\(tremor\) ]] && \
     TREMOR=true

  # Extract file's full pathname.
  [[ "$REPLY" =~ "Playing item" ]] && \
     FILE="$(echo "$REPLY" | sed 's/^.* item [0-9]*: \(.*\)$/\1/')"

  # Ignore all non-MD5 lines.
  [[ "$REPLY" =~ "MD5" ]] || continue

  # Extract fields of interest.
  NAME="$(echo "$REPLY" | sed 's/^.*MD5(\([^)]*\)) = .*$/\1/')"
  REST="$(echo "$REPLY" | sed 's/^.*MD5([^)]*) = \(.*\)$/\1/')"
  SUM=$(echo $REST | cut -f1 -d' ')
  LEN=$(echo $REST | cut -f2 -d' ')
  DEC=$(echo $REST | cut -f3 -d' ')
  $TREMOR && [[ "$DEC" = "vorbis" ]] && DEC=tremor
  FMT=$(echo $REST | cut -f4 -d' ')
  CHANS=$(echo $REST | cut -f5 -d' ')
  RATE=$(echo $REST | cut -f6 -d' ')

  # Check that we have the full pathname and it's not a dangling symlink.
  [[ "$NAME" = "$(basename "$FILE")" ]] || die Filename mismatch
  [[ -L "$FILE" && ! -f "$FILE" ]] && continue

  # Get the independant MD5 sum and length of audio file.
  case $DEC in
  aac|ffmpeg|flac|mp3|sndfile|speex|vorbis)
      IGNORE_LEN=false
      IGNORE_SUM=false
      $DEC
      SUM2=$(echo "$SUM2" | cut -f1 -d' ')
      ;;
  modplug|musepack|sidplay2|timidity|tremor|wavpack)
      $IGNORE && continue
      [[ "${UNSUPPORTED[$DEC]}" ]] || {
        echo -e "*** Decoder not yet supported: $DEC\n" > /dev/stderr
        UNSUPPORTED[$DEC]="Y"
      }
      continue
      ;;
  *)  [[ "${UNKNOWN[$DEC]}" ]] || {
        echo -e "*** Unknown decoder: $DEC\n" > /dev/stderr
        UNKNOWN[$DEC]="Y"
      }
      continue
      ;;
  esac

  # Compare results.
  BADFMT=false
  $EXTRA && [[ "${NAME:0:9}" = "sinewave-" ]] && {
    FMT2=$(echo $NAME | cut -f2 -d'-' | sed "s/24/32/")
    CHANS2=$(echo $NAME | cut -f3 -d'-')
    RATE2=$(echo $NAME | cut -f4 -d'-' | cut -f1 -d'.')
    [[ "$FMT" = "$FMT2" ]] || BADFMT=true
    [[ "$CHANS" = "$CHANS2" ]] || BADFMT=true
    [[ "$RATE" = "$RATE2" ]] || BADFMT=true
  }
  BADSUM=false; $IGNORE_SUM || [[ "$SUM" = "$SUM2" ]] || BADSUM=true
  BADLEN=false; $IGNORE_LEN || [[ "$LEN" = "$LEN2" ]] || BADLEN=true

  # Set exit code.
  $BADFMT || $BADSUM || $BADLEN && RC=2

  # Determine output requirements.
  $SILENT && continue
  $BADFMT || $BADSUM || $BADLEN || $VERBOSE || continue

  # Report result.
  [[ "$REVN" ]] && {
    echo "Test Results for $REVN"
    echo
    REVN=
  }
  echo "$NAME:"
  echo "    $SUM $LEN $DEC $FMT $CHANS $RATE"
  echo "    $SUM2 $LEN2"
  $BADFMT && echo "*** Format mismatch"
  $BADSUM && echo "*** MD5 sum mismatch"
  $BADLEN && echo "*** Length mismatch"
  echo

done < $LOG

$SILENT || {
  case "$RV" in
  1)
    echo "No mismatches found"
    ;;
  2)
    echo "NOTE: This tool is still being refined.  Do not accept mismatches"
    echo "      at face value; they may be due to factors such as sample size"
    echo "      differences.  But it does provide a reason to investigate"
    echo "      such mismatches further (and further refine this tool if false)."
    echo
    ;;
  esac
}

exit $RC
