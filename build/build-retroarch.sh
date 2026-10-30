#!/bin/bash
# configure + build the G36 RetroArch (same flags as the original build)
set -e
. "$(dirname "$0")/g36-env.sh"
cd "$(dirname "$0")/RetroArch-1.22.2"
if [ ! -f config.mk ]; then
./configure --enable-odroidgo2 --enable-neon --enable-floathard \
 --enable-alsa --enable-udev --enable-freetype --enable-rgui --enable-builtinzlib \
 --disable-opengl --disable-opengl1 --disable-opengl_core --disable-opengles --disable-egl --disable-kms --disable-vulkan \
 --disable-x11 --disable-wayland --disable-sdl --disable-sdl2 --disable-ffmpeg --disable-qt --disable-xmb --disable-ozone --disable-materialui \
 --disable-pulse --disable-jack --disable-oss --disable-pipewire --disable-roar --disable-rsound --disable-tinyalsa --disable-audioio \
 --disable-networking --disable-cheevos --disable-discord --disable-online_updater --disable-update_cores --disable-update_core_info --disable-update_assets \
 --disable-ssl --disable-libusb --disable-dbus --disable-systemd --disable-v4l2 --disable-caca --disable-sixel --disable-cg --disable-glsl --disable-slang --disable-glslang --disable-spirv_cross \
 --disable-crtswitchres --disable-videocore --disable-videoprocessor --disable-cdrom --disable-parport --disable-microphone --disable-translate --disable-accessibility --disable-gfx_widgets \
 2>&1 | tail -15
fi
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -40
ls -l retroarch
