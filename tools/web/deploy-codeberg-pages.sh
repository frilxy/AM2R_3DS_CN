#!/usr/bin/env bash
# Publishes the SD card maker on Codeberg Pages: builds build-web/index.html and pushes it as the only file of the
# `pages` branch of a Codeberg remote (default: `codeberg`), which Codeberg serves at
# https://<owner>.codeberg.page/<repo>/ (here https://nattensmadrigal.codeberg.page/AM2R_3DS/).
#   tools/web/deploy-codeberg-pages.sh [remote]
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
remote="${1:-codeberg}"
"$src/tools/web/build.sh" "$src/build-web"

site="$(mktemp -d)"
trap 'rm -rf "$site"' EXIT
cp "$src/build-web/index.html" "$site/index.html"
# Codeberg Pages serves the branch as is: no Jekyll or headers needed.
git -C "$site" init -q -b pages
git -C "$site" add index.html
git -C "$site" -c user.name="$(git -C "$src" config user.name)" -c user.email="$(git -C "$src" config user.email)" \
    commit -q -m "SD card maker from $(git -C "$src" rev-parse --short HEAD)"
git -C "$site" push -f "$(git -C "$src" remote get-url "$remote")" pages:pages
