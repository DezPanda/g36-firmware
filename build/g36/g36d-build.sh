B=${G36_BUILD:?set G36_BUILD to the full path of the build/ folder (a path without spaces)}
. $B/g36-env.sh
mkdir -p $B/out
cd $B/g36
$CC $CFLAGS -D_FILE_OFFSET_BITS=64 -std=gnu11 -Wall -Wextra -o $B/out/g36d.new g36d.c $LDFLAGS -lasound && $STRIP $B/out/g36d.new && ls -l $B/out/g36d.new
