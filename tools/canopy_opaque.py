#!/usr/bin/env python3
"""EXPERIMENT: bake a canopy as OPAQUE colour plus an ADDITIVE outline.

    python tools/canopy_opaque.py design/canopy/chariot_test.png \\
           src/vg/generated/canopy_opaque_chariot.h --name=CANOPY_OP_CHARIOT

This is not how a canopy has been drawn until now. tools/canopy_bake.py treats a
drawing as a signed light DELTA over the finished picture -- the frame lights
whatever is behind it -- and this treats most of it as METAL, which replaces the
pixel, with only a thin outline still adding light.

WHAT THE DRAWING SAYS, by colour, which is the console chassis's rule and not the
canopy's:

    magenta   nothing is stored. The world shows through, exactly as the screen
              aperture does in a bezel.
    cyan      an ADDITIVE run. No colour is stored for it: every pixel takes the
              same HUD amber, added to whatever is behind.
    anything  OPAQUE. The pixel is stored as a palette index and replaces what
    else      was there.

...and the ALPHA channel carries the arrival sequence, one flat value per region.
It used to be the green channel, which a full-colour drawing no longer has spare.

A span carries its zone, so the arrival can gate whole runs rather than testing
per pixel -- and a run is never allowed to straddle two zones, which is the one
thing the flight table in the old baker deliberately does not bother with.
"""
import sys

from PIL import Image
import numpy as np

PANEL = 480

OPAQUE = 0
ADDITIVE = 1


def load(src, colors):
    """Read the drawing. Return the palette, indices, the two masks and zones."""
    im = Image.open(src)
    if im.mode != "RGBA":
        sys.exit("%s has no alpha channel. The arrival sequence lives there --\n"
                 "save as RGBA (PNG colour type 6) with one flat value a region."
                 % src)

    rgba = np.asarray(im).astype(int)
    r, g, b, a = rgba[:, :, 0], rgba[:, :, 1], rgba[:, :, 2], rgba[:, :, 3]

    # BY HUE, NOT BY LEVEL, the same test the bezel baker uses: ask whether the
    # two channels that define the hue both stand clear of the third, and
    # brightness stops mattering.
    hit = {
        "exempt": (np.minimum(r, b) - g) > 40,      # magenta: the world
        "add":    (np.minimum(g, b) - r) > 40,      # cyan: the lit outline
    }

    def grow(m, n=2):
        """Swallow the smoothed edge, so a marked area does not fray into art.

        HOW MUCH DEPENDS ON WHAT IS BEING MARKED. A magenta pane is a big flat
        area whose edge is the only part that blends, so two pixels of growth
        costs it nothing and cleans the boundary. The cyan outline IS an edge --
        one or two pixels wide, drawn deliberately at that weight -- and growing
        it by two each side makes it five, which is not the drawing any more. It
        came out 3.6 times its drawn area before this took an argument.
        """
        e = m.copy()
        for _ in range(n):
            e[1:, :] |= e[:-1, :]; e[:-1, :] |= e[1:, :]
            e[:, 1:] |= e[:, :-1]; e[:, :-1] |= e[:, 1:]
        return e

    def shrink(m, n=2):
        # NEAREST. A smooth reduction blends the marker paint with the metal
        # beside it and invents pixels that are neither.
        return np.asarray(Image.fromarray((grow(m, n) * 255).astype(np.uint8))
                          .resize((PANEL, PANEL), Image.NEAREST)) > 127

    # THE OUTLINE WINS THE OVERLAP, and it has to.
    #
    # grow() spreads a marked area by four pixels to swallow its own smoothed
    # edge, which is right where magenta meets metal and wrong where it meets the
    # cyan line -- that line is one or two pixels wide and sits exactly on the
    # boundary, so a grown magenta eats the whole of it. The first bake came out
    # with 45,591 opaque pixels and ZERO additive ones.
    #
    # So cyan is taken first and magenta is what is left. The outline keeps its
    # own grown edge; the pane gives up the pixels it would have stolen.
    add    = shrink(hit["add"], 0)
    exempt = shrink(hit["exempt"]) & ~add

    # THE ZONES, from alpha. The drawing holds a handful of flat values; each
    # distinct one is a region and they arrive in ascending order. Reduced with
    # NEAREST for the same reason the masks are -- an averaged edge between two
    # regions is a third region that was never drawn.
    az = np.asarray(Image.fromarray(a.astype(np.uint8))
                    .resize((PANEL, PANEL), Image.NEAREST)).astype(int)
    vals, counts = np.unique(az, return_counts=True)
    keep = sorted(int(v) for v, c in zip(vals, counts) if c > az.size // 500)
    if not keep:
        sys.exit("alpha holds no flat regions -- nothing to sequence.")
    # Everything else is an antialiased boundary: snap it to the nearest kept
    # level rather than inventing a zone for it.
    zone = np.zeros_like(az, dtype=np.uint8)
    for y in range(PANEL):
        row = az[y]
        d = np.abs(row[:, None] - np.array(keep)[None, :])
        zone[y] = np.argmin(d, axis=1).astype(np.uint8)

    art = im.convert("RGB").resize((PANEL, PANEL), Image.LANCZOS)
    pal_im = art.quantize(colors=colors, method=Image.MEDIANCUT,
                          dither=Image.Dither.NONE)
    idx = np.asarray(pal_im).astype(np.uint8)

    pal = pal_im.getpalette()[:colors * 3]
    pal565 = []
    for i in range(colors):
        rr, gg, bb = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        # PRE-SWAPPED, like every colour in the game -- see cfg_palette.h.
        c = ((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3)
        pal565.append(((c >> 8) & 0x00FF) | ((c << 8) & 0xFF00))

    err = np.abs(np.asarray(pal_im.convert("RGB")).astype(int)
                 - np.asarray(art).astype(int))[~exempt]
    return pal565, idx, exempt, add, zone, len(keep), err


def to_panel(a):
    """Picture space to PANEL rows. The glass is mounted a quarter turn, so the
    firmware reads straight lines and the rotation is done once, here.

    ONE QUARTER TURN, THE SAME WAY THE BEZEL BAKER TURNS. This was three, which
    is the same axis and the opposite direction -- a hundred and eighty degrees
    out, which on a cockpit that is very nearly symmetrical about its vertical
    does not look like a rotation at all. It looks like the canopy is upside
    down, which is what it was reported as.
    """
    return np.rot90(a, 1)


def spans_of(exempt_p, add_p, zone_p):
    """Every stored run, in panel row order. A run breaks on a change of kind or
    of zone, so the renderer never tests either per pixel."""
    spans = []
    rows = [0] * (PANEL + 1)
    for y in range(PANEL):
        rows[y] = len(spans)
        x = 0
        while x < PANEL:
            if exempt_p[y, x]:
                x += 1
                continue
            kind = ADDITIVE if add_p[y, x] else OPAQUE
            z = int(zone_p[y, x])
            x0 = x
            while (x < PANEL and not exempt_p[y, x]
                   and (ADDITIVE if add_p[y, x] else OPAQUE) == kind
                   and int(zone_p[y, x]) == z):
                x += 1
            spans.append((x0, x - x0, kind, z))
    rows[PANEL] = len(spans)
    return spans, rows


def zone_runs(zone_p):
    """The region map, run-length by panel row.

    EVERY PIXEL, not just the stored ones. This is not the drawing -- it is WHICH
    REGION OF THE VIEW a pixel belongs to, and both things that act on a region act
    on the whole of it: the arrival holds a region black and dissolves the world out
    of it, and a round through the canopy takes a region to white and then to static.
    Both have to cover the PANES, which the span table deliberately does not store.

    The delta canopy carries the same thing in zofs/zdata, for the same reasons. It
    is a separate list rather than a flag on a span because the two are read at
    different moments: the span table moves with the frame and this does not.
    """
    runs = []
    rows = [0] * (PANEL + 1)
    for y in range(PANEL):
        rows[y] = len(runs)
        x = 0
        while x < PANEL:
            z = int(zone_p[y, x])
            x0 = x
            while x < PANEL and int(zone_p[y, x]) == z:
                x += 1
            runs.append((x0, x - x0, z))
    rows[PANEL] = len(runs)
    return runs, rows


def col(vals, per, fmt):
    out = []
    for i in range(0, len(vals), per):
        out.append("    " + ",".join(fmt % v for v in vals[i:i + per]) + ",")
    return "\n".join(out)


def write(path, name, pal, idx_p, spans, rows, data, nzone, stats,
          zruns, zrows, centre):
    with open(path, "w") as fh:
        fh.write("// GENERATED by tools/canopy_opaque.py -- do not edit.\n"
                 "//\n"
                 "// An EXPERIMENT: the canopy as opaque colour plus an additive\n"
                 "// outline, baked the way the console chassis is rather than as\n"
                 "// the signed light delta a canopy has always been.\n"
                 "//\n"
                 "// %d spans over %d panel rows, %d opaque pixels stored,\n"
                 "// %d additive pixels, %d zones. %.1f KB of flash.\n"
                 % (len(spans), PANEL, len(data), stats["add_px"], nzone,
                    stats["kb"]))
        fh.write("#pragma once\n#include <stdint.h>\n"
                 '#include "../vg_canopy_op.h"\n\n')

        fh.write("static const uint16_t %s_PAL[%d] = {\n%s\n};\n\n"
                 % (name, len(pal), col(pal, 8, "0x%04x")))
        fh.write("static const uint8_t %s_DATA[%d] = {\n%s\n};\n\n"
                 % (name, len(data), col(data, 16, "%3d")))

        fh.write("static const VgCanOpSpan %s_SPAN[%d] = {\n" % (name, len(spans)))
        for (x0, ln, kind, z, off) in spans:
            fh.write("    { %3d, %3d, %6d, %d, %d },\n" % (x0, ln, off, z, kind))
        fh.write("};\n\n")

        fh.write("static const uint16_t %s_ROW[%d] = {\n%s\n};\n\n"
                 % (name, PANEL + 1, col(rows, 16, "%5d")))

        fh.write("static const VgCanOpZone %s_ZONE[%d] = {\n"
                 % (name, len(zruns)))
        for (x0, ln, z) in zruns:
            fh.write("    { %3d, %3d, %d },\n" % (x0, ln, z))
        fh.write("};\n\n")

        fh.write("static const uint16_t %s_ZROW[%d] = {\n%s\n};\n\n"
                 % (name, PANEL + 1, col(zrows, 16, "%5d")))

        fh.write("static const VgCanOp %s = {\n"
                 "    %s_PAL, %s_DATA, %s_SPAN, %s_ROW,\n"
                 "    %s_ZONE, %s_ZROW,\n"
                 "    %d, %d, %d, %d, %d,\n};\n"
                 % (name, name, name, name, name, name, name,
                    len(spans), len(zruns), len(data), nzone, centre))


def main(argv):
    name = "CANOPY_OP"
    colors = 256
    args = []
    for a in argv:
        if a.startswith("--name="):
            name = a.split("=", 1)[1]
        elif a.startswith("--colors="):
            colors = int(a.split("=", 1)[1])
        else:
            args.append(a)
    if len(args) < 2:
        sys.exit(__doc__)

    pal, idx, exempt, add, zone, nzone, err = load(args[0], colors)

    idx_p    = to_panel(idx)
    exempt_p = to_panel(exempt)
    add_p    = to_panel(add)
    zone_p   = to_panel(zone)

    raw, rows = spans_of(exempt_p, add_p, zone_p)

    # An opaque run stores one palette index a pixel; an additive one stores
    # nothing at all, because every pixel of it takes the same colour.
    data = []
    spans = []
    add_px = 0
    si = 0
    for y in range(PANEL):
        for _ in range(rows[y], rows[y + 1]):
            x0, ln, kind, z = raw[si]; si += 1
            if kind == OPAQUE:
                off = len(data)
                data.extend(int(v) for v in idx_p[y, x0:x0 + ln])
                spans.append((x0, ln, kind, z, off))
            else:
                add_px += ln
                spans.append((x0, ln, kind, z, 0))

    zruns, zrows = zone_runs(zone_p)
    # WHICH REGION IS BEING LOOKED THROUGH, so vg_canopy_damage can refuse to fail
    # it -- taking out the middle of the view is not atmosphere, it is a blindfold.
    # Read at the middle of the panel rather than by area, because what makes a
    # region central is that the pilot is aiming through it.
    centre = int(zone_p[PANEL // 2, PANEL // 2])

    kb = (len(data) + len(spans) * 12 + len(zruns) * 6
          + (PANEL + 1) * 4 + len(pal) * 2) / 1024.0
    stats = {"add_px": add_px, "kb": kb}

    print("  %s: %d x %d, %d colours, %d zones"
          % (args[0], PANEL, PANEL, colors, nzone))
    print("  exempt %.1f%% of the panel" % (100.0 * exempt_p.mean()))
    print("  opaque %d px, additive %d px, %d spans (%.1f a row)"
          % (len(data), add_px, len(spans), len(spans) / float(PANEL)))
    print("  colour error: mean %.2f, 99%% within %.0f"
          % (err.mean(), np.percentile(err, 99)))
    print("  zone map: %d runs (%.1f a row), centre region %d"
          % (len(zruns), len(zruns) / float(PANEL), centre))
    print("  %.1f KB of flash" % kb)

    if len(args) > 1 and args[1] != "--report":
        write(args[1], name, pal, idx_p, spans, rows, data, nzone, stats,
              zruns, zrows, centre)
        print("  wrote %s" % args[1])


main(sys.argv[1:])
