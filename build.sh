#!/bin/sh
# Build d3d12.dll, dxgi.dll, and D3D12Core.dll. Needs Homebrew mingw-w64.
set -e
cd "$(dirname "$0")"
INCLUDE=$(dirname "$(echo '#include <d3d12.h>' | x86_64-w64-mingw32-gcc -E - | grep -m1 -o '"[^"]*d3d12.h"' | tr -d '"')")
python3 -I gen_slots.py "$INCLUDE"
FLAGS="-O2 -Wall -Wno-unused-function -Wno-unused-variable -shared -static-libgcc"
x86_64-w64-mingw32-gcc $FLAGS -o d3d12.dll d3d12shim.c d3d12.def -lole32
x86_64-w64-mingw32-gcc $FLAGS -o dxgi.dll dxgishim.c dxgi.def
x86_64-w64-mingw32-gcc $FLAGS -o D3D12Core.dll d3d12core.c d3d12core.def
ls -l d3d12.dll dxgi.dll D3D12Core.dll
