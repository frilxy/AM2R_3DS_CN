#!/usr/bin/env bash
# Builds the SD-card page (web/) into one self-contained HTML file:
#   tools/web/build.sh [out dir, default build-web]   ->   <out dir>/index.html
# Needs Emscripten (emcc on PATH) and Node. xdelta3 and liblzma are fetched at pinned versions.
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mkdir -p "${1:-$src/build-web}" && cd "${1:-$src/build-web}" && pwd)"
deps="$out/deps"
mkdir -p "$deps"

fetch() { # fetch <dir> <git url> <tag>
    [ -d "$deps/$1" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "$deps/$1"
}
fetch xdelta https://github.com/jmacd/xdelta.git v3.1.0
fetch xz https://github.com/tukaani-project/xz.git v5.6.3

# liblzma: xdelta3's secondary compressor (the AM2R patch uses it). Decoder only.
emcmake cmake -S "$deps/xz" -B "$out/xz" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DXZ_THREADS=no -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_SANDBOX=no -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF \
    -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF >/dev/null
cmake --build "$out/xz" --target liblzma >/dev/null

xd3="$deps/xdelta/xdelta3"
xd3_defs=(-DSECONDARY_LZMA=1 -DSECONDARY_DJW=1 -DSECONDARY_FGK=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0
    -DSHELL_TESTS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8
    -DXD3_USE_LARGEFILE64=1 -DLZMA_API_STATIC)
emcc -O3 -w "${xd3_defs[@]}" -I"$xd3" -I"$deps/xz/src/liblzma/api" \
    "$xd3/xdelta3.c" "$src/tools/web/xdelta_wasm.c" "$out/xz/liblzma.a" -o "$out/xdelta3.js" \
    -sMODULARIZE=1 -sEXPORT_NAME=createXdelta -sSINGLE_FILE=1 -sENVIRONMENT=worker,node -sWASM_BIGINT=1 \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
    -sEXPORTED_FUNCTIONS=_xd3wasmDecode,_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,getValue

emcmake cmake -S "$src/tools/n3ds-preprocess" -B "$out/preprocess" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$out/preprocess" >/dev/null
cp "$out/preprocess/n3ds-preprocess.js" "$out/"

node "$src/tools/web/bundle.mjs" "$out" "$out/index.html"
