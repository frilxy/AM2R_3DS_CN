#!/usr/bin/env bash
# Builds the 3DS program (<name>.cia and <name>.3dsx): tools/n3ds/build.sh [build-dir] [extra cmake args...]
# BS_N3DS_GAME picks the game profile (default am2r); BS_N3DS_GAME=generic builds a plain Butterscotch runner.
set -euo pipefail
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/../build-n3ds}"
shift || true

# Only the WAD versions a profile needs: fewer lets the compiler fold the interpreter's version checks, as upstream does
# for its console builds.
case "${BS_N3DS_GAME:-am2r}" in
    am2r)
        profile=(
            # WAD 14 is AM2R 1.1, WAD 15 (built into the WAD 16 support) is the Community Updates (1.5.x).
            -DENABLE_WAD14=ON -DENABLE_WAD16=ON -DENABLE_WAD17=OFF
            "-DN3DS_APP_NAME=AM2R"
            "-DN3DS_APP_DESCRIPTION=Another Metroid 2 Remake"
            "-DN3DS_APP_AUTHOR=DoctorM64"
            -DN3DS_OUTPUT_NAME=am2r
            -DN3DS_SD_FOLDER=am2r
            -DN3DS_UNIQUE_ID=0xA2E21
            -DN3DS_PRODUCT_CODE=CTR-P-AM2R
            # AM2R lays out its own 320x240 display (application_surface and HUD surface at its own offsets), so
            # the widescreen hack can't widen it: stretch it to the screen instead.
            -DN3DS_SCREEN_MODE=stretch
            # AM2R switches its button prompts to the keyboard's while any keyboard binding is held: gamepad only.
            -DN3DS_KEYBOARD_MIRROR=OFF
        )
        ;;
    *)
        profile=(-DENABLE_WAD14=ON -DENABLE_WAD16=ON -DENABLE_WAD17=ON)
        ;;
esac

cmake -S "$src" -B "$build" -G Ninja -DPLATFORM=n3ds -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_LEGACY_GL=OFF -DENABLE_MODERN_GL=OFF "${profile[@]}" "$@" >/dev/null
cmake --build "$build"
