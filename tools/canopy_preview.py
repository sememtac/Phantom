"""Show what a tolerance setting removes from a canopy drawing.

    python tools/canopy_preview.py design/canopy/chariot.png out.png
    python tools/canopy_preview.py design/canopy/chariot.png out.png --tol=0,20,28

The baker does not paint a pixel whose value is within the tolerance of the
background, so raising the tolerance removes the faintest parts of the drawing
first. The pixel count it saves is easy to read and what it costs the picture is
not, which is what this is for.

Each tolerance gets two panels. The first is the drawing that survives. The
second marks the pixels that were dropped, so a change that is hard to see in
the first panel can still be located in the second.

The count under each panel is the pixels painted in a whole frame. Compare it
against BUDGET_PX in tools/canopy_bake.py.
"""

import sys
from PIL import Image, ImageDraw

PANEL = 480
LOST = (220, 40, 40)


def load(src):
    """The drawing's own channel, and the background value the baker would pick."""
    rgb = Image.open(src).convert("RGB")
    im = rgb.split()[0]
    px = im.load()
    sw, sh = im.size
    hist = {}
    for y in range(sh):
        for x in range(sw):
            hist[px[x, y]] = hist.get(px[x, y], 0) + 1
    return im, px, sw, sh, max(hist, key=lambda v: hist[v])


def panels(px, sw, sh, bg, tol):
    """The surviving drawing, the map of what went, and how many pixels are left."""
    keep = Image.new("RGB", (sw, sh))
    gone = Image.new("RGB", (sw, sh))
    kp, gp = keep.load(), gone.load()
    n = 0
    for y in range(sh):
        for x in range(sw):
            v = px[x, y]
            if abs(v - bg) > tol:
                kp[x, y] = (v, v, v)
                gp[x, y] = (v // 3, v // 3, v // 3)
                n += 1
            else:
                kp[x, y] = (bg, bg, bg)
                gp[x, y] = LOST if abs(v - bg) > 0 else (bg // 3, bg // 3, bg // 3)
    # Scaled to the panel the firmware draws at, which is what decides the cost.
    return keep, gone, int(round(n * (PANEL / float(sw)) * (PANEL / float(sh))))


def main(argv):
    tols = [0, 20, 28]
    args = [a for a in argv if not a.startswith("--")]
    for a in argv:
        if a.startswith("--tol="):
            tols = [int(v) for v in a.split("=", 1)[1].split(",")]
    if len(args) < 2:
        sys.exit(__doc__)
    src, out = args[0], args[1]

    im, px, sw, sh, bg = load(src)
    print("%s: %dx%d, background %d" % (src, sw, sh, bg))

    cell = 300
    pad, top = 12, 26
    sheet = Image.new("RGB", (len(tols) * (cell + pad) + pad,
                              2 * (cell + top) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, tol in enumerate(tols):
        keep, gone, n = panels(px, sw, sh, bg, tol)
        x = pad + i * (cell + pad)
        sheet.paste(keep.resize((cell, cell)), (x, top))
        sheet.paste(gone.resize((cell, cell)), (x, top + cell + top))
        draw.text((x, 8), "tol %d   %d px a frame" % (tol, n), fill=(235, 235, 235))
        draw.text((x, top + cell + 8), "dropped, in red", fill=(210, 120, 120))
        print("  tol %2d: %6d px a frame" % (tol, n))
    sheet.save(out)
    print("wrote %s" % out)


if __name__ == "__main__":
    main(sys.argv[1:])
