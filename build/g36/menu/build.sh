#!/bin/sh
# Build the G36 menu programs for the handheld. Usage: sh build.sh [target...]
set -e
B=${G36_BUILD:?set G36_BUILD to the full path of the build/ folder (a path without spaces)}
. "$B/g36-env.sh"
cd "$B/g36/menu"
LIBS="-lrga -ldrm -lfreetype -lasound -lm"
for t in "${@:-gfxtest}"; do
   # the menu carries the media player, which uses the handheld's own FFmpeg
   EXTRA= XLIBS=
   if [ "$t" = g36menu ]; then
      EXTRA="player.c -I$B/sysinc/ffmpeg"
      XLIBS="-lavfilter -lavformat -lavcodec -lavutil -lswresample -lswscale -lpthread"
   fi
   # 64-bit file sizes: on this 32-bit system a file over 2 GB (a long video) would
   # otherwise fail stat() and be left out of the lists
   $CC $CFLAGS -D_FILE_OFFSET_BITS=64 -std=gnu11 -Wall -Wextra -Wno-unused-function -o "$t" "$t.c" gfx.c input.c $EXTRA $LDFLAGS $LIBS $XLIBS
   $STRIP "$t"
   ls -l "$t"
done
