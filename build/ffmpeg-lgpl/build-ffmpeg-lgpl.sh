set -e
B=${G36_BUILD:?set G36_BUILD to the full path of the build/ folder (a path without spaces)}
FFW=${G36_FFWORK:-/tmp/g36-ffmpeg}   # where FFmpeg is unpacked and built
. $B/g36-env.sh
cd $FFW
make distclean >/dev/null 2>&1 || true
ZINC=$B/src/RetroArch-1.22.2/libretro-common/include/compat/zlib
./configure --prefix=/usr --enable-cross-compile --cross-prefix=${CROSS_COMPILE} --arch=arm --cpu=cortex-a7 --target-os=linux \
 --enable-shared --disable-static --enable-pic \
 --disable-gpl --disable-nonfree --disable-version3 --disable-autodetect \
 --disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-avresample --disable-network \
 --disable-debug --disable-stripping --enable-neon --enable-vfp --enable-zlib --disable-armv5te \
 --extra-cflags="-O3 -mfpu=neon-vfpv4 -mfloat-abi=hard -I$ZINC" --extra-ldflags="-L$B/device-sysroot/usr/lib -Wl,-rpath-link,$B/device-sysroot/usr/lib" > $FFW-configure.log 2>&1
tail -1 $FFW-configure.log
make -j6 > $FFW-make.log 2>&1
echo MAKE-EXIT=$?
