#!/usr/bin/env python3
"""Turn a canopy drawing into a table the firmware can draw.

    python tools/canopy_bake.py design/Test.png src/vg/vg_canopy_data.h
    python tools/canopy_bake.py design/Chariot.png src/vg/vg_canopy_chariot.h \n        --name=CANOPY_CHARIOT

Give each drawing its own --name. The firmware holds every cockpit at once and
selects one per ship, so two headers that define the same name cannot both be
used. See src/vg/vg_canopy_set.cpp.

The drawing is a grey field with the frame drawn on it. Mid grey means "leave this
pixel alone"; brighter means add light, darker means take it away. The firmware
applies the table to the finished picture, so the frame lights whatever is behind
it instead of painting over it.

The table stores COLUMNS, not rows. The panel is turned a quarter turn, so one
band of the display is 32 columns of the picture, and a column layout keeps each
band's data together.

Symmetry is detected, not required. A symmetric drawing stores its left half and
is mirrored when drawn, which halves the table. A lopsided one stores every
column.

Empty pixels are not stored at all. A frame covers about 10% of the screen, so
the table holds only the used pixels and skips the rest.
"""
import sys
from PIL import Image

PANEL = 480          # the display is 480 x 480
TOL = 6              # ignore a pixel this close to the background
QUANT = 4            # snap levels to this, so flat areas become one block
MINFLAT = 4          # shorter than this is cheaper stored pixel by pixel

# WHAT A BLOCK COSTS, relative to one flat pixel. Measured on the device, not
# reasoned about: see the note above the estimate at the end of this file. These
# set where each band splits across the two cores, so they only have to be right
# in proportion to each other.
#
# Both are close to 1, and that is the finding rather than the setting: a block
# header costs less than one pixel, so a band's cost IS very nearly its pixel
# count. Balancing the split by cost instead of by pixels was expected to be
# worth about a millisecond and is worth almost nothing. It would have mattered
# under the old encoding, where headers were 17% of the cost.
ITEM_W = 0.87        # a block header, in flat pixels
LIT_W = 1.08         # a pixel that carries its own level


def bake(src, out, name="CANOPY"):
    # SWIZZLED, NOT GREY. Red carries the frame, green carries the activation mask.
    #
    # It used to open with .convert("L"), which on an RGB file is a luminance blend --
    # 0.299R + 0.587G + 0.114B, weighting the MASK most heavily of the three. That silently
    # folded the activation zones into the shape of the cockpit.
    rgb = Image.open(src).convert("RGB")
    sw, sh = rgb.size
    im, gm, _ = rgb.split()
    px = im.load()
    gp = gm.load()

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

    def gsample(x, y):
        """The mask at panel resolution, NEAREST.

        A mask must not be interpolated. Bilinear between a zone at 54 and one at 98 invents
        76, which is not a zone the artist painted and would become one -- a seam of phantom
        panels along every border between real ones.
        """
        return gp[min(int(x * scale), sw - 1), min(int(y * scale), sh - 1)]

    # WHAT ZONES THERE ARE, discovered rather than assumed.
    #
    # The artist decides how many panels there are and what order they come online in: the
    # values are the order, read low to high, and this only has to find them. A value with a
    # trivial population is a border artefact, not a panel, so it snaps to the nearest real one.
    zhist = {}
    for cx in range(PANEL):
        for cy in range(PANEL):
            if abs(sample(cx, cy) - bg) > TOL:
                v = gsample(cx, cy)
                zhist[v] = zhist.get(v, 0) + 1
    # ONE PER CENT OF THE COVERED FRAME. At half a per cent a 186-pixel fringe between two
    # real panels was promoted to a panel of its own, and would have flashed as an invisible
    # sliver in the middle of the sequence. The smallest deliberate panel in the reference
    # drawing is 746 px, three per cent, so there is room between the two.
    #
    # Anything rejected snaps to its nearest real zone, so a fringe joins the panel it borders
    # rather than disappearing.
    covered = sum(zhist.values())
    floor_px = max(64, covered // 100)
    zones = sorted(v for v, c in zhist.items() if c >= floor_px)
    if not zones:
        zones = [0]
    # REFUSED, not truncated. The firmware keeps a colour table per region, so it has a
    # ceiling -- see VG_CANOPY_MAX_ZONES. Silently dropping the extras would alias them onto
    # the last region and flash them at the wrong time, which reads as a drawing mistake.
    if len(zones) > MAX_ZONES:
        raise SystemExit(
            "%s: %d activation regions in the green channel, and the limit is %d.\n"
            "  found these values: %s\n"
            "  Use fewer distinct greys. Regions meant to come on together must share EXACTLY\n"
            "  one value, so look for a feathered or anti-aliased border between two of them --\n"
            "  a soft edge invents values that are neither." %
            (src, len(zones), MAX_ZONES, ", ".join(str(v) for v in zones)))

    def zone_of(v):
        best, bi = None, 0
        for i, z in enumerate(zones):
            d = abs(v - z)
            if best is None or d < best:
                best, bi = d, i
        return bi

    def level(g):
        """The stored grey, snapped so that flat areas become one block.

        Steps below about four greys cannot be told apart once the level is turned
        into a 5-bit-per-channel colour, so this loses nothing visible and it is
        what makes a solid member one block instead of a hundred.
        """
        return max(0, min(255, bg + int(round((g - bg) / float(QUANT))) * QUANT))

    cols = PANEL // 2 if mirror else PANEL

    # TWO KINDS OF BLOCK, because the drawing has two kinds of pixel in it.
    #
    # Most of the covered area is a flat member: a solid stretch of one level, which
    # is stored once and drawn with the colour and the channel deltas hoisted out of
    # the inner loop. That is the cheap path and it carries 80% of the pixels.
    #
    # The rest is the antialiased edge of a shape, where every pixel is a different
    # level. Stored as flat blocks those were 78% of all the blocks while carrying
    # 18% of the pixels -- four bytes of header to deliver one byte of level, and a
    # header decode is worth several pixels. So a stretch of short runs is stored as
    # one LITERAL block: one header, then a level per pixel. It pays a table lookup
    # per pixel and saves the headers, which is the right trade whenever it absorbs
    # more than about two short runs.
    #
    # A block never crosses the background, so it is all-add or all-subtract and the
    # inner loop needs no test per pixel. Splitting at the crossings turned out to
    # cost nothing: they already fall on flat-run boundaries.
    def build(zone_split):
      stream = bytearray()
      offsets = []
      n_flat = n_lit = flat_px = lit_px = 0
      col_cost = []
      for col in range(cols):
        offsets.append(len(stream))
        field = [sample(col, y) for y in range(PANEL)]
        cost = 0.0
        y = 0
        while y < PANEL:
            if abs(field[y] - bg) <= TOL:
                y += 1
                continue

            # Gather one contiguous, one-sided stretch as (level, count) pairs.
            up = field[y] > bg
            z_at = zone_of(gsample(col, y))
            y_at = y
            seg = []
            while y < PANEL:
                g = field[y]
                if abs(g - bg) <= TOL or (g > bg) != up:
                    break
                # A block belongs to ONE zone, so a zone border ends the run as surely as a
                # change of level does -- the intro switches whole blocks on.
                if zone_split and zone_of(gsample(col, y)) != z_at:
                    break
                lv = level(g)
                if seg and seg[-1][0] == lv:
                    seg[-1][1] += 1
                else:
                    seg.append([lv, 1])
                y += 1

            # Emit it: long runs flat, and everything else batched into literals.
            def put(y0, n, lit, levels):
                nonlocal cost, n_flat, n_lit, flat_px, lit_px
                # bits 7 literal, 6 subtract, 5..2 zone, 1 length bit 8, 0 start bit 8.
                head = ((0x80 if lit else 0) | (0x00 if up else 0x40)
                        | ((z_at & 15) << 2)
                        | ((n >> 8) << 1) | (y0 >> 8))
                stream.extend([head, y0 & 255, n & 255])
                stream.extend(levels)
                if lit:
                    n_lit += 1
                    lit_px += n
                    cost += ITEM_W + LIT_W * n
                else:
                    n_flat += 1
                    flat_px += n
                    cost += ITEM_W + n

            at = y_at
            pend = []
            for lv, n in seg:
                if MINFLAT and n >= MINFLAT:
                    if pend:
                        put(at - len(pend), len(pend), True, pend)
                        pend = []
                    put(at, n, False, [lv])
                else:
                    pend.extend([lv] * n)
                at += n
            if pend:
                put(at - len(pend), len(pend), True, pend)
        col_cost.append(cost)
      offsets.append(len(stream))
      return stream, offsets, n_flat, n_lit, flat_px, lit_px, col_cost

    # TWO TABLES, and the reason is that the intro must cost nothing once it is over.
    #
    # A block belongs to one zone, so tagging them splits every run that crosses a zone border:
    # 2,492 blocks became 3,563 and the pass grew 140 us. That is a standing charge on every
    # frame of every flight, for a sequence that plays once.
    #
    # So the flight table is built WITHOUT zone splits -- byte for byte what it always was --
    # and a second one with them serves the intro alone. 17 KB of flash against 90 us of frame,
    # for ever, which is not a close call on a part with 2.6 MB spare.
    stream, offsets, n_flat, n_lit, flat_px, lit_px, col_cost = build(False)
    i_stream, i_offsets, i_flat, i_lit, i_fpx, i_lpx, _ = build(True)

    # THE ZONE MAP, over the whole screen and not just the frame.
    #
    # The intro is not a canopy effect. The world stays black until a region comes online, so
    # every pixel needs a zone -- including the 92% the cockpit does not cover, which the block
    # table has no way to describe because it stores only what the frame touches.
    #
    # Run coded by column, the same shape as everything else here, because the regions are few
    # and large: three bytes a run, and a column crosses only a handful of them. Nearest
    # sampled, for the reason gsample gives.
    zstream = bytearray()
    zoffs = []
    zruns = 0
    for col in range(PANEL):
        zoffs.append(len(zstream))
        y = 0
        while y < PANEL:
            z = zone_of(gsample(col, y))
            y0 = y
            while y < PANEL and zone_of(gsample(col, y)) == z:
                y += 1
            n = y - y0
            # zone in bits 5..2, the odd bits of start and length below them -- the block
            # header's layout, so one reader understands both.
            zstream.extend([((z & 15) << 2) | ((n >> 8) << 1) | (y0 >> 8),
                            y0 & 255, n & 255])
            zruns += 1
    zoffs.append(len(zstream))

    # WHERE EACH BAND BALANCES.
    #
    # The pass is split across the two cores, and a band costs whichever half is
    # slower, not the average. Splitting at the midpoint looked fair and was not: a
    # frame is not spread evenly across a band's 32 columns, so a band whose work sits
    # mostly on one side saved nothing at all. Measured, a midpoint split returned 1.2
    # of the 1.9 ms it should have.
    #
    # The weight is COST and not pixel count. A half holding many short edge blocks
    # costs more per pixel than one holding a few solid members, so counting pixels
    # balanced the wrong quantity -- see ITEM_W and LIT_W at the top.
    per_col = []
    for col in range(PANEL):
        c = col if not mirror else (col if col < cols else PANEL - 1 - col)
        per_col.append(col_cost[c])

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
        run = 0.0
        for r in range(1, band_h):
            run += rows[r - 1]
            gap = abs(run - (total - run))
            if best is None or gap < best:
                best, best_at = gap, r
        splits.append(best_at)

    items = n_flat + n_lit
    used = flat_px + lit_px
    kb = (len(stream) + len(offsets) * 2) / 1024.0

    with open(out, "w") as fh:
        fh.write(f"// GENERATED by tools/canopy_bake.py from {src} -- do not edit.\n")
        fh.write("//\n")
        fh.write(f"// {items} blocks ({n_flat} flat, {n_lit} literal), "
                 f"{used} pixels a half-frame, {kb:.1f} KB.\n")
        fh.write(f"// Background level {bg}: that value means no change.\n")
        fh.write("//\n")
        fh.write("// Columns of the picture. Each column is a list of blocks. A block is a\n")
        fh.write("// three byte header:\n")
        fh.write("//     byte 0   bit 7 literal, bit 6 subtract, bits 5..2 zone,\n")
        fh.write("//              bit 1 length bit 8, bit 0 start bit 8\n")
        fh.write("//     byte 1   start, low eight bits\n")
        fh.write("//     byte 2   length, low eight bits\n")
        fh.write("// followed by one grey level for the whole block, or by one level per\n")
        fh.write("// pixel if the literal bit is set.\n")
        fh.write("//\n")
        fh.write("// The zone is the activation order from the green channel, and the FLIGHT\n")
        fh.write("// table carries it only because it is free to store -- its runs are not\n")
        fh.write("// split at zone borders, so a block there may span two zones and the\n")
        fh.write("// renderer masks the field off. Use the INTRO table where it has to mean\n")
        fh.write("// something.\n")
        if mirror:
            fh.write("//\n// SYMMETRIC: the left half is stored and mirrored at draw time.\n")
        else:
            fh.write(f"//\n// ASYMMETRIC ({bad} pixels differ, worst {worst}), so every"
                     " column is stored.\n")
        fh.write("#pragma once\n#include <stdint.h>\n")
        # ONE DIRECTORY UP, because a baked header lives in src/vg/generated/ and the
        # struct it describes is hand-written code in src/vg/. Kept as an include rather
        # than left to the set header that pulls this in first: a header that cannot be
        # compiled on its own is a trap for whoever includes it second.
        fh.write('#include "../vg_canopy.h"\n\n')
        fh.write("// Where each band's WORK balances, so the two cores finish together.\n")
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

        # THE INTRO'S OWN TABLE, in the same format and read by the same walk.
        #
        # Identical to the one above except that a run stops at a zone border, so every
        # block belongs to exactly one zone and switching a zone on is switching a set of
        # whole blocks on. That costs blocks -- which is why the flight table does not pay
        # for it.
        fh.write(f"\n// {len(zones)} activation regions in the green channel.\n")
        fh.write(f"// {i_flat + i_lit} blocks, against the flight table's {items}.\n")
        fh.write("// The same drawing, with every run cut at its zone border.\n")
        fh.write(f"static const uint16_t {name}_IOFS[{len(i_offsets)}] = {{\n")
        for i in range(0, len(i_offsets), 16):
            fh.write("    " + ",".join(str(v) for v in i_offsets[i:i + 16]) + ",\n")
        fh.write("};\n\n")
        fh.write(f"static const uint8_t {name}_IDATA[{len(i_stream)}] = {{\n")
        for i in range(0, len(i_stream), 24):
            fh.write("    " + ",".join(str(v) for v in i_stream[i:i + 24]) + ",\n")
        fh.write("};\n")

        # THE ZONE MAP, which is about the WORLD and not about the frame.
        #
        # Every pixel of the screen has a zone, including the 90% the cockpit does not
        # cover. That is what the intro holds black and releases a region at a time, and
        # the block table cannot answer it: the block table only knows where the drawing is.
        #
        # Three bytes a run, no payload, same header layout so one reader serves both.
        fh.write(f"\n// {zruns} runs, covering every pixel of the screen.\n")
        fh.write("// Every pixel of the screen, by activation zone. Runs down a column,\n")
        fh.write("// three bytes each: zone in bits 5..2, then start and length.\n")
        fh.write(f"static const uint16_t {name}_ZOFS[{len(zoffs)}] = {{\n")
        for i in range(0, len(zoffs), 16):
            fh.write("    " + ",".join(str(v) for v in zoffs[i:i + 16]) + ",\n")
        fh.write("};\n\n")
        fh.write(f"static const uint8_t {name}_ZDATA[{len(zstream)}] = {{\n")
        for i in range(0, len(zstream), 24):
            fh.write("    " + ",".join(str(v) for v in zstream[i:i + 24]) + ",\n")
        fh.write("};\n")

        # THE DRAWING AS ONE OBJECT, which is what makes a second cockpit possible.
        #
        # The three tables and the numbers describing them used to be loose arrays and CANOPY_*
        # macros. A macro cannot be chosen at runtime, so that shape allowed exactly one canopy
        # to exist -- selecting per hull means the whole drawing has to be a thing that can be
        # pointed at.
        fh.write("\n// The drawing, as one object. Hand it to vg_canopy_use.\n")
        fh.write(f"static const VgCanopy {name} = {{\n")
        fh.write(f"    {name}_OFS, {name}_DATA,\n")
        fh.write(f"    {name}_IOFS, {name}_IDATA,\n")
        fh.write(f"    {name}_ZOFS, {name}_ZDATA,\n")
        fh.write(f"    {name}_SPLIT,\n")
        fh.write(f"    {cols}, {bg}, {1 if mirror else 0}, {len(zones)},\n")
        fh.write(f"    {items}, {flat_px}, {lit_px},\n")
        fh.write("};\n")

    print(f"{src}: background {bg}, {items} blocks, {kb:.1f} KB -> {out}")
    print("  symmetric, stored as a half" if mirror
          else f"  asymmetric ({bad} px differ, worst {worst}), stored whole")
    print(f"  {n_flat} flat blocks carrying {flat_px} px, "
          f"{n_lit} literal blocks carrying {lit_px} px")
    ikb = (len(i_stream) + len(i_offsets) * 2) / 1024.0
    print(f"  flight table {n_flat + n_lit} blocks; intro table {i_flat + i_lit} blocks, "
          f"+{ikb:.1f} KB of flash and no frame time")
    zkb = (len(zstream) + len(zoffs) * 2) / 1024.0
    print(f"  zone map {zruns} runs, {zkb:.1f} KB -- every pixel of the screen, "
          f"{zruns / float(PANEL):.1f} runs a column")
    print(f"  {len(zones)} activation zones from the green channel, in order: "
          + ", ".join(str(v) for v in zones))
    for i, z in enumerate(zones):
        print(f"     {i}: green {z:3d}, {zhist.get(z, 0):6d} px")

    # WHAT IT COSTS, which is the number to watch while drawing.
    #
    # The three constants below are FITTED to a device measurement of this encoding,
    # not derived. An earlier version of this estimate reasoned about the instruction
    # count instead and was wrong by 1.7x, which is worse than no estimate at all --
    # it would have had the author drawing into a budget that was not there.
    #
    # Refit them if the encoding or the inner loop changes. Two bakes with different
    # block-to-pixel ratios, each flown and read off the `can` counter, give three
    # numbers from two equations well enough for this purpose.
    scaled = 2 if mirror else 1
    cyc = (items * A_ITEM + flat_px * A_FLAT + lit_px * A_LIT) * scaled
    pass_us = cyc / 240.0
    whole = used * scaled
    # The pass is halved across the two cores, so the frame does not pay all of it. It
    # pays two thirds, not one half: measured, 4.72 ms of pass billed the frame 3.15 ms.
    # The rest goes on the band that finishes late and on the rendezvous. Reporting the
    # pass as though it were the frame cost would overstate the charge by half.
    us = pass_us * SPLIT_YIELD
    print(f"  {whole} pixels a frame, {whole * 100.0 / (PANEL * PANEL):.1f}% of the screen")
    print(f"  {pass_us / 1000.0:.2f} ms of drawing, halved across the cores")
    print(f"  costs the frame about {us / 1000.0:.2f} ms")
    # GRADED AGAINST THE BUDGET, not against a guess at the frame rate. The old grades
    # named frame rates they had never measured -- "expect to lose 60", "drops to about
    # 50" -- and were wrong about two drawings running.
    pct = 100.0 * us / CANOPY_BUDGET_US
    print(f"  {pct:.0f}% of the {CANOPY_BUDGET_US / 1000.0:.1f} ms the cockpit may have")
    if us > CANOPY_BUDGET_US:
        print("  TOO HEAVY: over budget, so the raster sets the frame instead of the wire")
        print("     and every microsecond of this lands on the frame. Narrow the shapes:")
        print("     area is the cost, and levels and detail are free.")
    elif pct > 80.0:
        print("  HEAVY: close to the ceiling, with nothing left for anything else that grows.")
    elif pct > 50.0:
        print("  worth watching, but there is room for it")
    else:
        print("  cheap")


# Cycles, at 240 MHz. Fitted to three device measurements of the shipping loop: the
# normal bake, one with every run stored flat, and one with every pixel storing its
# own level. Those three give three equations for these three numbers.
#
# The shape of them is the useful part. A pixel costs about 37 cycles and a header
# about 32, so a header is worth less than one pixel and 95% of the bill is pixels.
# There is nothing left to win in the encoding: AREA is the only lever, which is the
# author's, not the compiler's.
# MUST MATCH VG_CANOPY_MAX_ZONES in src/vg/vg_canopy.h. The firmware keeps a 256-entry colour
# table per activation region so each can run its own glow, and internal SRAM is the scarce
# memory on this part -- not flash. Eight of them is 4 KB of it.
MAX_ZONES = 8

# Set by --name=. A list so the argument loop can write to it.
NAME = ["CANOPY"]

A_ITEM = 31.6        # decoding one block header
A_FLAT = 36.3        # one pixel of a flat block
A_LIT = 39.3         # one pixel that carries its own level

# How much of the pass the frame actually pays, the two cores having shared it. Measured
# rather than assumed to be a half: the band waits for its slower side and then for the
# rendezvous. Re-measure it against the `can` counter after a flight if the split changes.
#
# 0.59 read in a match on 2026-08-04: 4028 us on the bench billed the frame 2361. The
# earlier 0.67 came from the old encoding and overstated the charge -- fewer blocks means
# less per-band overhead outside the split, so more of the pass halves cleanly.
# WHAT SHARE OF THE ONE-CORE PASS THE FRAME ACTUALLY PAYS.
#
# 0.59 until it was measured. Two drawings replayed over the same recorded session, and
# the `can` counter read off the device rather than guessed at:
#
#     10.5% of the screen   pass 4060 us   frame paid 1855   ->  0.457
#     16.6% of the screen   pass 6460 us   frame paid 2799   ->  0.433
#
# 0.445 fits both to within 3%. The old 0.59 overstated every drawing by a third, which
# is how two drawings in a row were graded TOO HEAVY while one of them ran at 60 to 62
# frames a second and the other had 2.1 ms of headroom to spare.
#
# Refit it with tools/replay_cost.py, which is the only measurement that can: the `can`
# counter was watched swinging 2.8x between moments of one live fight, so a figure taken
# by flying compares two moments rather than two drawings.
SPLIT_YIELD = 0.445

# WHAT THE COCKPIT IS ALLOWED TO COST, in microseconds of the frame.
#
# Not an opinion about frame rate. The wire needs 11520 us to send a frame and the frame
# cannot beat it, so raster work under that ceiling is largely absorbed by the transfer
# wait and work above it lands on the frame in full. Measured over the regression session,
# everything in the raster EXCEPT the cockpit comes to about 6600 us -- 8448 minus 1855 on
# one drawing, 9414 minus 2799 on the other, which agree to 22 us.
#
# So the cockpit may have what is left. The grades below are fractions of it.
CANOPY_BUDGET_US = 11520.0 - 6600.0


if __name__ == "__main__":
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    for a in sys.argv[1:]:
        # MINFLAT, for fitting the cost constants above. 1 stores every run as a flat
        # block and 0 stores every pixel as a literal; those two extremes and the
        # normal bake give three measurements for the three constants. Not for
        # ordinary use -- the default is the setting that is actually fast.
        if a.startswith("--minflat="):
            MINFLAT = int(a.split("=", 1)[1])
        # The C name of the object the header defines. One per drawing, because the
        # firmware holds them all at once and selects per ship.
        if a.startswith("--name="):
            NAME[0] = a.split("=", 1)[1]
    if len(argv) < 2:
        sys.exit(__doc__)
    bake(argv[0], argv[1], argv[2] if len(argv) > 2 else NAME[0])
