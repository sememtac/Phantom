#!/usr/bin/env python3
"""Bake a canopy as OPAQUE colour plus an ADDITIVE outline.

    python tools/canopy_opaque.py design/canopy/chariot_opaque.png \\
           src/vg/generated/canopy_op_chariot.h --name=CANOPY_OP_CHARIOT

Normally run by tools/canopy_set.py, which bakes every <hull>_opaque.png in
design/canopy/ and writes the wiring.

tools/canopy_bake.py treats a drawing as a signed light DELTA over the finished
picture -- the frame lights whatever is behind it. This treats most of it as
METAL, which replaces the pixel, with only a thin outline still adding light. A
hull with a drawing of this kind flies it; the delta stays for hulls without one,
and its drawing is still needed beside this one for the frame's bend and split.

--tint=<mask.png> marks the metal that takes the player's colour. White is
painted, black is bare. The paint is applied to the PALETTE at runtime, not to
the pixels, so it costs nothing per pixel -- which is only true if no palette
entry is shared between painted and bare metal, and that is what this option
makes the baker guarantee.

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


def quantise(pixels, k):
    """Median-cut an (N, 3) list of pixels into k colours. Returns the palette and
    one index a pixel. A list rather than a picture, because the two groups below
    are not rectangles."""
    if len(pixels) == 0:
        return np.zeros((k, 3), int), np.zeros(0, np.uint8)
    w = min(len(pixels), 4096)
    h = (len(pixels) + w - 1) // w
    buf = np.zeros((h * w, 3), np.uint8)
    buf[:len(pixels)] = pixels
    q = Image.fromarray(buf.reshape(h, w, 3)).quantize(
        colors=k, method=Image.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:k * 3], int).reshape(k, 3)
    idx = np.asarray(q).reshape(-1)[:len(pixels)].astype(np.uint8)
    return pal, idx


def split_palette(art, stored, painted, colors):
    """Two groups of pixels, two ranges of one palette, and NO ENTRY SHARED.

    That last part is the whole trick. The renderer reads every metal pixel as
    spal[index], so if the painted metal owns its own entries, the player's colour
    is applied by rewriting those entries once -- not by touching a single pixel.
    Baked as it is here, that is free; it cannot be done to a palette whose entries
    are shared, and a plain median cut over the drawing shares 142 of them.

    THE PAINTED RANGE COMES FIRST, [0, k), because the firmware walks it as a count
    rather than a pair of bounds. See tint_n in VgCanOp.

    Entries are split in proportion to the pixels, with a floor under each side so
    that a small painted area still gets enough colours to hold its shading.

    It also fits BETTER than the bake it replaces, which is not a coincidence: the
    old one quantised the whole drawing, so most of the palette was fitted to the
    magenta panes, which are never stored. Only 146 of 256 entries were reachable.
    """
    hot  = stored & painted
    cold = stored & ~painted
    n_hot, n_all = int(hot.sum()), int(stored.sum())
    if n_hot == 0 or n_hot == n_all:
        sys.exit("the tint mask covers %s of the stored pixels -- nothing to split."
                 % ("none" if n_hot == 0 else "all"))
    k = int(round(colors * float(n_hot) / float(n_all)))
    k = max(32, min(colors - 32, k))

    pal_h, idx_h = quantise(art[hot],  k)
    pal_c, idx_c = quantise(art[cold], colors - k)

    idx = np.zeros(art.shape[:2], np.uint8)
    idx[hot]  = idx_h
    idx[cold] = (idx_c.astype(int) + k).astype(np.uint8)

    err = np.concatenate([np.abs(pal_h[idx_h] - art[hot]).ravel(),
                          np.abs(pal_c[idx_c] - art[cold]).ravel()])
    return np.concatenate([pal_h, pal_c]), idx, k, err


def load(src, colors, tint=None):
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

    if tint:
        # THE MASK IS ITS OWN FILE, and it has to be: the drawing's four channels are
        # spent -- three on the paint and the alpha on the arrival -- so there is
        # nowhere left to swizzle this into.
        mk = Image.open(tint).convert("L")
        if mk.size != im.size:
            print("  note: the tint mask is %dx%d and the drawing is %dx%d"
                  % (mk.size[0], mk.size[1], im.size[0], im.size[1]))
        mkp = np.asarray(mk.resize((PANEL, PANEL), Image.LANCZOS)).astype(int)
        # A HARD EDGE, and that is a decision rather than a shortcut. The mask is
        # drawn with soft boundaries, and about 5% of the metal sits on one; a paint
        # edge in the world is crisp, and giving those pixels their own half-painted
        # palette range was measured at 0.92 mean error against 0.86 for the hard cut.
        painted = mkp > 127
        stored  = ~exempt & ~add
        pal_rgb, idx, tint_n, err = split_palette(np.asarray(art).astype(int),
                                                  stored, painted, colors)
    else:
        pal_im = art.quantize(colors=colors, method=Image.MEDIANCUT,
                              dither=Image.Dither.NONE)
        idx = np.asarray(pal_im).astype(np.uint8)
        pal_rgb = np.array(pal_im.getpalette()[:colors * 3], int).reshape(colors, 3)
        tint_n = 0
        # OVER THE STORED PIXELS, which is what the split path measures too. This
        # was over ~exempt, so it included the additive outline -- pixels whose
        # colour is never stored and never read from the palette. The two paths
        # printed the same label for two different numbers, 0.96 against 0.86,
        # and the difference was the denominator rather than the bake.
        err = np.abs(np.asarray(pal_im.convert("RGB")).astype(int)
                     - np.asarray(art).astype(int))[~exempt & ~add]

    pal565 = []
    for i in range(colors):
        rr, gg, bb = pal_rgb[i]
        # PRE-SWAPPED, like every colour in the game -- see cfg_palette.h.
        c = ((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3)
        pal565.append(((c >> 8) & 0x00FF) | ((c << 8) & 0xFF00))

    return pal565, idx, exempt, add, zone, len(keep), err, tint_n


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
          zruns, zrows, centre, tint_n):
    with open(path, "w") as fh:
        fh.write("// GENERATED by tools/canopy_opaque.py -- do not edit.\n"
                 "//\n"
                 "// The canopy as opaque colour plus an additive outline, baked the\n"
                 "// way the console chassis is rather than as a signed light delta.\n"
                 "//\n"
                 "// %d spans over %d panel rows, %d opaque pixels stored,\n"
                 "// %d additive pixels, %d zones. %.1f KB of flash.\n"
                 % (len(spans), PANEL, len(data), stats["add_px"], nzone,
                    stats["kb"]))
        if tint_n:
            fh.write("//\n"
                     "// Palette entries 0 to %d take the player's colour: they are used\n"
                     "// by painted metal and by nothing else. See tint_n in VgCanOp.\n"
                     % (tint_n - 1))
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
                 "    %d, %d, %d, %d, %d, %d,\n};\n"
                 % (name, name, name, name, name, name, name,
                    len(spans), len(zruns), len(data), nzone, centre, tint_n))


def main(argv):
    name = "CANOPY_OP"
    colors = 256
    tint = None
    args = []
    for a in argv:
        if a.startswith("--name="):
            name = a.split("=", 1)[1]
        elif a.startswith("--colors="):
            colors = int(a.split("=", 1)[1])
        elif a.startswith("--tint="):
            tint = a.split("=", 1)[1]
        else:
            args.append(a)
    if len(args) < 2:
        sys.exit(__doc__)

    pal, idx, exempt, add, zone, nzone, err, tint_n = load(args[0], colors, tint)

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
    if tint_n:
        n_hot = int(((idx_p < tint_n) & ~exempt_p & ~add_p).sum())
        print("  the player's colour: %d px on %d palette entries, bare metal on %d"
              % (n_hot, tint_n, colors - tint_n))
    print("  zone map: %d runs (%.1f a row), centre region %d"
          % (len(zruns), len(zruns) / float(PANEL), centre))
    print("  %.1f KB of flash" % kb)

    if len(args) > 1 and args[1] != "--report":
        write(args[1], name, pal, idx_p, spans, rows, data, nzone, stats,
              zruns, zrows, centre, tint_n)
        print("  wrote %s" % args[1])


main(sys.argv[1:])
