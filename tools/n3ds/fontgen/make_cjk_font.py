#!/usr/bin/env python3
"""Bakes the CJK glyphs AM2R's language packs ask for into one texture page.

The 3DS never parses a TTF: this script reads the fonts' own 1-bit bitmap strikes offline and writes
`cjk-font.bin`, which src/n3ds/n3ds_cjk_font.c loads at the first font_replace and turns into one 1024-wide texture
that every replaced font points at.

Why the strikes and not the outlines: the packs ship pixel fonts (fusion-pixel-font), and rasterizing their outlines
comes out grey and too bold. The bitmaps here are the same ones the game's layout was designed around.

    python3 make_cjk_font.py [--lang DIR] [--fonts DIR] [--out DIR]

The paths default to `res/n3ds/am2r/overlay/lang` (the language files) and its `fonts/` subdirectory, in and out, so
a plain run from the repository root regenerates the pack the build ships. What it bakes:

* every character the language files use (all of them, so switching language needs no rebuild),
* printable ASCII and the CJK punctuation the translations lean on,
* every Chinese string literal in src/n3ds/*.c -- the port's own front-end pages (Display, Cheats) draw text that is
  in no language file, and a character missing from the pack renders as a blank.

Rectangles use the convention the runner draws with (see docs/font-plan.md): a glyph's rectangle starts at the line
top and ends on the baseline, the ink sits at its bottom, and the line stride is the tallest rectangle.
"""
import argparse
import hashlib
import re
import struct
import sys
import zlib
from pathlib import Path

ATLAS_WIDTH = 1024
ATLAS_MAX_HEIGHT = 1024

# What oControl's Create event asks font_replace for: (font file, key in sizes.ini, fallback size). The keys are the
# game's own (sizes.ini is keyed by FILENAME, not by GameMaker font name -- see docs/localization.md).
REQUESTS = [
    ("04b09.ttf", "04b09_(large)", 12),  # fontGUI
    ("04b09.ttf", "04b09_(small)", 10),  # fontSubScr (04b24 replaces it again later)
    ("Acknowledge_TT_BRK.ttf", "Acknowledge_TT_BRK", 12),  # fontGUI2
    ("uni_05_53.ttf", "uni_05_53", 10),  # fontMenuSmall
    ("04b24.ttf", "04b24", 10),  # fontMenuTiny
    ("Glasstown_NBP.ttf", "Glasstown_NBP", 12),  # fontMenuSmall2
]

PUNCTUATION = "“”‘’—…×÷°％·、。，！？：；（）《》【】"


# ===[ TTF: table lookup, cmap and the bitmap strike ]===
# A port of the parser the port used to run at boot (src/n3ds/n3ds_font_replace.c, now deleted). It was checked glyph
# by glyph against that C code before it was removed, which is what keeps these bitmaps pixel-exact.

class Font:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        self.tables = {}
        count = struct.unpack(">H", self.data[4:6])[0]
        for i in range(count):
            record = 12 + i * 16
            tag = self.data[record:record + 4].decode("latin1")
            offset, length = struct.unpack(">II", self.data[record + 8:record + 16])
            self.tables[tag] = (offset, length)
        self._cmap = None

    def cmap(self):
        """codepoint -> glyph id, from the (3,1) format 4 subtable."""
        if self._cmap is not None:
            return self._cmap
        data = self.data
        offset, _ = self.tables["cmap"]
        count = struct.unpack(">H", data[offset + 2:offset + 4])[0]
        subtable = None
        for i in range(count):
            platform, encoding, relative = struct.unpack(">HHI", data[offset + 4 + i * 8:offset + 12 + i * 8])
            at = offset + relative
            if struct.unpack(">H", data[at:at + 2])[0] != 4:
                continue
            subtable = at
            if (platform, encoding) == (3, 1):
                break
        if subtable is None:
            raise SystemExit("no format 4 cmap subtable")
        seg_x2 = struct.unpack(">H", data[subtable + 6:subtable + 8])[0]
        segments = seg_x2 // 2
        base = subtable + 14
        ends = struct.unpack(">%dH" % segments, data[base:base + seg_x2])
        starts = struct.unpack(">%dH" % segments, data[base + seg_x2 + 2:base + 2 * seg_x2 + 2])
        deltas = struct.unpack(">%dh" % segments, data[base + 2 * seg_x2 + 2:base + 3 * seg_x2 + 2])
        range_offset_pos = base + 3 * seg_x2 + 2
        range_offsets = struct.unpack(">%dH" % segments, data[range_offset_pos:range_offset_pos + seg_x2])
        mapping = {}
        for i in range(segments):
            for codepoint in range(starts[i], min(ends[i], 0xFFFE) + 1):
                if range_offsets[i] == 0:
                    glyph = (codepoint + deltas[i]) & 0xFFFF
                else:
                    at = range_offset_pos + i * 2 + range_offsets[i] + (codepoint - starts[i]) * 2
                    glyph = struct.unpack(">H", data[at:at + 2])[0]
                    if glyph:
                        glyph = (glyph + deltas[i]) & 0xFFFF
                if glyph:
                    mapping[codepoint] = glyph
        self._cmap = mapping
        return mapping

    def strike(self, em_size):
        """The bitmapSizeTable closest to em_size. The packs carry exactly one strike per file."""
        data = self.data
        eblc, _ = self.tables["EBLC"]
        count = struct.unpack(">I", data[eblc + 4:eblc + 8])[0]
        best, best_distance = None, 1 << 32
        for i in range(count):
            entry = eblc + 8 + i * 48
            ppem = data[entry + 44]
            distance = abs(ppem - em_size)
            if distance < best_distance:
                best, best_distance = i, distance
        if best is None:
            raise SystemExit("no bitmap strike")
        size_table = eblc + 8 + best * 48
        return {"eblc": eblc, "ebdt": self.tables["EBDT"][0], "size_table": size_table,
                "ppem": data[size_table + 44]}

    def _subtable(self, strike, glyph_id):
        data = self.data
        array_offset = struct.unpack(">I", data[strike["size_table"]:strike["size_table"] + 4])[0]
        count = struct.unpack(">I", data[strike["size_table"] + 8:strike["size_table"] + 12])[0]
        array = strike["eblc"] + array_offset
        for i in range(count):
            entry = array + i * 8
            first, last, additional = struct.unpack(">HHI", data[entry:entry + 8])
            if not (first <= glyph_id <= last):
                continue
            header = array + additional
            index_format, image_format = struct.unpack(">HH", data[header:header + 4])
            out = {"first": first, "last": last, "index_format": index_format, "image_format": image_format,
                   "header": header, "metrics": None, "image_size": 0,
                   "image_data_offset": struct.unpack(">I", data[header + 4:header + 8])[0]}
            if index_format == 2:
                out["image_size"] = struct.unpack(">I", data[header + 8:header + 12])[0]
                if image_format == 5:
                    out["metrics"] = header + 12
            if image_format == 5 and out["metrics"] is None:
                out["metrics"] = header + 8
            return out
        return None

    def glyph(self, strike, glyph_id):
        """(width, height, bearingX, bearingY, advance, rows-of-0/1) or None."""
        data = self.data
        sub = self._subtable(strike, glyph_id)
        if sub is None:
            return None
        image_format = sub["image_format"]
        if sub["index_format"] in (1, 3):
            # One offset per glyph PLUS a final one that ends the last glyph: a glyph's size is the gap to the next.
            slot = glyph_id - sub["first"]
            table = sub["header"] + 8
            if sub["index_format"] == 1:
                offset = struct.unpack(">I", data[table + slot * 4:table + slot * 4 + 4])[0]
                following = struct.unpack(">I", data[table + (slot + 1) * 4:table + (slot + 1) * 4 + 4])[0]
            else:
                offset = struct.unpack(">H", data[table + slot * 2:table + slot * 2 + 2])[0]
                following = struct.unpack(">H", data[table + (slot + 1) * 2:table + (slot + 1) * 2 + 2])[0]
            entry_size = following - offset
            at = strike["ebdt"] + sub["image_data_offset"] + offset
        else:
            if sub["image_size"] == 0:
                return None
            entry_size = sub["image_size"]
            at = strike["ebdt"] + sub["image_data_offset"] + entry_size * (glyph_id - sub["first"])
        if image_format in (1, 2, 8, 9):
            if entry_size < 5:
                return None
            height, width = data[at], data[at + 1]
            bearing_x, bearing_y = self._s8(at + 2), self._s8(at + 3)
            advance = data[at + 4]
            bits_at, byte_aligned = at + 5, image_format in (1, 2)
        elif image_format == 5:
            if sub["metrics"] is None:
                return None
            metrics = sub["metrics"]
            height, width = data[metrics], data[metrics + 1]
            bearing_x, bearing_y = self._s8(metrics + 2), self._s8(metrics + 3)
            advance = data[metrics + 4]
            bits_at, byte_aligned = at, False
        elif image_format in (6, 7):
            if entry_size < 8:
                return None
            height, width = data[at], data[at + 1]
            bearing_x, bearing_y = self._s8(at + 2), self._s8(at + 3)
            advance = data[at + 4]
            bits_at, byte_aligned = at + 8, image_format == 6
        else:
            return None
        rows = []
        if width and height:
            stride = (width + 7) // 8
            for y in range(height):
                row = []
                for x in range(width):
                    if byte_aligned:
                        bit = (data[bits_at + y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
                    else:
                        index = y * width + x  # formats 5 and 7 are one continuous bit stream, not padded rows
                        bit = (data[bits_at + (index >> 3)] >> (7 - (index & 7))) & 1
                    row.append(bit)
                rows.append(row)
        return {"w": width, "h": height, "bx": bearing_x, "by": bearing_y, "adv": advance, "rows": rows}

    def _s8(self, at):
        return struct.unpack(">b", self.data[at:at + 1])[0]


# ===[ What to bake ]===

def language_chars(lang_dir):
    """Every character the language files the game ships can show."""
    files = []
    listing = lang_dir / "languages.txt"
    if listing.exists():
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            name = line.split("#", 1)[0].strip()
            if name:
                files.append(lang_dir / name)
    else:
        files = sorted(lang_dir.glob("*.ini"))
    chars = set()
    for path in files:
        if not path.exists():
            print(f"missing language file: {path}")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"<[^>]+>", "", text)  # drop inline markup
        chars |= set(text)
    return chars


def port_chars(source_dir):
    """The Chinese the port's own pages draw: the string literals in src/n3ds, which no language file holds."""
    chars = set()
    for path in sorted(Path(source_dir).glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for literal in re.findall(r'"([^"\n]*)"', text):
            if any(ord(c) > 0x7F for c in literal):
                chars |= set(literal)
    return chars


def collect_chars(lang_dir, source_dir):
    chars = language_chars(lang_dir) | port_chars(source_dir)
    chars |= {chr(c) for c in range(0x20, 0x7F)}
    chars |= set(PUNCTUATION)
    return sorted(ord(c) for c in chars if c not in "\r\n\t")


def read_sizes(fonts_dir):
    """sizes.ini's [Sizes] section, key -> int. No file -> {}."""
    sizes = {}
    path = fonts_dir / "sizes.ini"
    if not path.exists():
        return sizes
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("[") or "=" not in line or line.startswith(" "):
            continue
        key, value = line.split("=", 1)
        found = re.match(r'\s*"?\s*(-?\d+)', value)  # the values are quoted strings: 04b09_(large)="12"
        if found:
            sizes[key.strip().strip('"')] = int(found.group(1))
    return sizes


# ===[ Rectangles and packing ]===

def build_strip(font, em_size, codepoints):
    """One (font, size)'s glyph records, shaped the way the runner draws them.

    A rectangle starts at the line top and its bottom edge is the baseline, so its height is the ascent minus the
    glyph's bearing plus its ink height, and the ink sits at the rectangle's bottom. The requested size IS the ascent
    (that is what the game means by it); a strike taller than the request keeps its bitmaps as they are.
    """
    strike = font.strike(em_size)
    cmap = font.cmap()
    glyphs = []
    for codepoint in codepoints:
        glyph_id = cmap.get(codepoint)
        if glyph_id is None:
            continue
        ink = font.glyph(strike, glyph_id)
        if ink is None:
            continue
        left = abs(ink["bx"])
        # A glyph that pokes above the requested ascent would get a negative rectangle: clamp it to the line top and
        # let it sit a pixel or two below the baseline instead of drawing nothing at all (what the runtime did).
        above = max(0, em_size - ink["by"])
        glyphs.append({
            "codepoint": codepoint,
            "width": left + ink["w"],
            "height": above + ink["h"],
            "ink_x": left if ink["bx"] >= 0 else 0,
            "ink_y": above,
            "rows": ink["rows"],
            # The game's own fonts nudge a glyph right by one, which is where the pixel font's left side bearing lands;
            # a negative bearing grows the rectangle instead of shifting the ink.
            "shift": ink["adv"],
            "offset": 1 - left if ink["bx"] < 0 else 1,
        })
    return {"ppem": strike["ppem"], "glyphs": glyphs,
            "maxGlyphHeight": max((g["height"] for g in glyphs), default=0)}


def pack_strips(strips):
    """Shelf packing, tallest strip first, into ATLAS_WIDTH columns. Sets x/y on every glyph.

    Each strip is packed as one block and no other strip is interleaved into it: every font built from a strip shares
    its yOffset, so the block has to be contiguous. Inside a block the glyphs are shelved tallest first.
    """
    cursor_y = 0
    for strip in sorted(strips, key=lambda s: -s["maxGlyphHeight"]):
        start = cursor_y
        cursor_x = 0
        shelf_height = 0
        for glyph in sorted(strip["glyphs"], key=lambda g: (-g["height"], g["codepoint"])):
            if glyph["width"] > ATLAS_WIDTH:
                raise SystemExit(f"a glyph is {glyph['width']} px wide, wider than the atlas")
            if cursor_x + glyph["width"] > ATLAS_WIDTH:
                cursor_x = 0
                cursor_y += shelf_height
                shelf_height = 0
            glyph["x"], glyph["y"] = cursor_x, cursor_y
            cursor_x += glyph["width"]
            shelf_height = max(shelf_height, glyph["height"])
        cursor_y += shelf_height
        strip["yOffset"] = start
        strip["blockHeight"] = cursor_y - start
    return cursor_y


def derive_strip(base, em_size):
    """A smaller size of the same font file, drawn from the pixels already packed for the larger one.

    Same file, same strike: the bitmaps are identical, and only the rectangles differ, because the requested size is
    the ascent (a 10px request puts a glyph's ink 2 rows higher in its rectangle than a 12px one). Cropping the blank
    rows off the top of each rectangle gives exactly the rectangles a dedicated bake would have produced, so the page
    does not carry the same ink twice. A glyph whose ink reaches the top of its rectangle (bearing above the smaller
    ascent) has nothing to crop and keeps the larger rectangle, which draws it a row or two lower than a dedicated
    bake would - one character in 1291, and not a visible one.
    """
    delta = base["ascent"] - em_size
    glyphs = []
    for glyph in base["glyphs"]:
        copied = dict(glyph)
        if delta > 0 and copied["height"] - delta >= 1 and copied["ink_y"] >= delta:
            copied["y"] += delta
            copied["ink_y"] -= delta
            copied["height"] -= delta
        glyphs.append(copied)
    return {"ppem": base["ppem"], "ascent": em_size, "glyphs": glyphs,
            "maxGlyphHeight": max((g["height"] for g in glyphs), default=0), "names": [],
            "yOffset": base["yOffset"], "blockHeight": base["blockHeight"]}


def next_power_of_two(value, minimum=8):
    result = minimum
    while result < value:
        result <<= 1
    return result


# ===[ Output ]===

def write_png(path, width, height, rows):
    """A greyscale preview of the page (white paper, black ink). The 3DS never reads this."""
    raw = b"".join(b"\x00" + bytes(0 if bit else 255 for bit in row) for row in rows)
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lang", default=str(root / "res/n3ds/am2r/overlay/lang"), help="language file directory")
    parser.add_argument("--fonts", default=None, help="TTF directory (default: <lang>/fonts)")
    parser.add_argument("--out", default=None, help="output directory (default: <lang>/fonts)")
    parser.add_argument("--source", default=str(root / "src/n3ds"), help="where the port's own Chinese strings live")
    parser.add_argument("--preview", default=str(root / "docs/cjk-font-atlas.png"),
                        help="where to write the page as a picture (review only, the 3DS never reads it)")
    args = parser.parse_args()

    lang_dir = Path(args.lang)
    fonts_dir = Path(args.fonts) if args.fonts else lang_dir / "fonts"
    out_dir = Path(args.out) if args.out else fonts_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    codepoints = collect_chars(lang_dir, args.source)
    sizes = read_sizes(fonts_dir)
    print(f"{len(codepoints)} characters to bake, sizes.ini {'read' if sizes else 'missing (using the defaults)'}")

    # One image per font file's CONTENTS: three of the five files are byte-identical, so they share it, and a file asked
    # for at two sizes (04b09 at 12 and 10) is packed once at the larger size - the smaller one reuses those pixels.
    groups = {}
    order = []
    for name, size_key, fallback in REQUESTS:
        path = fonts_dir / name
        if not path.exists():
            print(f"missing font: {path}")
            continue
        content = hashlib.md5(path.read_bytes()).hexdigest()
        em_size = sizes.get(size_key, fallback)
        if content not in groups:
            groups[content] = {"path": path, "requests": []}
            order.append(content)
        groups[content]["requests"].append((name, em_size))

    bases = []
    for content in order:
        group = groups[content]
        largest = max(em_size for _, em_size in group["requests"])
        strip = build_strip(Font(group["path"]), largest, codepoints)
        strip["ascent"] = largest
        strip["names"] = [(n, s) for n, s in group["requests"] if s == largest]
        strip["derived"] = []
        bases.append(strip)
        print(f"  {group['path'].name} @ {largest}px: {len(strip['glyphs'])} glyphs from the {strip['ppem']}px strike, "
              f"tallest {strip['maxGlyphHeight']} rows")

    if not bases:
        sys.exit("no fonts found")

    used_rows = pack_strips(bases)
    # A size the same file is also asked for gets rectangles cropped out of the packed ones, not a second copy of the
    # ink: 04b09@10 would otherwise sit in the page as ~150 rows that are an exact duplicate of 04b09@12.
    strips = []
    for content, base in zip(order, bases):
        strips.append(base)
        for name, em_size in groups[content]["requests"]:
            if em_size == base["ascent"]:
                continue
            derived = derive_strip(base, em_size)
            derived["names"] = [(name, em_size)]
            base["derived"].append(derived)
            strips.append(derived)
    if used_rows > ATLAS_MAX_HEIGHT:
        sys.exit(f"the strips need {used_rows} rows, more than the {ATLAS_MAX_HEIGHT}-row page allows")
    texture_height = next_power_of_two(used_rows)

    # Lay the ink out: white pixels for the glyphs, everything else transparent.
    page = [bytearray(ATLAS_WIDTH) for _ in range(used_rows)]
    for strip in bases:  # a derived strip shares these pixels; drawing it too would paint them twice
        for glyph in strip["glyphs"]:
            base_x = glyph["x"] + glyph["ink_x"]
            for row_index, row in enumerate(glyph["rows"]):
                y = glyph["y"] + glyph["ink_y"] + row_index
                if y >= used_rows:
                    continue
                target = page[y]
                for x, bit in enumerate(row):
                    if bit and base_x + x < ATLAS_WIDTH:
                        target[base_x + x] = 1

    stride = ATLAS_WIDTH // 8
    bitmap = bytearray(stride * used_rows)
    for y, row in enumerate(page):
        offset = y * stride
        for x, bit in enumerate(row):
            if bit:
                bitmap[offset + (x >> 3)] |= 0x80 >> (x & 7)

    # Header, strip table, glyph table, then the 1-bit rows. Little-endian fixed sizes: the loader is plain C.
    glyph_records = []
    glyph_offsets = {}
    strip_count = sum(len(strip["names"]) for strip in strips)
    header_size = 32
    strip_table_at = header_size
    glyph_table_at = strip_table_at + 44 * strip_count
    for strip in strips:
        glyph_offsets[id(strip)] = glyph_table_at + len(glyph_records) * 14
        for glyph in strip["glyphs"]:
            glyph_records.append(struct.pack("<HHHHHhh", glyph["codepoint"], glyph["x"], glyph["y"],
                                             glyph["width"], glyph["height"], glyph["shift"], glyph["offset"]))
    bitmap_at = glyph_table_at + len(glyph_records) * 14

    pack = bytearray()
    # magic, version, page size, rows used, strip count, strip table, bitmap, bitmap size, reserved (keeps the
    # header a round 32 bytes).
    pack += struct.pack("<4sIHHHHIIII", b"CJKF", 1, ATLAS_WIDTH, texture_height, used_rows,
                        strip_count, strip_table_at, bitmap_at, len(bitmap), 0)
    for strip in strips:
        for name, em_size in strip["names"]:
            pack += struct.pack("<32sHHHHI", name.encode("ascii")[:31], em_size, strip["yOffset"],
                                strip["maxGlyphHeight"], len(strip["glyphs"]), glyph_offsets[id(strip)])
    for record in glyph_records:
        pack += record
    pack += bitmap

    pack_path = out_dir / "cjk-font.bin"
    pack_path.write_bytes(pack)
    # The picture is for review only: it goes to docs/ rather than next to the pack, which is copied to every card.
    preview_path = Path(args.preview)
    if preview_path.parent.exists() or preview_path.parent.mkdir(parents=True, exist_ok=True) is None:
        write_png(preview_path, ATLAS_WIDTH, used_rows, page)

    print(f"{len(strips)} strips, {len(glyph_records)} glyph records, {used_rows}/{texture_height} texture rows")
    print(f"wrote {pack_path} ({len(pack) // 1024} KB) and {preview_path}")
    for strip in strips:
        end = strip["yOffset"] + strip["blockHeight"]
        names = ", ".join(f"{n}@{s}" for n, s in strip["names"])
        print(f"  rows {strip['yOffset']:4d}-{end:4d}  {names}")


if __name__ == "__main__":
    main()
