#!/bin/sh
# Builds goodies/*.c into goodies/*.wasm for /debug/goodies/ (clang + wasm-ld, any recent LLVM).
# MVP features only: the firmware's WAMR build has bulk memory, SIMD and reference types off.
# One 64 KB page of linear memory, 4 KB shadow stack, no libc.
set -e
cd "$(dirname "$0")"
for src in *.c; do
  clang --target=wasm32 -mcpu=mvp -Os -nostdlib -Wall -Wextra -Werror \
    -Wl,--no-entry -Wl,--strip-all -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
    -Wl,-z,stack-size=4096 -Wl,--global-base=4096 \
    -o "${src%.c}.wasm" "$src"
  echo "${src%.c}.wasm $(wc -c < "${src%.c}.wasm") B"
done
