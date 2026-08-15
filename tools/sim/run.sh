#!/bin/sh
# Build and run the simulated-owner harness against the real models.
set -e

here=$(cd "$(dirname "$0")" && pwd)
main="$here/../../main"

cp "$main/model.c" "$here/model_host.c"
trap 'rm -f "$here/model_host.c"' EXIT

cc -O1 -std=c11 -Wall -I "$main" \
    "$here/sim_owner.c" "$here/model_host.c" -lm -o "$here/sim_owner"

"$here/sim_owner"
