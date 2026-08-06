# Design sources

The drawings the firmware is built from. These are **build inputs**, not reference
images. `tools/canopy_bake.py` reads a PNG here and writes a table into `src/vg/`,
so a change to a PNG changes what the game looks like.

| file | what it is |
|---|---|
| `Test.png` | the canopy in use. Red channel is the frame, green channel is the activation mask. |
| `hudSrc.psd` | the editable source for the HUD layout |
| `HUD.af` | Affinity Designer source |
| `HUD.png` | HUD layout reference |

## The generated header is not a source

`src/vg/vg_canopy_data.h` says `GENERATED ... do not edit` at the top and means it.
It is one bake away from the PNG and cannot be turned back into one. Edit the PNG.

## Canopy drawings

Read the authoring rules in `tools/README.md` before you draw one. Two of them catch
people:

- Use a **swizzled** PNG. Red carries the frame, green carries the activation
  regions. A grey image puts the same values in both, so the frame becomes its own
  mask.
- Paint the green channel over the **whole image**, not only where the frame is. The
  regions gate the world behind the cockpit, not just the cockpit.

To bake one:

    .\tools\canopy.ps1 -Png design\Chariot.png -Name CANOPY_CHARIOT `
        -Out src\vg\vg_canopy_chariot.h

## A note on file sizes

These are binary files, so git keeps a full copy of each version. A PSD is about
340 KB. Many saved revisions will make the repository large, and git cannot pack
them the way it packs text.

Keep the number of committed revisions low. Commit a drawing when it is worth
keeping, not on every save.
