# Cross-build environment for RetroArch on the G36 (RK3506B, armv7 hard-float, glibc 2.38 device).
# Usage: . ./g36-env.sh   (from the build dir, via a path without spaces)
B=${G36_BUILD:?set G36_BUILD to the full path of the build/ folder (a path without spaces)}
TC=$B/toolchain/armv7-unknown-linux-gnueabihf/bin
export PATH="$B/fakebin:$TC:$PATH"
export CROSS_COMPILE=armv7-unknown-linux-gnueabihf-
export CC=${CROSS_COMPILE}gcc CXX=${CROSS_COMPILE}g++ AR=${CROSS_COMPILE}ar STRIP=${CROSS_COMPILE}strip
export CFLAGS="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 -I$B/sysinc -I$B/sysinc/drm -I$B/sysinc/freetype2"
export CXXFLAGS="$CFLAGS"
# Link other libraries against the copies pulled from the handheld; libc comes from the
# toolchain (glibc 2.28) so the binary only needs symbols the device's glibc 2.38 provides.
export LDFLAGS="-L$B/device-sysroot/usr/lib -Wl,-rpath-link,$B/device-sysroot/usr/lib -Wl,--allow-shlib-undefined"
