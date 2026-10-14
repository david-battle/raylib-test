#!/usr/bin/env bash
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO"

BIN=./tetris
SRC=tetris.c

# Rebuild if source newer or binary missing
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
  gcc -I ~/raylib/src -Wall -Wextra -O2 -o "$BIN" "$SRC" ~/raylib/src/libraylib.a -lm -lpthread -ldl -lX11
fi

# Run detached so it survives the shell/tool exit
nohup "$BIN" >/dev/null 2>&1 &
echo "tetris started (pid $!)"
