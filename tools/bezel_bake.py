#!/usr/bin/env python3
"""Turn a console drawing into a table the firmware can draw.

    python tools/bezel_bake.py design/selection/SelectionConcept2.png \\
        src/vg/generated/bezel_console.h --name=BEZEL_CONSOLE

A bezel is the metal chassis around a screen. The firmware draws it as opaque
pixels, so it hides what is behind it. This is not a canopy. A canopy adds light
to the picture and a bezel replaces it, so the two use different tools.

MAGENTA MARKS THE EXEMPT AREA. Paint every part of the drawing that the game
fills itself in pure magenta (255, 0, 255). The baker stores no pixel there, and
the firmware draws no pixel there. The screen aperture and the two bar windows
are exempt, because the menu draws in them.

The table holds PANEL ROWS. The display is turned a quarter turn, so the baker
rotates the drawing once and the firmware reads straight lines. Each panel row
holds up to a few spans, and a span is one run of pixels between exempt areas.

Colours are reduced to a palette of 256. The drawing is dark metal with a small
number of light accents, so the error is about one level per channel.

Options:
    --name=NAME     the C name of the table. Default BEZEL_CONSOLE.
    --colors=N      palette size, 2 to 256. Default 256.
    --report        print the cost and write no file.
"""
import sys

from PIL import Image
import numpy as np

PANEL = 480          # the display is 480 x 480

# The jobs a painted area can have. The numbers are the firmware's enum, so they
# are part of the format and not a detail of this script.
SLOT     = 0         # magenta: the game draws here and decides what
HEADLINE = 1         # cyan:    the ticker runs across here, clipped to it

ROLE_NAME = { SLOT: "SLOT", HEADLINE: "HEADLINE" }


def load(src, colors):
    """Read the drawing. Return the palette, the indices, and the exempt mask."""
    rgb = Image.open(src).convert("RGB")
    a = np.asarray(rgb).astype(int)

    # ROLES COME FROM COLOUR. The artist paints what the game draws, and paints it
    # in the colour of the JOB it does:
    #
    #   MAGENTA  a slot. The game draws in it and decides what.
    #   CYAN     the headline. The ticker runs across it, clipped to it.
    #
    # This used to be one colour and the roles were worked out from geometry --
    # largest region is the screen, then sort the rest by position -- which is
    # inference dressed up as a rule. It got the two bar windows the wrong way
    # round on the first drawing, because they differ in area by three hundred
    # pixels and the sort was by area. The drawing knows what each hole is for;
    # it should say so rather than be guessed at.
    #
    # BY HUE, NOT BY LEVEL. A test on channel levels lets (255, 130, 255)
    # through as not-magenta, which it plainly is. Ask instead whether the two
    # channels that define the hue both stand clear of the third, and brightness
    # stops mattering.
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    role_hit = {
        SLOT:     (np.minimum(r, b) - g) > 40,      # magenta
        HEADLINE: (np.minimum(g, b) - r) > 40,      # cyan
    }

    # GROWN BY TWO PIXELS, each role on its own. The edge of a painted area is
    # smoothed against the metal beside it, so a ring of part-coloured pixels sits
    # just outside any test. Widening the test only moves the ring; growing the
    # area swallows it, at the cost of two pixels of metal the game draws over.
    def grow(m):
        for _ in range(2):
            e = m.copy()
            e[1:, :] |= m[:-1, :]
            e[:-1, :] |= m[1:, :]
            e[:, 1:] |= m[:, :-1]
            e[:, :-1] |= m[:, 1:]
            m = e
        return m

    # Reduced with NEAREST. A smooth reduction blends the marker paint with the
    # metal beside it and invents pixels that are neither.
    def shrink(m):
        return np.asarray(Image.fromarray((grow(m) * 255).astype(np.uint8))
                          .resize((PANEL, PANEL), Image.NEAREST)) > 127

    roles = {k: shrink(v) for k, v in role_hit.items()}
    mask  = np.zeros((PANEL, PANEL), bool)
    for m in roles.values():
        mask |= m

    art = rgb.resize((PANEL, PANEL), Image.LANCZOS)

    # Quantise the whole drawing. The exempt area takes one palette entry and
    # that entry is never stored, which costs one colour of the 256.
    pal_im = art.quantize(colors=colors, method=Image.MEDIANCUT,
                          dither=Image.Dither.NONE)
    idx = np.asarray(pal_im).astype(np.uint8)

    pal = pal_im.getpalette()[:colors * 3]
    pal565 = []
    for i in range(colors):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        # PRE-SWAPPED, like every other colour in the game. The display takes
        # RGB565 big-endian, so cfg_palette.h stores entries with the bytes
        # already exchanged and the raster copies them out untouched. A palette
        # written in the natural order draws the right picture in the wrong
        # colours: the low bits of blue land in the high bits of red.
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        pal565.append(((c >> 8) & 0x00FF) | ((c << 8) & 0xFF00))

    err = np.abs(np.asarray(pal_im.convert("RGB")).astype(int)
                 - np.asarray(art).astype(int))[~mask]
    rgb3 = [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for i in range(colors)]
    return pal565, rgb3, idx, mask, roles, err


def regions(roles):
    """Every painted area, with the job its colour gives it.

    Returns a list of (role, inner, box) in READING ORDER within each role --
    top to bottom, then left to right. That is the order a screen refers to them
    in, so a drawing with three keys along the bottom hands them over left to
    right without anybody having to say which is which.

    Two rectangles per area, because they answer different questions. `inner` is
    the largest rectangle that fits INSIDE the area and is what a hit test and a
    layout want. `box` is the full extent, chamfered corners included, and is
    what a fill and a clip want: filling only the inner rectangle leaves the
    corners unpainted, and clipping to it cuts a moving label short of the glass.
    """
    out = []
    for role, mask in roles.items():
        seen = np.zeros(mask.shape, np.int32)
        tag = 0
        for sy in range(PANEL):
            for sx in range(PANEL):
                if not mask[sy, sx] or seen[sy, sx]:
                    continue
                tag += 1
                stack = [(sy, sx)]
                seen[sy, sx] = tag
                while stack:
                    y, x = stack.pop()
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < PANEL and 0 <= nx < PANEL                                 and mask[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = tag
                            stack.append((ny, nx))
                m = seen == tag
                ys, xs = np.nonzero(m)
                box = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
                out.append((role, max_rect(m), box))

    # Reading order within a role. Rows first, and two areas count as the same row
    # when they overlap vertically -- a line of keys along the bottom is one row
    # however their tops happen to land.
    def key(e):
        _, _, (x0, y0, _, _) = e
        return (y0 // 24, x0)
    out.sort(key=lambda e: (e[0], key(e)))
    return out


def max_rect(m):
    """The largest axis-aligned rectangle that fits inside a mask.

    Runs the largest-rectangle-in-a-histogram scan down the rows. Returns
    x0, y0, x1, y1, with all four inside the rectangle.
    """
    best = (0, 0, 0, 0, 0)
    h = np.zeros(PANEL, np.int32)
    for y in range(PANEL):
        h = np.where(m[y], h + 1, 0)
        stack = []
        for x in range(PANEL + 1):
            cur = int(h[x]) if x < PANEL else 0
            start = x
            while stack and stack[-1][1] >= cur:
                sx, sh = stack.pop()
                area = sh * (x - sx)
                if area > best[0]:
                    best = (area, sx, y - sh + 1, x - 1, y)
                start = sx
            stack.append((start, cur))
    return best[1:]


def to_panel(a):
    """Rotate the drawing into the order the display is scanned in.

    The panel is mounted a quarter turn from the drawing. A frame that is
    written straight out of this array needs no rotation at draw time.
    """
    return np.rot90(a, 1)


def spans_of(mask_p):
    """Find the runs of stored pixels in each panel row."""
    out = []
    for y in range(PANEL):
        row = mask_p[y]
        x = 0
        while x < PANEL:
            if not row[x]:
                x0 = x
                while x < PANEL and not row[x]:
                    x += 1
                out.append((y, x0, x - x0))
            else:
                x += 1
    return out


def write(path, name, pal, idx_p, spans, data, wins):
    rows = [0] * (PANEL + 1)
    for _, (y, _, _) in enumerate(spans):
        rows[y + 1] += 1
    for y in range(PANEL):
        rows[y + 1] += rows[y]

    def col(vals, per, fmt):
        line, out = [], []
        for v in vals:
            line.append(fmt % v)
            if len(line) == per:
                out.append("    " + ", ".join(line) + ",")
                line = []
        if line:
            out.append("    " + ", ".join(line) + ",")
        return "\n".join(out)

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("// GENERATED by tools/bezel_bake.py -- do not edit.\n//\n")
        fh.write("// A console bezel: the metal around the screen, drawn as opaque\n")
        fh.write("// pixels. Rows are PANEL rows, so the firmware reads straight\n")
        fh.write("// lines and does not rotate anything.\n//\n")
        fh.write("// %d spans over %d rows, %d pixels, palette of %d.\n"
                 % (len(spans), PANEL, len(data), len(pal)))
        fh.write("#pragma once\n#include \"../vg_bezel.h\"\n\n")

        fh.write("static const uint16_t %s_PAL[%d] = {\n%s\n};\n\n"
                 % (name, len(pal), col(pal, 8, "0x%04x")))
        fh.write("// One palette index per stored pixel, span by span.\n")
        fh.write("static const uint8_t %s_DATA[%d] = {\n%s\n};\n\n"
                 % (name, len(data), col(data, 16, "%3d")))
        fh.write("// x, length, and where the span's pixels start in the data.\n")
        fh.write("static const VgBezelSpan %s_SPAN[%d] = {\n" % (name, len(spans)))
        off = 0
        for (_, x0, n) in spans:
            fh.write("    { %3d, %3d, %6d },\n" % (x0, n, off))
            off += n
        fh.write("};\n\n")
        fh.write("// The first span of each panel row. The last entry is the total,\n")
        fh.write("// so a row's spans are row[y] up to row[y + 1].\n")
        fh.write("static const uint16_t %s_ROW[%d] = {\n%s\n};\n\n"
                 % (name, PANEL + 1, col(rows, 12, "%5d")))
        fh.write("// WHAT THE GAME DRAWS IN, in upright screen coordinates.\n")
        fh.write("// Role, then the largest rectangle inside the area, then its\n")
        fh.write("// full extent. Reading order within a role. Emitted from the\n")
        fh.write("// drawing rather than measured by hand, so a redrawn chassis\n")
        fh.write("// moves the layout with it instead of disagreeing with it.\n")
        fh.write("static const VgBezelSlot %s_SLOT[%d] = {\n" % (name, len(wins)))
        for k, (role, inr, box) in enumerate(wins):
            fh.write("    { %d, %3d,%4d,%4d,%4d,  %3d,%4d,%4d,%4d },   // %d %s\n"
                     % ((role,) + tuple(inr) + tuple(box) + (k, ROLE_NAME[role])))
        fh.write("};\n\n")
        # AND THE SAME NUMBERS AS DEFINES, for a screen that lays itself out at
        # compile time. The table is for the console layer, which is generic and
        # asks at runtime; these are for a screen that knows its own drawing and
        # would rather have constants. S0, S1... are the drawing slots in reading
        # order; HL is the headline.
        for k, (role, inr, box) in enumerate(wins):
            tag = "HL" if role == HEADLINE else ("S%d" % k)
            for suf, v in (("X0", inr[0]), ("Y0", inr[1]),
                           ("X1", inr[2]), ("Y1", inr[3]),
                           ("BX0", box[0]), ("BY0", box[1]),
                           ("BX1", box[2]), ("BY1", box[3])):
                fh.write("#define %s_%s_%s %3d\n" % (name, tag, suf, v))
        fh.write("\n")
        fh.write("static const VgBezel %s = {\n" % name)
        fh.write("    %s_PAL, %s_DATA, %s_SPAN, %s_ROW, %s_SLOT,\n"
                 % (name, name, name, name, name))
        fh.write("    %d, %d, %d,\n};\n" % (len(spans), len(data), len(wins)))


def main(argv):
    name, colors, report = "BEZEL_CONSOLE", 256, False
    args = []
    for a in argv:
        if a.startswith("--name="):
            name = a.split("=", 1)[1]
        elif a.startswith("--colors="):
            colors = int(a.split("=", 1)[1])
        elif a == "--report":
            report = True
        else:
            args.append(a)
    if len(args) < 1:
        sys.exit(__doc__)

    pal, _rgb, idx, mask, roles, err = load(args[0], colors)


    idx_p, mask_p = to_panel(idx), to_panel(mask)
    spans = spans_of(mask_p)
    data = [int(idx_p[y, x0 + i]) for (y, x0, n) in spans for i in range(n)]

    kb = (len(data) + len(spans) * 8 + (PANEL + 1) * 2 + len(pal) * 2) / 1024.0
    print("  %s: %d x %d, %d colours" % (args[0], PANEL, PANEL, colors))
    print("  exempt %.1f%% of the panel, %d pixels stored"
          % (100.0 * mask.mean(), len(data)))
    print("  %d spans, %.1f a row at most" % (len(spans), max(
        sum(1 for s in spans if s[0] == y) for y in range(PANEL))))
    print("  colour error: mean %.2f, 99%% within %.0f"
          % (err.mean(), np.percentile(err, 99)))
    print("  %.1f KB of flash" % kb)

    if report:
        return
    wins = regions(roles)
    for k, (role, w, bx) in enumerate(wins):
        print("  %d %-8s inner x %3d..%3d y %3d..%3d   box x %3d..%3d y %3d..%3d"
              % (k, ROLE_NAME[role], w[0], w[2], w[1], w[3], bx[0], bx[2], bx[1], bx[3]))
    write(args[1], name, pal, idx_p, spans, data, wins)
    print("  wrote %s" % args[1])


if __name__ == "__main__":
    main(sys.argv[1:])
