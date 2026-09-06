"""Build the canopy set from design/canopy/, and generate the wiring.

    python tools/canopy_set.py

Each drawing is named after the hull that flies it: chariot.png is the CHARIOT's
cockpit. A hull with no drawing flies with no cockpit, which the game supports --
there is no default texture and no substitute.

<hull>.png is an OPAQUE drawing: painted metal that replaces the pixel, with a thin
additive outline, baked by tools/canopy_opaque.py. It was a light delta until
2026-09-05 -- a drawing applied as a CHANGE to the finished picture -- and every
hull's art moved over on that day. The delta baker and its renderer were removed on
2026-09-06.

<hull>_tint.png beside it is a mask saying which of that metal takes the player's
chosen colour. White is painted and black is bare.

This bakes every drawing that has changed and writes the table that maps a hull to
its canopy, so no C++ has to be edited to add one. Drop a PNG in, run this, done.

Read tools/README.md before you draw one. The three rules that catch people are that
magenta marks a pane, cyan marks the lit outline, and the alpha channel has to hold
one flat activation region over every pixel of the image, not only where the frame
is.
"""
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "design", "canopy")
GEN = os.path.join(ROOT, "src", "vg", "generated")

# In ShipClass order, which is what the generated table indexes by. The names are
# the enum's, lowercased: the file name IS the wiring, so this list is the contract
# between the directory and the firmware.
HULLS = ["aegis", "lance", "chariot", "ballista"]


def main():
    if not os.path.isdir(SRC):
        sys.exit("no drawings folder: %s" % SRC)
    if not os.path.isdir(GEN):
        os.makedirs(GEN)

    baker = os.path.join(HERE, "canopy_opaque.py")
    rows = []
    for hull in HULLS:
        png = os.path.join(SRC, hull + ".png")
        if not os.path.isfile(png):
            rows.append((hull, None, None))
            continue
        out = os.path.join(GEN, "canopy_op_%s.h" % hull)
        name = "CANOPY_OP_%s" % hull.upper()
        # THE TINT MASK, if the hull has one: it says which metal takes the
        # player's colour. Its own file, because the drawing's four channels are
        # spent -- three on the paint, the alpha on the arrival.
        tint = os.path.join(SRC, hull + "_tint.png")
        opts = ["--tint=" + tint] if os.path.isfile(tint) else []

        # Baked when the drawing's CONTENT differs from what the header was made from,
        # and not when it is merely newer.
        #
        # A modification time says nothing useful here. Copying an older export back over
        # chariot.png carries its old timestamp with it, so the header looked newer, the
        # bake was skipped, and the build silently kept the table for a drawing that was no
        # longer on disk. That is not a slow rebuild, it is the wrong cockpit shipped
        # quietly -- and reverting to an earlier drawing is an ordinary thing to do while
        # working.
        #
        # It cost a measurement here: two canopies were compared and came back identical to
        # one microsecond, which looked like a beautifully repeatable harness and was
        # actually the same table twice.
        #
        # THE BAKER GOES INTO THE HASH TOO, and the reason is the same failure one level
        # up. This guarded the SOURCE and nothing else, so changing how the drawing is
        # baked left every header on disk looking fresh. It is the same shape as the
        # incident above: a cache that answers "has the input changed" when the question
        # is "is this output still what the tools would produce".
        #
        # AND THE MASK. Editing the mask changes which palette entry every painted pixel
        # gets, so a bake that ignored it would keep the old paint area and nothing would
        # say so.
        want = hashlib.sha256(open(png, "rb").read()
                              + open(baker, "rb").read()
                              + (open(tint, "rb").read() if opts else b"")
                              ).hexdigest()[:16]
        fresh = False
        if os.path.isfile(out):
            with open(out) as fh:
                head = fh.read(2048)
            m = re.search(r"source\+baker sha256 ([0-9a-f]{16})", head)
            fresh = bool(m and m.group(1) == want)
        if not fresh:
            print("-- baking %s" % os.path.basename(png))
            r = subprocess.run([sys.executable, baker, png, out, "--name=" + name] + opts)
            if r.returncode != 0:
                sys.exit("bake failed for %s" % png)
            # The stamp the check above reads. Written here rather than by the baker so
            # that the baker stays a plain PNG-to-header tool anybody can run by hand.
            with open(out) as fh:
                body = fh.read()
            with open(out, "w") as fh:
                fh.write("// source+baker sha256 %s -- tools/canopy_set.py rebakes when "
                         "either changes.\n" % want)
                fh.write(body)
        else:
            print("-- %s is up to date" % os.path.basename(out))
        rows.append((hull, name, out))

    # ---- the wiring -------------------------------------------------------
    #
    # Generated rather than hand-edited, because the hand-edited version was the one
    # step that was not "drop a file in" -- and nothing stopped a drawing being wired
    # to the wrong hull.
    path = os.path.join(GEN, "canopy_set.h")
    with open(path, "w") as fh:
        fh.write("// GENERATED by tools/canopy_set.py from design/canopy/ -- do not edit.\n")
        fh.write("//\n")
        fh.write("// One row per hull, in ShipClass order. A hull with no drawing gets\n")
        fh.write("// nullptr, which the renderer supports: it flies with no cockpit frame.\n")
        fh.write("// There is no default texture -- a canopy is authored per hull.\n")
        fh.write("#pragma once\n")
        # Resolved relative to THIS header, which sits a directory below the
        # hand-written code it needs.
        fh.write('#include "../vg_canopy_op.h"\n')
        for hull, name, out in rows:
            if name:
                fh.write('#include "canopy_op_%s.h"\n' % hull)
        fh.write("\n")
        fh.write("#define VG_CANOPY_OP_SET_ROWS \\\n")
        for i, (hull, name, out) in enumerate(rows):
            end = " \\" if i + 1 < len(rows) else ""
            fh.write("    /* %-8s */ %s,%s\n"
                     % (hull.upper(), ("&" + name) if name else "nullptr", end))
        fh.write("\n")

    print("\ncanopy set:")
    for hull, name, out in rows:
        if name:
            kb = os.path.getsize(out) / 1024.0
            tint = os.path.isfile(os.path.join(SRC, hull + "_tint.png"))
            print("  %-9s %-14s %6.0f KB of generated header%s"
                  % (hull.upper(), hull + ".png", kb,
                     ", painted" if tint else ""))
        else:
            print("  %-9s %-14s no cockpit frame" % (hull.upper(), "--"))
    n = sum(1 for _, name, _ in rows if name)
    print("\n%d of %d hulls have a canopy. Wrote %s"
          % (n, len(rows), os.path.relpath(path, ROOT)))


if __name__ == "__main__":
    main()
