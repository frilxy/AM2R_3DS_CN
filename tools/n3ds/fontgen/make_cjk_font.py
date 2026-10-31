#!/usr/bin/env python3
"""Bakes the CJK glyphs AM2R needs into one bitmap atlas plus a metrics file.

Run offline (needs Pillow); the 3DS only loads the result. Nothing here touches the game's rendering: the port reads
atlas.png + metrics.json at boot and points the fonts' glyphs at that one texture, which keeps the diff against
upstream tiny and makes re-adapting to a new upstream a matter of re-running this script.

    python3 make_cjk_font.py <lang.ini> <fonts dir> <out dir>

Outputs: <out>/atlas.png, <out>/metrics.json
"""
import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageFont

# font file -> pixel size used by the game (see res/n3ds/am2r/overlay/lang/fonts/sizes.ini)
FONTS = {
    "uni_05_53.ttf": 10,
    "04b09.ttf": 12,
    "04b24.ttf": 10,
    "Glasstown_NBP.ttf": 12,
    "Acknowledge_TT_BRK.ttf": 12,
}

ATLAS_W = 1024


def wanted_codepoints(text):
    """Every character the game can draw: the language file plus printable ASCII."""
    chars = set(text)
    chars |= set(chr(c) for c in range(0x20, 0x7F))
    chars |= set("“”‘’—…×÷°％·、。，！？：；（）《》【】")
    return sorted(c for c in chars if c not in "\r\n\t")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    lang_file, font_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)

    text = lang_file.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"<[^>]+>", "", text)  # drop inline markup
    codepoints = wanted_codepoints(text)

    rows = []      # (font, size, char, bitmap, metrics)
    for name, size in FONTS.items():
        path = font_dir / name
        if not path.exists():
            print(f"missing font: {path}")
            continue
        font = ImageFont.truetype(str(path), size)
        for ch in codepoints:
            try:
                bbox = font.getbbox(ch)
            except Exception:
                continue
            if bbox is None:
                continue
            w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
            if w <= 0 or h <= 0:
                continue
            img = Image.new("L", (w + 2, h + 2), 0)
            ImageDraw = __import__("PIL.ImageDraw", fromlist=["Draw"]).Draw(img)
            ImageDraw.text((1 - bbox[0], 1 - bbox[1]), ch, font=font, fill=255)
            rows.append((name, size, ch, img, font.getlength(ch)))

    # One atlas per font, spilling into extra pages when a font needs more room.
    outputs = []
    for name, size in FONTS.items():
        glyphs = [r for r in rows if r[0] == name]
        if not glyphs:
            continue
        page = 0
        atlas = Image.new("L", (ATLAS_W, ATLAS_W), 0)
        x = y = shelf_h = 0
        metrics = []
        for _, _, ch, img, advance in glyphs:
            if x + img.width > ATLAS_W:
                x, y, shelf_h = 0, y + shelf_h, 0
            if y + img.height > ATLAS_W:
                atlas.save(out_dir / f"atlas-{Path(name).stem}-{page}.png")
                outputs.append((f"atlas-{Path(name).stem}-{page}.png", len(metrics)))
                page += 1
                atlas = Image.new("L", (ATLAS_W, ATLAS_W), 0)
                metrics = []
                x = y = shelf_h = 0
            atlas.paste(img, (x, y))
            metrics.append({
                "size": size, "codepoint": ord(ch),
                "page": page, "x": x, "y": y, "w": img.width, "h": img.height,
                "advance": round(advance, 2),
            })
            x += img.width
            shelf_h = max(shelf_h, img.height)
        atlas.save(out_dir / f"atlas-{Path(name).stem}-{page}.png")
        outputs.append((f"atlas-{Path(name).stem}-{page}.png", len(metrics)))
        (out_dir / f"metrics-{Path(name).stem}.json").write_text(
            json.dumps({"atlas": ATLAS_W, "font": name, "glyphs": metrics}, ensure_ascii=False, indent=0),
            encoding="utf-8")

    for fname, count in outputs:
        print(f"{fname}: {count} glyphs")
    print(f"{len(rows)} glyphs total from {len(FONTS)} fonts")


if __name__ == "__main__":
    main()
