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


def load(src, colors):
    """Read the drawing. Return the palette, the indices, and the exempt mask."""
    rgb = Image.open(src).convert("RGB")
    a = np.asarray(rgb).astype(int)

    # The mask comes from the FULL SIZE drawing and is reduced with NEAREST.
    # A smooth reduction blends magenta with the metal beside it and makes a
    # fringe of pixels that are neither exempt nor the colour of the drawing.
    # MAGENTA BY HUE, NOT BY LEVEL. The first test asked for a lot of red, a lot
    # of blue and little green, and the green limit is what let pixels through:
    # (255, 130, 255) is plainly magenta and failed it. Ask instead whether red
    # and blue both stand clear of green, which is what magenta means, and no
    # level of brightness changes the answer.
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    exempt = (np.minimum(r, b) - g) > 40

    # GROWN BY TWO PIXELS. The edge of a painted area is smoothed against the
    # metal beside it, so a ring of part-magenta pixels sits just outside any
    # test. Widening the test only moves the ring. Growing the area swallows it,
    # at the cost of two pixels of metal that the game draws over anyway.
    for _ in range(2):
        e = exempt.copy()
        e[1:, :] |= exempt[:-1, :]
        e[:-1, :] |= exempt[1:, :]
        e[:, 1:] |= exempt[:, :-1]
        e[:, :-1] |= exempt[:, 1:]
        exempt = e

    mask = np.asarray(Image.fromarray((exempt * 255).astype(np.uint8))
                      .resize((PANEL, PANEL), Image.NEAREST)) > 127

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
    return pal565, idx, mask, err


def regions(mask):
    """Find each exempt area and the largest rectangle that fits inside it.

    The game needs to know where it may draw. A bounding box is not enough: the
    screen aperture is notched around the title window, so its box includes
    metal. The largest rectangle that fits inside the area is a place the layout
    can use without testing anything at draw time.
    """
    seen = np.zeros(mask.shape, np.int32)
    out, tag = [], 0
    for sy in range(PANEL):
        for sx in range(PANEL):
            if not mask[sy, sx] or seen[sy, sx]:
                continue
            tag += 1
            stack, n = [(sy, sx)], 0
            seen[sy, sx] = tag
            while stack:
                y, x = stack.pop()
                n += 1
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < PANEL and 0 <= nx < PANEL                             and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = tag
                        stack.append((ny, nx))
            out.append((n, max_rect(seen == tag)))
    # The aperture is the largest by area. The two bar windows are within a few
    # hundred pixels of each other, so area does NOT order them reliably -- it
    # put the bottom bar first on the drawing this was written for. Order them
    # by where they sit instead, which is what the names mean.
    out.sort(reverse=True, key=lambda r: r[0])
    rest = sorted((r[1] for r in out[1:]), key=lambda w: w[1])
    return [out[0][1]] + rest


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
        fh.write("// WHERE THE GAME MAY DRAW, in upright screen coordinates.\n")
        fh.write("// The largest rectangle inside each exempt area, biggest\n")
        fh.write("// first: the screen aperture, then the two bar windows.\n")
        fh.write("// Emitted rather than measured by hand, so a redrawn chassis\n")
        fh.write("// moves the layout with it instead of disagreeing with it.\n")
        for tag, (x0, y0, x1, y1) in zip(("APERTURE", "BAR_TOP", "BAR_BOT"), wins):
            fh.write("#define %s_%s_X0 %3d\n" % (name, tag, x0))
            fh.write("#define %s_%s_Y0 %3d\n" % (name, tag, y0))
            fh.write("#define %s_%s_X1 %3d\n" % (name, tag, x1))
            fh.write("#define %s_%s_Y1 %3d\n" % (name, tag, y1))
        fh.write("\n")
        fh.write("static const VgBezel %s = {\n" % name)
        fh.write("    %s_PAL, %s_DATA, %s_SPAN, %s_ROW,\n" % (name, name, name, name))
        fh.write("    %d, %d,\n};\n" % (len(spans), len(data)))


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

    pal, idx, mask, err = load(args[0], colors)
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
    wins = regions(mask)
    for tag, w in zip(("aperture", "top bar", "bottom bar"), wins):
        print("  %-10s x %3d..%3d  y %3d..%3d" % (tag, w[0], w[2], w[1], w[3]))
    write(args[1], name, pal, idx_p, spans, data, wins)
    print("  wrote %s" % args[1])


if __name__ == "__main__":
    main(sys.argv[1:])
