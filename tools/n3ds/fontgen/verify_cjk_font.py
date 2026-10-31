#!/usr/bin/env python3
"""Checks cjk-font.bin the way the 3DS loader reads it, and draws the fonts' text from it.

This is the host-side proof for src/n3ds/n3ds_cjk_font.c: it parses the pack with the same offsets and rules, then
renders a string through the glyph rectangles (line top to baseline, ink at the bottom) so a wrong rectangle shows up
as text that floats or sinks.

    python3 verify_cjk_font.py [--pack FILE] [--text "中文 text"]

Exits non-zero on any inconsistency, so it can be run right after make_cjk_font.py.
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"CJKF"
STRIP_SIZE = 44
GLYPH_SIZE = 14


def read_pack(path):
    data = Path(path).read_bytes()
    magic, version, width, height, used_rows, strips, strips_at, bitmap_at, bitmap_bytes, _reserved = \
        struct.unpack("<4sIHHHHIIII", data[:32])
    assert magic == MAGIC, f"magic {magic!r}"
    assert version == 1, f"version {version}"
    assert width % 8 == 0 and width <= 1024 and width & (width - 1) == 0, f"width {width}"
    assert height <= 1024 and height & (height - 1) == 0, f"height {height}"
    assert 0 < used_rows <= height, f"usedRows {used_rows} of {height}"
    stride = width // 8
    assert bitmap_bytes >= stride * used_rows, "bitmap too small"
    assert bitmap_at + bitmap_bytes <= len(data), "bitmap outside the file"
    assert strips_at + strips * STRIP_SIZE <= len(data), "strip table outside the file"

    table = {}
    for i in range(strips):
        entry = data[strips_at + i * STRIP_SIZE: strips_at + (i + 1) * STRIP_SIZE]
        name = entry[:32].split(b"\0", 1)[0].decode("ascii")
        size, y_offset, max_height, glyph_count, glyph_at = struct.unpack("<HHHHI", entry[32:44])
        assert glyph_at + glyph_count * GLYPH_SIZE <= len(data), f"{name}: glyph table outside the file"
        glyphs = []
        for g in range(glyph_count):
            codepoint, x, y, w, h, shift, offset = struct.unpack(
                "<HHHHHhh", data[glyph_at + g * GLYPH_SIZE: glyph_at + (g + 1) * GLYPH_SIZE])
            assert 0 <= x and x + w <= width, f"{name}: glyph {codepoint:#x} x={x} w={w}"
            assert 0 <= y and y + h <= used_rows, f"{name}: glyph {codepoint:#x} y={y} h={h}"
            glyphs.append({"codepoint": codepoint, "x": x, "y": y, "w": w, "h": h, "shift": shift, "offset": offset})
        table[(name, size)] = {"yOffset": y_offset, "maxGlyphHeight": max_height, "glyphs": glyphs,
                               "bitmap": data[bitmap_at: bitmap_at + bitmap_bytes], "stride": stride,
                               "width": width}

    # A strip's glyphs must all live inside the rows its yOffset claims, or a font built from it would draw another
    # font's rows.
    for (name, size), strip in table.items():
        if strip["glyphs"]:
            lowest = max(g["y"] + g["h"] for g in strip["glyphs"])
            assert lowest <= used_rows, f"{name}@{size}: glyphs reach row {lowest}"
    return table


def pixels(strip, glyph):
    """The glyph's ink, one row of 0/1 per line, read the way the 3DS samples the page."""
    rows = []
    for y in range(glyph["h"]):
        page_row = glyph["y"] + y
        line = []
        for x in range(glyph["w"]):
            page_x = glyph["x"] + x
            if page_x >= strip["width"]:
                line.append(0)
                continue
            byte = strip["bitmap"][page_row * strip["stride"] + (page_x >> 3)]
            line.append((byte >> (7 - (page_x & 7))) & 1)
        rows.append(line)
    return rows


def render(strip, text, ascent):
    """Draws text the way the runner does: glyph rectangles start at the line top, ink at their bottom."""
    glyphs = {g["codepoint"]: g for g in strip["glyphs"]}
    canvas = [[" "] * (len(text) * (max((g["w"] for g in strip["glyphs"]), default=1) + 2) + 4)
              for _ in range(strip["maxGlyphHeight"] + 2)]
    pen = 1
    for ch in text:
        glyph = glyphs.get(ord(ch))
        if glyph is None:
            pen += ascent // 2 + 1
            continue
        ink = pixels(strip, glyph)
        # Every rectangle starts at the line top; its height was built so its bottom is the glyph's ink, which is what
        # lines the text up (a descender's rectangle is taller).
        for y, row in enumerate(ink):
            for x, bit in enumerate(row):
                if not bit:
                    continue
                at_y = y
                at_x = pen + glyph["offset"] + x
                if 0 <= at_y < len(canvas) and 0 <= at_x < len(canvas[0]):
                    canvas[at_y][at_x] = "#"
        pen += glyph["shift"]
    return "\n".join("".join(row).rstrip() for row in canvas).rstrip()


def main():
    default = Path(__file__).resolve().parents[3] / "res/n3ds/am2r/overlay/lang/fonts/cjk-font.bin"
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pack", default=str(default))
    parser.add_argument("--text", default="萨姆斯 中文 Samus")
    args = parser.parse_args()

    table = read_pack(args.pack)
    print(f"{args.pack}: {len(table)} strips")
    for (name, size), strip in table.items():
        codepoints = [g["codepoint"] for g in strip["glyphs"]]
        blank = sum(1 for g in strip["glyphs"] if g["w"] == 0 or g["h"] == 0)
        print(f"  {name}@{size}px: {len(strip['glyphs'])} glyphs ({blank} blank), "
              f"rows {strip['yOffset']}.., stride {strip['maxGlyphHeight']}, "
              f"{min(codepoints):#x}..{max(codepoints):#x}")

    # Draw with the largest strip: that is the one body text uses.
    (name, size), strip = max(table.items(), key=lambda item: item[1]["maxGlyphHeight"])
    print(f"\n{name}@{size}px, drawn at ascent {size}:")
    print(render(strip, args.text, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
