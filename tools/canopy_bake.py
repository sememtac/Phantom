#!/usr/bin/env python3
"""Turn a canopy drawing into a table the firmware can draw.

    python tools/canopy_bake.py design/Test.png src/vg/vg_canopy_data.h

The drawing is a grey field with the frame drawn on it. Mid grey means "leave this
pixel alone"; brighter means add light, darker means take it away. The firmware
applies the table to the finished picture, so the frame lights whatever is behind
it instead of painting over it.

The table stores COLUMNS, not rows. The panel is turned a quarter turn, so one
band of the display is 32 columns of the picture, and a column layout keeps each
band's data together.

Only the left half is stored. The drawing must be left-right symmetric; this
program checks and refuses if it is not. The right half is drawn from the same
data mirrored.

Empty pixels are not stored at all. A frame covers about 7% of the screen, so the
table holds runs of used pixels and skips the rest.
"""
import sys
from PIL import Image

PANEL = 480          # the display is 480 x 480
TOL = 6              # ignore a pixel this close to the background
QUANT = 4            # snap levels to this, so flat areas become one run


def bake(src, out, name="CANOPY"):
    im = Image.open(src).convert("L")
    sw, sh = im.size
    px = im.load()

    hist = {}
    for y in range(sh):
        for x in range(sw):
            hist[px[x, y]] = hist.get(px[x, y], 0) + 1
    bg = max(hist, key=lambda v: hist[v])

    # SYMMETRY IS DETECTED, NOT REQUIRED. A symmetric drawing stores its left half and
    # is mirrored at draw time, which halves both the table and the reading. An
    # asymmetric one stores every column. Which it is, is the drawing's business: a
    # cockpit frame is allowed to be lopsided, and refusing one was the tool telling
    # the author how to draw.
    bad = worst = 0
    for y in range(sh):
        for x in range(sw // 2):
            d = abs(px[x, y] - px[sw - 1 - x, y])
            if d > TOL:
                bad += 1
                worst = max(worst, d)
    mirror = bad <= sw * sh // 2000        # a fringe of antialiasing either way is fine

    scale = sw / float(PANEL)

    def sample(x, y):
        """The drawing at panel resolution, bilinear."""
        sx, sy = x * scale, y * scale
        x0, y0 = int(sx), int(sy)
        fx, fy = sx - x0, sy - y0
        x1, y1 = min(x0 + 1, sw - 1), min(y0 + 1, sh - 1)
        a, b = px[x0, y0], px[x1, y0]
        c, d = px[x0, y1], px[x1, y1]
        top = a + (b - a) * fx
        bot = c + (d - c) * fx
        return int(round(top + (bot - top) * fy))

    cols = PANEL // 2 if mirror else PANEL
    stream = bytearray()
    offsets = []
    runs = used = 0

    # RUNS OF ONE LEVEL, not a level per pixel.
    #
    # A flat member is most of the drawing -- the body alone is over 40% of the covered
    # pixels -- and storing a grey for each one made the renderer re-fetch the colour
    # table and re-derive the channel deltas for every pixel of a solid area. A run
    # carries its level once and the inner loop hoists all of that out.
    #
    # Levels are snapped to QUANT first, which is what makes the runs long: a gradient
    # that wanders by one grey per pixel would otherwise be a run each. At 5 bits per
    # channel out, steps below about 4 greys cannot be told apart anyway.
    for col in range(cols):
        offsets.append(len(stream))
        y = 0
        while y < PANEL:
            g = sample(col, y)
            if abs(g - bg) <= TOL:
                y += 1
                continue
            lvl = bg + int(round((g - bg) / float(QUANT))) * QUANT
            lvl = max(0, min(255, lvl))
            y0 = y
            n = 0
            while y < PANEL:
                g2 = sample(col, y)
                if abs(g2 - bg) <= TOL:
                    break
                l2 = bg + int(round((g2 - bg) / float(QUANT))) * QUANT
                if max(0, min(255, l2)) != lvl:
                    break
                n += 1
                y += 1
            stream += bytes([y0 >> 8, y0 & 255, n >> 8, n & 255, lvl])
            runs += 1
            used += n
    offsets.append(len(stream))

    # WHERE EACH BAND BALANCES.
    #
    # The pass is split across the two cores, and a band costs whichever half is
    # slower, not the average. Splitting at the midpoint looked fair and was not: a
    # frame is not spread evenly across a band's 32 columns, so a band whose work sits
    # mostly on one side saved nothing at all. Measured, a midpoint split returned 1.2
    # of the 1.9 ms it should have.
    #
    # So the split point is computed per band from the real pixel counts. Fifteen bytes.
    per_col = []
    for col in range(PANEL):
        c = col if not mirror else (col if col < cols else PANEL - 1 - col)
        n = 0
        i, end = offsets[c], offsets[c + 1]
        while i + 5 <= end:
            n += (stream[i + 2] << 8) | stream[i + 3]
            i += 5
        per_col.append(n)

    band_h = 32
    splits = []
    for b in range(PANEL // band_h):
        # panel row by0+r is column 479-(by0+r)
        rows = [per_col[PANEL - 1 - (b * band_h + r)] for r in range(band_h)]
        total = sum(rows)
        if total == 0:
            splits.append(band_h // 2)
            continue
        best, best_at = None, band_h // 2
        run = 0
        for r in range(1, band_h):
            run += rows[r - 1]
            gap = abs(run - (total - run))
            if best is None or gap < best:
                best, best_at = gap, r
        splits.append(best_at)

    with open(out, "w") as fh:
        fh.write(f"// GENERATED by tools/canopy_bake.py from {src} -- do not edit.\n")
        fh.write("//\n")
        fh.write(f"// {runs} runs, {used} pixels a half-frame, "
                 f"{(len(stream) + len(offsets) * 2) / 1024.0:.1f} KB.\n")
        fh.write(f"// Background level {bg}: that value means no change.\n")
        fh.write("//\n")
        fh.write("// Columns of the picture. Each column is a list of runs, five bytes\n")
        fh.write("// each: two of start, two of length, one grey level for the whole run.\n")
        if mirror:
            fh.write("//\n// SYMMETRIC: the left half is stored and mirrored at draw time.\n")
        else:
            fh.write(f"//\n// ASYMMETRIC ({bad} pixels differ, worst {worst}), so every"
                     " column is stored.\n")
        fh.write("#pragma once\n#include <stdint.h>\n\n")
        fh.write(f"#define {name}_BG {bg}\n")
        fh.write(f"#define {name}_COLS {cols}\n")
        fh.write(f"#define {name}_MIRROR {1 if mirror else 0}\n\n")
        fh.write("// Where each band's pixels balance, so the two cores get equal work.\n")
        fh.write(f"static const uint8_t {name}_SPLIT[{len(splits)}] = {{\n    "
                 + ",".join(str(v) for v in splits) + ",\n};\n\n")
        fh.write(f"static const uint16_t {name}_OFS[{len(offsets)}] = {{\n")
        for i in range(0, len(offsets), 16):
            fh.write("    " + ",".join(str(v) for v in offsets[i:i + 16]) + ",\n")
        fh.write("};\n\n")
        fh.write(f"static const uint8_t {name}_DATA[{len(stream)}] = {{\n")
        for i in range(0, len(stream), 24):
            fh.write("    " + ",".join(str(v) for v in stream[i:i + 24]) + ",\n")
        fh.write("};\n")

    kb = (len(stream) + len(offsets) * 2) / 1024.0
    print(f"{src}: background {bg}, {runs} runs, {kb:.1f} KB -> {out}")

    # WHAT IT COSTS, which is the number to watch while drawing.
    #
    # Every stored pixel is read, changed and written back, which is about ten times
    # the work of a plain pixel. The whole frame has 16.7 ms to spend at 60 frames a
    # second and about 4 ms of that is free, so a frame this size is a real charge
    # against it. Coverage is the only lever: thinner shapes cost less, and nothing
    # else about the drawing matters to the budget.
    print("  symmetric, stored as a half" if mirror
          else f"  asymmetric ({bad} px differ, worst {worst}), stored whole")
    # 34-40 cycles a pixel, MEASURED on the device rather than reasoned about: a frame
    # covering 12.7% of the screen billed 4.5 ms, which is 37 cycles each. The first
    # version of this estimate guessed 15-25 and was wrong by 1.7x, which is worse than
    # no estimate -- it would have had the author drawing into a budget that was not
    # there. A load, a byte swap, three extracts, three saturating adds, a repack, a
    # swap, a store and a table lookup is just what it costs.
    whole = used * 2 if mirror else used
    lo = whole * 34.0 / 240.0              # cycles per pixel, at 240 MHz, in us
    hi = whole * 40.0 / 240.0
    print(f"  {whole} pixels a frame, {whole * 100.0 / (PANEL * PANEL):.1f}% of the screen")
    print(f"  costs roughly {lo / 1000.0:.2f}-{hi / 1000.0:.2f} ms a frame")
    # About 4 ms of the frame is spare at 60 a second, and the canopy is not the only
    # thing that wants it.
    if lo > 2500.0:
        print("  TOO HEAVY: measured at this coverage, the game drops to about 50 a")
        print("     second. Roughly 6% of the screen is what fits.")
    elif lo > 1200.0:
        print("  HEAVY: expect to lose 60 in busy fights. Narrowing the shapes is the")
        print("     only lever that matters -- levels and detail are free, area is not.")
    elif lo > 600.0:
        print("  worth watching, but there is room for it")
    else:
        print("  cheap")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bake(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "CANOPY")
