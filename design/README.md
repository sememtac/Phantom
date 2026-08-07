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

    .\tools\canopy.ps1

A ship with no drawing flies with no cockpit frame, which the game supports. You can
add them one at a time.

## A note on file sizes

These are binary files, so git keeps a full copy of each version. A PSD is about
340 KB. Many saved revisions will make the repository large, and git cannot pack
them the way it packs text.

Keep the number of committed revisions low. Commit a drawing when it is worth
keeping, not on every save.
