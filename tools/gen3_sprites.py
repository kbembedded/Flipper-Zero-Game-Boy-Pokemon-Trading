#!/usr/bin/env python3
"""Make sprites/252.png..386.png from a pinned pret/pokeruby clone, then rebuild
files/all_sprites.fxbm from sprites/001.png..386.png.

Usage: python3 -I tools/gen3_sprites.py <pokeruby>

The path must be the top of a clean checkout (tracked files) of the pinned commit; a sparse
checkout of graphics/pokemon is enough. The clone is read as data only; nothing in it is built
or run. Standard library only. On any failed check the script names the first failure, exits 1
and writes nothing.

Each species' front sprite (64x64, 16 colours, colour 0 transparent) becomes a 56x56 1-bit
sprite in the style of sprites/001.png..251.png, with three levels: white, a 50% checkerboard
and black.

- A colour's level comes from its luminance: below LUM_BLACK black, below LUM_WHITE
  checkerboard, else white.
- A pixel one pixel wide that is darker by more than LINE_MARGIN than both its neighbours
  along a row, a column or a diagonal is a line, and black.
- A pixel next to a transparent one is the outline, and black.
- The sprite is cropped to its pixels. One wider or taller than 56 loses the columns or rows
  that differ least from their neighbour, one at a time.
- It sits at the bottom of the 56x56 frame, centred left to right.

all_sprites.fxbm holds one 404-byte record per species, index = National Dex - 1:

    0      4    record size after this field, 400
    4      4    width 56
    8      4    height 56
    12     392  56 rows of 7 bytes, the leftmost pixel in bit 0, 1 = black

sprites/000.png is not in it; the app keeps that one in missingno_i.h.
"""

import os
import struct
import subprocess
import sys
import zlib

PINNED_COMMIT = "5784633ce4ef7ade1a7f2d2d0c288e3d5e6cdd7f"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPRITES_DIR = os.path.join(REPO_ROOT, "sprites")
FXBM_OUTPUT = os.path.join(REPO_ROOT, "files", "all_sprites.fxbm")
BULK_INPUT = os.path.join(REPO_ROOT, "files", "gen3_bulk.bin")

FIRST_NEW = 252
NATIONAL_COUNT = 386
SIZE = 56
SOURCE_SIZE = 64
ROW_BYTES = (SIZE + 7) // 8
RECORD_DATA_SIZE = 8 + ROW_BYTES * SIZE  # 400

LUM_BLACK = 60
LUM_WHITE = 165
LINE_MARGIN = 35

BULK_MAGIC = b"G3BK"
BULK_NAMES_OFFSET = 16
BULK_NAME_SIZE = 11

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

TRANSPARENT, WHITE, GREY, BLACK = range(4)


class GenError(Exception):
    pass


def fail(message):
    raise GenError(message)


# ---------------------------------------------------------------------------
# PNG: only the two kinds used here, non-interlaced: 4-bit palette (the game's sprites) and
# 1-bit greyscale (sprites/*.png)


def png_read(path):
    """Returns (width, height, rows of sample values)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        fail(f"cannot read {path}: {e.strerror}")
    if not data.startswith(PNG_SIGNATURE):
        fail(f"{path}: not a PNG")
    pos = len(PNG_SIGNATURE)
    header = None
    idat = b""
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    if header is None:
        fail(f"{path}: no IHDR")
    width, height, depth, colour, _, _, interlace = header
    if (depth, colour) not in ((4, 3), (1, 0)) or interlace:
        fail(f"{path}: depth {depth}, colour type {colour}, interlace {interlace} not supported")
    try:
        raw = zlib.decompress(idat)
    except zlib.error:
        fail(f"{path}: bad image data")
    stride = (width * depth + 7) // 8
    if len(raw) != (stride + 1) * height:
        fail(f"{path}: image data is {len(raw)} bytes, expected {(stride + 1) * height}")
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1 : start + 1 + stride])
        for i in range(stride):
            a = line[i - 1] if i else 0
            b = previous[i]
            c = previous[i - 1] if i else 0
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pred) & 0xFF
            elif kind != 0:
                fail(f"{path}: row {y} has filter {kind}")
        per_byte = 8 // depth
        mask = (1 << depth) - 1
        rows.append(
            [
                (line[x // per_byte] >> ((per_byte - 1 - x % per_byte) * depth)) & mask
                for x in range(width)
            ]
        )
        previous = line
    return width, height, rows


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def png_1bit(black):
    """A 1-bit greyscale PNG of a grid of booleans (True = black)."""
    height, width = len(black), len(black[0])
    raw = b""
    for row in black:
        line = bytearray((width + 7) // 8)
        for x, v in enumerate(row):
            if not v:
                line[x // 8] |= 0x80 >> (x % 8)
        raw += b"\x00" + bytes(line)
    ihdr = struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )


# ---------------------------------------------------------------------------
# Reading the clone


def git(clone, *args):
    """Runs a read-only git query in the clone: no fsmonitor hook, no index refresh writes."""
    command = ["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C", clone, *args]
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except OSError as e:
        fail(f"cannot run git: {e.strerror}")
    return result.returncode, result.stdout.strip()


def check_clone(clone):
    """The path must be the top of a clean checkout of the pinned commit."""
    code, top = git(clone, "rev-parse", "--show-toplevel")
    if code != 0 or os.path.realpath(top) != os.path.realpath(clone):
        fail(f"{clone} is not the top-level directory of a git clone")
    code, head = git(clone, "rev-parse", "HEAD")
    if code != 0 or head != PINNED_COMMIT:
        fail(f"{clone} is at commit {head or '(unknown)'}; need pret/pokeruby {PINNED_COMMIT}")
    code, changes = git(clone, "status", "--porcelain", "--untracked-files=no")
    if code != 0 or changes:
        fail(f"{clone} has local changes to tracked files; need a clean checkout of {PINNED_COMMIT}")


def species_names():
    """National Dex -> species name, from the app's own files/gen3_bulk.bin."""
    try:
        with open(BULK_INPUT, "rb") as f:
            data = f.read()
    except OSError as e:
        fail(f"cannot read files/gen3_bulk.bin: {e.strerror}")
    if data[:4] != BULK_MAGIC:
        fail("files/gen3_bulk.bin: bad magic")
    names = {}
    for national in range(FIRST_NEW, NATIONAL_COUNT + 1):
        start = BULK_NAMES_OFFSET + national * BULK_NAME_SIZE
        name = data[start : start + BULK_NAME_SIZE].rstrip(b"\0").decode("ascii")
        if not name:
            fail(f"files/gen3_bulk.bin: species {national} has no name")
        names[national] = name
    return names


def read_palette(path):
    """A JASC-PAL file: 16 colours as (r, g, b)."""
    try:
        with open(path, encoding="ascii") as f:
            lines = f.read().split()
    except (OSError, UnicodeDecodeError):
        fail(f"cannot read {path}")
    if lines[:3] != ["JASC-PAL", "0100", "16"] or len(lines) < 3 + 48:
        fail(f"{path}: not a 16-colour JASC-PAL file")
    values = [int(v) for v in lines[3 : 3 + 48]]
    return [tuple(values[i : i + 3]) for i in range(0, 48, 3)]


def front_sprite(clone, name):
    """The species' front sprite and normal palette. Castform has one per form; the normal
    form is the one the game shows outside battle weather."""
    folder = os.path.join(clone, "graphics", "pokemon", name.lower())
    for image, palette in (("front.png", "normal.pal"), ("front_normal_form.png", "normal_form.pal")):
        if os.path.exists(os.path.join(folder, image)):
            break
    else:
        fail(f"no front sprite for {name} in graphics/pokemon/{name.lower()}")
    width, height, pixels = png_read(os.path.join(folder, image))
    if (width, height) != (SOURCE_SIZE, SOURCE_SIZE):
        fail(f"{name}: front sprite is {width}x{height}, expected 64x64")
    return pixels, read_palette(os.path.join(folder, palette))


# ---------------------------------------------------------------------------
# Conversion


def outline(grid):
    """Every pixel next to a transparent one (or the edge) becomes black."""
    height, width = len(grid), len(grid[0])
    out = [row[:] for row in grid]
    for y in range(height):
        for x in range(width):
            if grid[y][x] == TRANSPARENT:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if not (0 <= xx < width and 0 <= yy < height) or grid[yy][xx] == TRANSPARENT:
                    out[y][x] = BLACK
                    break
    return out


def drop_rows(grid, target):
    """Removes rows, one at a time, the one that differs least from the row above it."""
    grid = [row[:] for row in grid]
    while len(grid) > target:
        best = min(
            range(1, len(grid)),
            key=lambda i: sum(a != b for a, b in zip(grid[i], grid[i - 1])),
        )
        del grid[best]
    return grid


def transpose(grid):
    return [list(col) for col in zip(*grid)]


def convert(pixels, palette):
    """A 64x64 sprite of palette indices -> 56x56 grid of booleans (True = black)."""
    lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in palette]
    level = [TRANSPARENT] + [
        BLACK if v < LUM_BLACK else GREY if v < LUM_WHITE else WHITE for v in lum[1:]
    ]
    grid = [[level[i] for i in row] for row in pixels]

    shade = [[lum[i] if i else 255.0 for i in row] for row in pixels]
    lines = []
    for y in range(1, SOURCE_SIZE - 1):
        for x in range(1, SOURCE_SIZE - 1):
            if grid[y][x] in (TRANSPARENT, BLACK):
                continue
            v = shade[y][x] + LINE_MARGIN
            for (ax, ay), (bx, by) in (
                ((-1, 0), (1, 0)),
                ((0, -1), (0, 1)),
                ((-1, -1), (1, 1)),
                ((-1, 1), (1, -1)),
            ):
                if shade[y + ay][x + ax] > v and shade[y + by][x + bx] > v:
                    lines.append((x, y))
                    break
    for x, y in lines:
        grid[y][x] = BLACK
    grid = outline(grid)

    rows = [y for y in range(SOURCE_SIZE) if any(v != TRANSPARENT for v in grid[y])]
    cols = [x for x in range(SOURCE_SIZE) if any(row[x] != TRANSPARENT for row in grid)]
    if not rows:
        fail("sprite has no pixels")
    grid = [row[cols[0] : cols[-1] + 1] for row in grid[rows[0] : rows[-1] + 1]]
    if len(grid[0]) > SIZE:
        grid = transpose(drop_rows(transpose(grid), SIZE))
    if len(grid) > SIZE:
        grid = drop_rows(grid, SIZE)
    grid = outline(grid)

    left, top = (SIZE - len(grid[0])) // 2, SIZE - len(grid)
    black = [[False] * SIZE for _ in range(SIZE)]
    for y, row in enumerate(grid):
        for x, v in enumerate(row):
            fx, fy = left + x, top + y
            black[fy][fx] = v == BLACK or (v == GREY and (fx + fy) % 2 == 0)
    return black


# ---------------------------------------------------------------------------
# Output


def fxbm_record(black):
    data = bytearray()
    for row in black:
        line = bytearray(ROW_BYTES)
        for x, v in enumerate(row):
            if v:
                line[x // 8] |= 1 << (x % 8)
        data += line
    return struct.pack("<III", RECORD_DATA_SIZE, SIZE, SIZE) + bytes(data)


def sprite_grid(path):
    width, height, rows = png_read(path)
    if (width, height) != (SIZE, SIZE):
        fail(f"{os.path.relpath(path, REPO_ROOT)} is {width}x{height}, expected 56x56")
    return [[v == 0 for v in row] for row in rows]


def main(argv):
    if len(argv) != 2:
        print("usage: python3 -I tools/gen3_sprites.py <pokeruby>", file=sys.stderr)
        return 2
    clone = argv[1]
    try:
        check_clone(clone)
        new = {}
        for national, name in species_names().items():
            pixels, palette = front_sprite(clone, name)
            try:
                new[national] = convert(pixels, palette)
            except GenError as e:
                fail(f"{name}: {e}")
        fxbm = b""
        for national in range(1, NATIONAL_COUNT + 1):
            if national in new:
                black = new[national]
            else:
                black = sprite_grid(os.path.join(SPRITES_DIR, f"{national:03d}.png"))
            fxbm += fxbm_record(black)
    except GenError as e:
        print(f"gen3_sprites.py: error: {e}", file=sys.stderr)
        return 1
    for national, black in new.items():
        with open(os.path.join(SPRITES_DIR, f"{national:03d}.png"), "wb") as f:
            f.write(png_1bit(black))
    print(f"wrote sprites/{FIRST_NEW:03d}.png..{NATIONAL_COUNT:03d}.png")
    with open(FXBM_OUTPUT, "wb") as f:
        f.write(fxbm)
    print(f"wrote {os.path.relpath(FXBM_OUTPUT, REPO_ROOT)} ({len(fxbm)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
