#!/usr/bin/env bash
# Builds the game folder for the SD card (sdmc:/3ds/<folder>/) from a game's install directory:
#   tools/n3ds/make_sd.sh <game dir with data.win> <out dir, e.g. sd/3ds/am2r>
# It holds the game's own files (data.win, lang/, music, ...; not the Windows .exe/.dll), the textures converted by
# n3ds-preprocess (gfx/, audio/) and sd_data_rev.txt. It only needs redoing when tools/n3ds/sd_data_rev.txt goes up.
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
game="$1"
out="$2"
pre="${BS_PREPROCESS_BUILD:-$src/../build-preprocess}"

if [ ! -x "$pre/n3ds-preprocess" ]; then
    cmake -S "$src/tools/n3ds-preprocess" -B "$pre" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$pre"
fi

mkdir -p "$out"
# Audio files are converted to audio/ (bank + streamed BCWAV), so the originals stay off the card.
rsync -a --exclude '*.exe' --exclude '*.dll' --exclude '*.ogg' --exclude '*.wav' --exclude '*.mp3' --exclude 'gfx/' --exclude 'audio/' "$game"/ "$out"/
# rsync -a also copies the game folder's own permissions, and an install directory that is not readable/writable by
# its owner (mode 000 from a NAS, an unzipped release, a copied disc) makes the output unusable and blocks the writes
# below. The owner always needs the folder.
chmod -R u+rwX "$out" 2>/dev/null || true
"$pre/n3ds-preprocess" "$game/data.win" "$out"
cp "$src/tools/n3ds/sd_data_rev.txt" "$out/sd_data_rev.txt"
# Default files for this game (e.g. AM2R's 3DS control config), only where the card has none. cp's --update=none
# needs coreutils 9.2+ and fails outright on 9.1 (Ubuntu 24.04, the build image), so each file is checked instead;
# the seed is only a handful of small files.
seed="$src/res/n3ds/$(basename "$out")/sd"
if [ -d "$seed" ]; then
    while IFS= read -r -d '' file; do
        if [ ! -e "$out/${file#./}" ]; then
            mkdir -p "$out/$(dirname "${file#./}")"
            cp "$seed/${file#./}" "$out/${file#./}"
        fi
    done < <(cd "$seed" && find . -type f -print0)
fi

# This port's own language assets, which REPLACE the game's copies: the language files that were shipped with AM2R
# are built for outline fonts (the five .ttf files are a 16 MB CJK face with 12-16px strikes), and the port's
# font_replace reads the 1-bit bitmap strikes the layout was designed around. Without this the game's own files win
# and the sizes in sizes.ini do not match any strike. Applied after the game's files on purpose.
overlay="$src/res/n3ds/$(basename "$out")/overlay"
if [ -d "$overlay" ]; then
    cp -r "$overlay"/. "$out"/
fi
du -sh "$out"
