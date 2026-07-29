# Phantom — working notes

## The writing boundary

Two kinds of prose live in this repo, and they are governed differently.

**The game's writing is VOICE. Do not calibrate it, do not simplify it, do not
apply Simplified Technical English to it.** It is meant to be read, not
executed. It includes:

- pilot dialogue and voice archetypes (`src/vg/vg_voice.cpp`)
- ship names and taglines (`src/vg/vg_ship.*`)
- the backstory crawl, menu copy, and every string the player sees in the game
  (`src/vg/vg_screens.cpp`, `src/vg/vg_overlay.cpp`, `src/vg/vg_game.cpp`)
- `DESIGN.md` and the root `README.md`

The author keeps full creative control of all of it. If a line there reads
oddly, that is a decision until the author says otherwise. Ask; do not tidy.

**The tools' writing is FUNCTIONAL**, and the `ste` skill applies to it:

- `tools/README.md`
- `tools/*.py` — docstrings, comments, status lines, error messages, the About
  dialog, and the command line help

That text is read by someone who is trying to get a recording out of a device
that is not cooperating. It should be plain, and it is allowed to be dull.

## Firmware comments

Comments in `src/` explain why the code is the way it is. They are engineering
prose, not player-facing writing and not tool documentation. Leave their voice
alone unless asked. STE does not apply.
