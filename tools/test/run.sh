#!/bin/sh
# Build and run the host-side gameplay tests against the real engine.
set -e

here=$(cd "$(dirname "$0")" && pwd)
main="$here/../../main"
preview="$here/../preview"

cp "$main/cat.c" "$here/cat_host.c"
cp "$main/cat_sprite.c" "$here/cat_sprite_host.c"
cp "$main/stats.c" "$here/stats_host.c"
trap 'rm -f "$here/cat_host.c" "$here/cat_sprite_host.c" "$here/stats_host.c"' EXIT

cc -O1 -std=c11 -Wall -I "$preview" -I "$main" \
    "$here/game_test.c" "$here/cat_host.c" "$here/cat_sprite_host.c" \
    "$here/stats_host.c" "$main/cat_bg.c" -lm -o "$here/game_test"

"$here/game_test"
