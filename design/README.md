# Design sources

The drawings the firmware is built from. These are **build inputs**, not reference
images. `tools/canopy_set.py` reads the PNGs here and writes tables into
`src/vg/generated/`, so a change to a PNG changes what the game looks like.

| file | what it is |
|---|---|
| `canopy/chariot.png` | the CHARIOT's cockpit. Red channel is the frame, green channel is the activation mask. |
| `hudSrc.psd` | the editable source for the HUD layout |
| `HUD.af` | Affinity Designer source |
| `HUD.png` | HUD layout reference |
| `selection/SelectionConcept2.png` | the console chassis. Colour marks the holes the game draws in. |
| `selection/SelectionConcept1.png` | the layout it was drawn to. Reference only. |

## Console chassis drawings

A chassis is the metal around a menu. The firmware draws it as opaque pixels, so
it hides what is behind it. This is not a canopy: a canopy adds light to the
picture and a chassis replaces it, so the two use different tools.

    python tools/bezel_bake.py design/selection/SelectionConcept2.png         src/vg/generated/bezel_console.h --name=BEZEL_CONSOLE

**Paint the holes in the colour of the job they do.** The baker stores no pixel
there and the firmware draws no pixel there, and the colour tells the game what
each hole is for.

| colour | job |
|---|---|
| magenta (255, 0, 255) | a slot. The game draws in it. |
| cyan (0, 174, 239) | the headline. A ticker runs across it. |

Slots are numbered top to bottom, then left to right. Slot 0 is the big window a
screen lays itself out in. The rest are keys, in the order you see them. So a
drawing with three keys along the bottom gives the game key 0, key 1 and key 2
from left to right, and nothing has to say which is which.

Two rules for the paint:

- Use a **flat** colour. The baker grows each area by two pixels to swallow the
  smoothed edge, but it cannot find an area that fades away.
- Leave metal **between** two holes. Two areas of the same colour that touch are
  one area.

The baker prints what it found. Read it:

    0 SLOT     inner x  24..455 y  98..365   box x  24..455 y  70..375
    1 SLOT     inner x 111..367 y 378..407   box x 108..371 y 377..407
    2 HEADLINE inner x 118..363 y  58.. 87   box x 113..366 y  58.. 87

If the count or the order is wrong, the paint is wrong. Fix the PNG.

## The generated header is not a source

Everything in `src/vg/generated/` says `GENERATED ... do not edit` at the top and
means it. A generated file is one bake away from the PNG and cannot be turned back
into one. Edit the PNG.

## Canopy drawings

Read the authoring rules in `tools/README.md` before you draw one. Two of them catch
people:

- Use a **swizzled** PNG. Red carries the frame, green carries the activation
  regions. A grey image puts the same values in both, so the frame becomes its own
  mask.
- Paint the green channel over the **whole image**, not only where the frame is. The
  regions gate the world behind the cockpit, not just the cockpit.

Name it after the ship and put it in `canopy/`: `aegis.png`, `lance.png`,
`chariot.png`, `ballista.png`. The file name is the wiring, so there is nothing to
edit afterwards.

A ship can also have an opaque drawing, `<ship>_opaque.png`. Its colours say what
each pixel is: magenta is a pane, cyan is the lit outline, and any other colour is
metal that the firmware paints over the picture. The alpha channel holds the
activation regions. Keep the plain drawing next to it; the firmware still needs it.
The rules are in `tools/README.md` under **An opaque drawing**.

    .\tools\canopy.ps1

A ship with no drawing flies with no cockpit frame, which the game supports. You can
add them one at a time.

## A note on file sizes

These are binary files, so git keeps a full copy of each version. A PSD is about
340 KB. Many saved revisions will make the repository large, and git cannot pack
them the way it packs text.

Keep the number of committed revisions low. Commit a drawing when it is worth
keeping, not on every save.
