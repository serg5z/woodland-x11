#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build/woodland_x11_playground}
mkdir -p "$(dirname "$out")"
cc -std=c99 -O2 -Wall -Wextra -Werror \
  -o "$out" tools/client_matrix/woodland_playground.c \
  $(pkg-config --cflags --libs x11)
printf 'Built %s\n' "$out"
