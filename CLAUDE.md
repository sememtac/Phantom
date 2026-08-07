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
- `DESIGN.md`, the working design document

`DESIGN.md` is **not tracked by git** and must stay that way. It is the working
design document, for us, and it is listed in `.gitignore`. Keep it updated as the
game changes; do not add it back to the repository.

The author keeps full creative control of all of it. If a line there reads
oddly, that is a decision until the author says otherwise. Ask; do not tidy.

**The tools' and the repository's writing is FUNCTIONAL**, and the `ste` skill
applies to it:

- the root `README.md`
- `tools/README.md` and `design/README.md`
- `tools/*.py` — docstrings, comments, status lines, error messages, the About
  dialog, and the command line help

That text is read by someone who is trying to get a recording out of a device
that is not cooperating, or to build the firmware for the first time. It should be
plain, and it is allowed to be dull.

The root `README.md` MOVED HERE from the voice list, and the reason is worth keeping.
It is a reference document: people execute its build steps, look up a constant, and
read it to decide whether the project is worth their evening. It also had the faults
STE is for — `frame` meant both a rendered picture and the cockpit structure, `wire`
was a metaphor carrying real meaning, and `cost` covered both microseconds and
credits. It reads plainer now and it lost some good lines doing it. That trade was
the author's call.

What it is NOT is a place for the game's voice. Ship names and taglines quoted in it
are the game's writing and stay verbatim.

## Firmware comments

Comments in `src/` explain why the code is the way it is. They are engineering
prose, not player-facing writing and not tool documentation. Leave their voice
alone unless asked. STE does not apply.
