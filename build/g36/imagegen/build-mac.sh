#!/bin/sh
# Builds ig-cli for this Mac (to test the engine and pack weights), from llama.cpp's
# ggml. Usage: sh build-mac.sh LLAMA_CPP_SOURCE OUT_DIR
set -e
L="$1"
O="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$O/obj" "$O/gen"
printf '#pragma once\n#define GGML_VERSION "b11177"\n#define GGML_COMMIT "b11177"\n' > "$O/gen/ggml-version.h"
DEFS="-DGGML_USE_CPU -DNDEBUG"
INC="-I$O/gen -I$L/ggml/include -I$L/ggml/src -I$L/ggml/src/ggml-cpu"
for f in $L/ggml/src/*.c $L/ggml/src/*.cpp $L/ggml/src/ggml-cpu/*.c $L/ggml/src/ggml-cpu/*.cpp \
      $L/ggml/src/ggml-cpu/arch/arm/*.c $L/ggml/src/ggml-cpu/arch/arm/*.cpp; do
   case "$f" in *hbm.cpp) continue ;; esac
   o="$O/obj/$(echo "$f" | sed "s|$L/||; s|/|_|g").o"
   [ "$o" -nt "$f" ] && continue
   case "$f" in
      *.c) clang -O3 -mcpu=apple-m1 $DEFS $INC -std=gnu11 -c "$f" -o "$o" ;;
      *) clang++ -O3 -mcpu=apple-m1 $DEFS $INC -std=gnu++17 -c "$f" -o "$o" ;;
   esac &
   while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.2; done
done
wait
clang++ -O3 -mcpu=apple-m1 $DEFS $INC -std=gnu++17 -o "$O/ig-cli" "$HERE/ig-cli.cpp" "$HERE/ig.cpp" "$O"/obj/*.o
ls -l "$O/ig-cli"
