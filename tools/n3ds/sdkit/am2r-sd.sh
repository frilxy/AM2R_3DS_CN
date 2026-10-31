#!/usr/bin/env bash
# AM2R 3DS SD card builder: pick your AM2R data.win and a folder; this writes <folder>/3ds/am2r/ (the game's files,
# the 3DS textures and audio, and the 3DS control config), ready to copy to the root of the SD card.
# Linux, or Windows through WSL (am2r-sd.bat). What it needs (n3ds-preprocess, tex3ds, the SD data revision, the
# 3DS control config) is downloaded next to it from the latest release the first time; delete bin/ to update them.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
bin="$here/bin"
title="AM2R 3DS - SD card builder"
repo="${AM2R_REPO:-pegacha/Butterscotch}"
# Where the tools and am2r.cia come from: the release this script came with (a Codeberg release's copy has its own).
release_page="https://github.com/$repo/releases/latest"
release="${AM2R_RELEASE_URL:-$release_page/download}"

have_whiptail() { command -v whiptail >/dev/null 2>&1; }
in_wsl() { grep -qi microsoft /proc/version 2>/dev/null; }

msg() {
    if have_whiptail; then whiptail --title "$title" --msgbox "$1" 0 0; else printf '\n%s\n\nPress Enter. ' "$1"; read -r _; fi
}

ask_yes() {
    if have_whiptail; then whiptail --title "$title" --yesno "$1" 0 0; return; fi
    printf '\n%s [y/N] ' "$1"; read -r a; [[ "$a" == [yY]* ]]
}

# A path typed or pasted by the user; Windows paths (C:\...) become /mnt/c/... in WSL.
normalize_path() {
    local p="${1%\"}"; p="${p#\"}"
    if in_wsl && [[ "$p" =~ ^[A-Za-z]:[\\/] ]] && command -v wslpath >/dev/null 2>&1; then p="$(wslpath -u "$p")"; fi
    printf '%s' "$p"
}

ask_path() {
    local prompt="$1" answer
    if have_whiptail; then
        answer=$(whiptail --title "$title" --inputbox "$prompt" 10 76 3>&1 1>&2 2>&3) || return 1
    else
        printf '\n%s\n> ' "$prompt"; read -r answer
    fi
    normalize_path "$answer"
}

start_dir() {
    if in_wsl; then
        local profile
        profile=$(cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d '\r')
        if [[ -n "$profile" ]] && command -v wslpath >/dev/null 2>&1; then
            profile="$(wslpath -u "$profile" 2>/dev/null)"
            [[ -d "$profile" ]] && { printf '%s' "$profile"; return; }
        fi
    fi
    printf '%s' "$HOME"
}

# browse <mode> <start dir>: mode "file" picks a data.win (or game.unx), mode "dir" picks a folder.
# Prints the chosen path; returns 1 if cancelled.
browse() {
    local mode="$1" dir="$2"
    if ! have_whiptail; then
        if [[ "$mode" == file ]]; then ask_path "Path of your AM2R data.win:"; else ask_path "Folder to write the SD card files to:"; fi
        return
    fi
    while true; do
        local items=() entry name
        if [[ "$mode" == dir ]]; then items+=("[ use this folder ]" ""); fi
        items+=("[ type a path ]" "")
        [[ "$dir" != "/" ]] && items+=(".." "up")
        while IFS= read -r -d '' entry; do
            name="${entry##*/}"
            [[ "$name" == .* ]] && continue
            if [[ -d "$entry" ]]; then items+=("$name/" "")
            elif [[ "$mode" == file && ( "${name,,}" == *.win || "${name,,}" == game.unx ) ]]; then items+=("$name" "game data")
            fi
        done < <(find "$dir" -mindepth 1 -maxdepth 1 \( -type d -o -type f \) -print0 2>/dev/null | sort -z -f)
        local heading="Pick your AM2R data.win"
        [[ "$mode" == dir ]] && heading="Pick the folder to write the SD card files to"
        local choice
        choice=$(whiptail --title "$title" --menu "$heading\n$dir" 22 76 12 "${items[@]}" 3>&1 1>&2 2>&3) || return 1
        case "$choice" in
            "[ use this folder ]") printf '%s' "$dir"; return 0 ;;
            "[ type a path ]")
                local typed
                typed=$(ask_path "Path:") || continue
                if [[ -d "$typed" ]]; then dir="$typed"
                elif [[ "$mode" == file && -f "$typed" ]]; then printf '%s' "$typed"; return 0
                else msg "Not found: $typed"; fi ;;
            "..") dir="$(dirname "$dir")" ;;
            */) dir="${dir%/}/${choice%/}" ;;
            *) printf '%s' "${dir%/}/$choice"; return 0 ;;
        esac
    done
}

download() { # download <url> <file>
    if command -v curl >/dev/null 2>&1; then curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then wget -O "$2" "$1"
    else echo "Neither curl nor wget is installed (sudo apt install curl)." >&2; return 1; fi
}

ensure_tools() {
    if [[ -f "$bin/n3ds-preprocess" && -f "$bin/tex3ds" && -f "$here/sd_data_rev.txt" && -f "$here/seed/config.ini" ]]; then
        chmod +x "$bin/n3ds-preprocess" "$bin/tex3ds" 2>/dev/null
        return 0
    fi
    echo "== Downloading the tools from $release"
    local archive="$here/am2r-sd-tools.tar.gz"
    if ! download "$release/am2r-sd-tools.tar.gz" "$archive" || ! tar -xzf "$archive" -C "$here"; then
        rm -f "$archive"
        msg "Couldn't download the tools from $release/am2r-sd-tools.tar.gz. Check the internet connection and try again."
        exit 1
    fi
    rm -f "$archive"
    chmod +x "$bin/n3ds-preprocess" "$bin/tex3ds" 2>/dev/null
}

main() {
    ensure_tools
    msg "This builds the AM2R files for the 3DS's SD card.

You need your own copy of AM2R, updated with the AM2R Launcher. Pick its data.win, then a folder: the files go in <folder>/3ds/am2r/. Copy that 3ds folder to the root of the SD card."

    local datawin
    datawin=$(browse file "$(start_dir)") || exit 0
    [[ -f "$datawin" ]] || { msg "Not a file: $datawin"; exit 1; }
    local game; game="$(dirname "$datawin")"

    local outroot
    outroot=$(browse dir "$(dirname "$game")") || exit 0
    mkdir -p "$outroot" 2>/dev/null || { msg "Can't create $outroot"; exit 1; }
    local out="${outroot%/}/3ds/am2r"

    ask_yes "Game data: $datawin
Output: $out

This takes a few minutes. Go?" || exit 0

    clear 2>/dev/null
    echo "== Copying the game's files to $out"
    mkdir -p "$out"
    # The game's own files (lang/, ...), not the Windows program or the audio (converted below).
    (cd "$game" && find . -type f ! -iname '*.exe' ! -iname '*.dll' ! -iname '*.ogg' ! -iname '*.wav' ! -iname '*.mp3' \
        ! -path './gfx/*' ! -path './audio/*' -print0 | while IFS= read -r -d '' f; do
            mkdir -p "$out/$(dirname "$f")" && cp -f "$f" "$out/$f"
        done)
    # Only Chinese is translated here, and the language list on the card is ours (lang/languages.txt), so the other
    # languages' title logos and area banners can never be loaded: drop them, they are ~3 MB of the card.
    for dir in "$out/lang/headers" "$out/lang/titles"; do
        [ -d "$dir" ] || continue
        find "$dir" -maxdepth 1 -type f -name '*.png' -print0 | while IFS= read -r -d '' f; do
            case "$f" in *简体中文*) ;; *) rm -f "$f" ;; esac
        done
    done

    # The 3DS program loads data.win (a Linux build's game.unx is the same file).
    [[ "$(basename "$datawin")" != "data.win" ]] && cp -f "$datawin" "$out/data.win"

    echo "== Converting textures and audio (n3ds-preprocess)"
    if ! "$bin/n3ds-preprocess" "$datawin" "$out" --tex3ds "$bin/tex3ds"; then
        msg "The conversion failed; the messages above say why. Nothing on the SD card is touched until you copy the folder."
        exit 1
    fi
    cp -f "$here/sd_data_rev.txt" "$out/sd_data_rev.txt"
    # This port's own language assets, which REPLACE the game's copies (applied after them on purpose): the game ships
    # outline fonts, the port draws from the pixel fonts' 1-bit strikes and reads the baked glyph pack next to them.
    if [[ -d "$here/overlay" ]]; then cp -r "$here/overlay/." "$out/"; fi
    # The 3DS control layout, only where there is no config yet.
    [[ -f "$out/config.ini" ]] || cp "$here/seed/config.ini" "$out/config.ini"

    echo "== Downloading the 3DS program (am2r.cia)"
    local cia="${outroot%/}/am2r.cia" ciamsg
    if download "$release/am2r.cia" "$cia"; then
        ciamsg="am2r.cia is in $outroot too: copy it to the SD card and install it with FBI (or scan the QR code on the release page)."
    else
        rm -f "$cia"
        ciamsg="Install am2r.cia with FBI from the release page: $release_page"
    fi

    local size; size=$(du -sh "$out" 2>/dev/null | cut -f1)
    msg "Done ($size): $out

Copy the 3ds folder in $outroot to the root of the SD card (merge it with the one there).
$ciamsg"
}

main "$@"
