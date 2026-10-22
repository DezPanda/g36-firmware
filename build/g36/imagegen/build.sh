#!/bin/sh
# Builds g36-imagegen (and ig-cli) for the handheld, from llama.cpp's ggml objects
# (built for the chatbot) and the menu's drawing code. Usage: sh build.sh BUILD_DIR
# BUILD_DIR has g36-env.sh, src/llama.cpp-b11177 and out/llama/ggml_src_*.o.
set -e
B="$1"   # (objects named menu_* so the chatbot build leaves them out)
. "$B/g36-env.sh"
# (through a link without spaces: the compiler flags can't hold "G36 Files")
ln -sfn "$(cd "$(dirname "$0")/.." && pwd)" "$B/g36src"
HERE="$B/g36src/imagegen"
M="$B/g36src/menu"
L="$B/src/llama.cpp-b11177"
O="$B/out/llama"
ARCH="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mfp16-format=ieee"
INC="-I$O/gen -I$L/ggml/include -I$L/ggml/src -I$M -I$HERE"
$CC $CFLAGS -D_FILE_OFFSET_BITS=64 -std=gnu11 -O2 -c "$M/gfx.c" -o "$O/menu_ig_gfx.o"
$CC $CFLAGS -D_FILE_OFFSET_BITS=64 -std=gnu11 -O2 -c "$M/input.c" -o "$O/menu_ig_input.o"
$CXX $ARCH -DGGML_USE_CPU -DNDEBUG $INC $CFLAGS -O2 -std=gnu++17 -c "$HERE/ig.cpp" -o "$O/menu_ig_engine.o"
$CXX $ARCH $INC $CFLAGS -O2 -std=gnu++17 -o "$O/g36-imagegen" "$HERE/g36-imagegen.cpp" "$O/menu_ig_engine.o" \
   "$B/g36src/chat/futex_shim.cpp" "$O/menu_ig_gfx.o" "$O/menu_ig_input.o" "$O"/ggml_src_*.o \
   $LDFLAGS -lrga -ldrm -lfreetype -static-libstdc++ -static-libgcc -lpthread -lm -ldl
$STRIP "$O/g36-imagegen"
$CXX $ARCH $INC -O2 -std=gnu++17 -o "$O/ig-cli" "$HERE/ig-cli.cpp" "$O/menu_ig_engine.o" "$B/g36src/chat/futex_shim.cpp" \
   "$O"/ggml_src_*.o -static-libstdc++ -static-libgcc -lpthread -lm -ldl
$STRIP "$O/ig-cli"
ls -l "$O/g36-imagegen" "$O/ig-cli"
