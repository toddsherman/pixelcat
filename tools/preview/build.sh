#!/bin/sh
# Build the host preview and render every state/pose to out/.
set -e

here=$(cd "$(dirname "$0")" && pwd)
main="$here/../../main"
out="$here/out"

mkdir -p "$out"
cp "$main/cat.c" "$here/cat_host.c"
cp "$main/cat_sprite.c" "$here/cat_sprite_host.c"
trap 'rm -f "$here/cat_host.c" "$here/cat_sprite_host.c"' EXIT

cc -O1 -std=c11 -Wall -I "$here" -I "$main" \
    "$here/preview.c" "$here/cat_host.c" "$here/cat_sprite_host.c" "$main/cat_bg.c" -lm -o "$out/preview"

# 0-3: behaviour states. 10+: forced modes (sprite cat: portrait, profile,
# clean_paw, clean_ear, trot, leap, sleep, pawing, big_jump, angry, pet,
# eat, play_paw). 50/51: bowl, play session. 53: the menu.
for st in 0 1 2 3 10 11 12 13 14 15 16 17 18 19 20 21 22 50 51 53 54 55 56 57 58 60 61; do
    "$out/preview" "$st" 2 > "$out/state$st.ppm"
    if command -v sips >/dev/null 2>&1; then
        sips -s format png "$out/state$st.ppm" --out "$out/state$st.png" >/dev/null
        rm "$out/state$st.ppm"
    fi
done
echo "$out"
