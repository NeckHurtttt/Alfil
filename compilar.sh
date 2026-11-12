#!/bin/sh
# Compila el motor a WebAssembly sin Emscripten: solo clang y wasm-ld.
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O3 -nostdlib -fno-exceptions -fno-rtti -DALFIL_WASM \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined -Wl,--initial-memory=16777216 \
  -o web/alfil.wasm motor/alfil.cpp
echo "web/alfil.wasm: $(wc -c < web/alfil.wasm) bytes"
