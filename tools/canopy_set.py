"""Build the canopy set from design/canopy/, and generate the wiring.

    python tools/canopy_set.py

Each drawing is named after the hull that flies it: chariot.png is the CHARIOT's
cockpit. A hull with no drawing flies with no cockpit, which the game supports --
there is no default texture and no substitute.

This bakes every drawing that has changed and writes the table that maps a hull to
its canopy, so no C++ has to be edited to add one. Drop a PNG in, run this, done.

Read tools/README.md before you draw one. The two rules that catch people are that
the PNG must be swizzled -- red for the frame, green for the activation regions --
and that the green channel has to cover the whole image, not only where the frame is.
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

# Per-hull bake options. The author sets them one drawing at a time, after looking at
# what each option changes (tools/canopy_preview.py renders it). A hull not listed here
# bakes at the default tolerance. The baker never reduces a drawing on its own: it
# reports the cost, and the decision is made here or not at all.
#
# THE OPTION GOES BOTH WAYS. It was written to make a heavy drawing lighter. LANCE uses
# it to make a light drawing heavier, which is the same control read the other way.
#
# LANCE: --tol=8. The baker skips a pixel within the tolerance of the background, and
# the background is 128 exactly, so any tolerance above 0 discards ART and not empty
# space. This drawing is low contrast -- its faintest deliberate tone sits at 148, only
# twenty levels off the ground -- so the default of 20 landed on it and threw away
# 37,806 drawn pixels, 55.8% of the artwork. That is the frame the author saw as bled
# out, and it is what this entry exists to stop.
#
# TOLERANCE 0 WAS FLOWN AND REJECTED, 2026-08-21. It paints every pixel drawn: 60,100 a
# frame, 26.1% of the screen, 4.94 ms, which is the whole of what the cockpit may have
# and 21% heavier than the AEGIS. The frame rate was poor on the glass.
#
# 8 keeps 84% of the art and lands at 48,828 px a frame -- the AEGIS's weight almost
# exactly, and the AEGIS flies at 60 to 65 fps. It is the flyable setting, not the
# finished one.
#
# THE REAL FIX IS AREA, AND IT IS IN THE DRAWING. Cost is pixels PAINTED; brightness and
# gradients are free, so brightening the faint shading does NOT make it cheaper -- the
# same pixels are painted either way. What brightness buys is CONTROL: shading drawn
# past about 155 survives any tolerance, so the artist chooses what a cut removes
# instead of the cut taking whatever happens to be faintest. To make the drawing
# cheaper, narrow the shapes. 26.1% of the screen is more than the frame can carry;
# the AEGIS is 21.6% and the CHARIOT 14.3%.
#
# AEGIS: nothing, and that is a decision. It was fitted to 33,000 px, which is the
# CHARIOT's flown weight and runs the course at 58.9 fps. The author flew it on
# 2026-08-19, judged the loss on the glass, and rejected it. The drawing at full weight
# is the look, and about 55 fps is what that look costs.
FIT = {"lance": ["--tol=24"]}


def main():
    if not os.path.isdir(SRC):
        sys.exit("no drawings folder: %s" % SRC)
    if not os.path.isdir(GEN):
        os.makedirs(GEN)

    baker = os.path.join(HERE, "canopy_bake.py")
    rows = []
    for hull in HULLS:
        png = os.path.join(SRC, hull + ".png")
        if not os.path.isfile(png):
            rows.append((hull, None, None))
            continue
        out = os.path.join(GEN, "canopy_%s.h" % hull)
        name = "CANOPY_%s" % hull.upper()

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
        # THE BAKER GOES INTO THE HASH TOO, and the reason is the same failure one level
        # up. This guarded the SOURCE and nothing else, so changing how the drawing is
        # baked -- TOL, QUANT, MINFLAT, or the algorithm itself -- left every header on
        # disk looking fresh. A canopy retuned from 90% of budget to 56% was silently
        # discarded the next time anything ran, because the png had not moved.
        #
        # It is the same shape as the incident above: a cache that answers "has the input
        # changed" when the question is "is this output still what the tools would produce".
        # The hash covers the drawing, the baker, AND this hull's fit options. A fit
        # added or changed in FIT must rebake, and without the options in the hash it
        # silently did not -- the stale header passed the check and the new budget never
        # applied.
        want = hashlib.sha256(open(png, "rb").read()
                              + open(baker, "rb").read()
                              + " ".join(FIT.get(hull, [])).encode()).hexdigest()[:16]
        fresh = False
        if os.path.isfile(out):
            with open(out) as fh:
                head = fh.read(2048)
            m = re.search(r"source(?:\+baker(?:\+fit)?)? sha256 ([0-9a-f]{16})", head)
            fresh = bool(m and m.group(1) == want)
        if not fresh:
            print("-- baking %s" % os.path.basename(png))
            # NO TOLERANCE PASSED, so the baker uses its own default. It also reports
            # when a drawing costs more than the frame can carry, and it does NOT reduce
            # it -- see the note above BUDGET_PX in the baker. The art is the input.
            r = subprocess.run([sys.executable, baker, png, out, "--name=" + name]
                               + FIT.get(hull, []))
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
        fh.write('#include "../vg_canopy.h"\n')
        for hull, name, out in rows:
            if name:
                fh.write('#include "canopy_%s.h"\n' % hull)
        fh.write("\n")
        fh.write("#define VG_CANOPY_SET_ROWS \\\n")
        for i, (hull, name, out) in enumerate(rows):
            end = " \\" if i + 1 < len(rows) else ""
            fh.write("    /* %-8s */ %s,%s\n"
                     % (hull.upper(), ("&" + name) if name else "nullptr", end))
        fh.write("\n")

    print("\ncanopy set:")
    for hull, name, out in rows:
        if name:
            kb = os.path.getsize(out) / 1024.0
            print("  %-9s %-14s %6.0f KB of generated header" % (hull.upper(),
                                                                hull + ".png", kb))
        else:
            print("  %-9s %-14s no cockpit frame" % (hull.upper(), "--"))
    n = sum(1 for _, name, _ in rows if name)
    print("\n%d of %d hulls have a canopy. Wrote %s"
          % (n, len(rows), os.path.relpath(path, ROOT)))


if __name__ == "__main__":
    main()
