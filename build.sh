#!/bin/sh
set -eu

if ! command -v pkg-config >/dev/null 2>&1 ||
   ! pkg-config --exists alsa; then
  echo "ALSA development files are required (pkg-config package: alsa)." >&2
  exit 1
fi

${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic \
  lp.c midi_linux.c -o lp \
  $(pkg-config --cflags --libs alsa) -pthread
