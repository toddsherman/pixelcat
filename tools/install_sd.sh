#!/bin/sh
# Copy the park (and anything else the firmware reads) onto the SD card.
#
# The world lives on the card, not in flash — that is what keeps the
# firmware at ~1 MB instead of 10 MB and leaves room for weather. Run this
# after tools/gen_world.py, with the card in a reader:
#
#   tools/install_sd.sh                 # finds the only mounted volume
#   tools/install_sd.sh /Volumes/CAT    # or name it
set -e

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../sd/pixelcat"

if [ ! -d "$src" ]; then
    echo "no $src — run: python3 tools/gen_world.py background main" >&2
    exit 1
fi

vol="$1"
if [ -z "$vol" ]; then
    # Exactly one removable volume: take it. More than one: make the user say.
    set -- /Volumes/*
    for v in "$@"; do
        [ "$v" = "/Volumes/Macintosh HD" ] && continue
        [ -w "$v" ] || continue
        if [ -n "$vol" ]; then
            echo "several volumes mounted; name one:" >&2
            ls -d /Volumes/* >&2
            exit 1
        fi
        vol="$v"
    done
fi

if [ -z "$vol" ] || [ ! -d "$vol" ]; then
    echo "no writable volume found — insert the card, or pass its path" >&2
    exit 1
fi

echo "installing to $vol/pixelcat"
mkdir -p "$vol/pixelcat"
cp -v "$src"/world_*.bin "$vol/pixelcat/"
sync
echo "done — eject the card and put it back in the board"
