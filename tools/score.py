#!/usr/bin/env python3
"""Build the game's music from a score file.

    python tools/score.py design/score/motif.score
    python tools/score.py design/score/motif.score --mid motif.mid
    python tools/score.py design/score/motif.score --bake
    python tools/score.py --from-mid some.mid

A score file says what the music is. This tool reads it and does three jobs.

1. It prints a report. The report gives the length, the harmony, the voice cost,
   and a speaker check. Read the speaker check first. The device speaker is one
   centimetre across and it gives very little below about 300 Hz.
2. `--mid` writes a MIDI file. Play that file on this computer to judge the
   notes. It uses a piano sound, so it does not tell you the timbre.
3. `--bake` writes a table into src/vg/generated/. The device plays the table
   through vg_synth.

This tool does NOT make sound. There is no copy of the synthesiser on this
computer, and there must not be one. A second copy drifts from the first, and
then the preview lies. To hear the true timbre, bake the table and flash it.

`--from-mid` reads a MIDI file and prints a score file. Use it to bring in music
that you wrote somewhere else.

WARNING: the synthesiser has 10 voices and no priority. vg_synth.cpp takes the
voice with the least time left. Music and the missile alert compete for the same
10 voices. Keep the peak voice count in the report at 4 or less.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "src", "vg", "generated")

PPQ = 480                  # MIDI ticks in one beat
AUDIO_RATE = 22050         # VG_AUDIO_RATE in src/vg/vg_port.h
NYQUIST = AUDIO_RATE // 2

# Below this frequency the device speaker gives very little. The number comes
# from the note at the top of vg_sfx.cpp, which was measured by ear on the
# device.
SPEAKER_FLOOR = 300.0

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
STEP = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
IVAL = ["unison", "minor 2nd", "major 2nd", "minor 3rd", "major 3rd", "4th",
        "tritone", "5th", "minor 6th", "major 6th", "minor 7th", "major 7th",
        "octave"]
WAVES = {"square": "SW_SQUARE", "noise": "SW_NOISE", "sine": "SW_SINE"}

NOTE_RE = re.compile(r"^([A-Ga-g])([#b]?)(-?\d+)$")
HIT = 60               # the pitch of an X slot. The noise wave ignores it.
ROOT_RE = re.compile(r"^R([+-]\d+)?$")


def note_num(tok):
    """Turn a note name into a MIDI number. C4 is 60."""
    m = NOTE_RE.match(tok)
    if not m:
        raise ValueError("bad note name %r" % tok)
    letter, accidental, octave = m.group(1).upper(), m.group(2), int(m.group(3))
    n = STEP[letter] + (1 if accidental == "#" else -1 if accidental == "b" else 0)
    return 12 * (octave + 1) + n


def rel(path):
    """Return a short path for a message. A path on another drive has none."""
    try:
        return os.path.relpath(path, ROOT).replace("\\", "/")
    except ValueError:
        return path


def plural(n, word):
    """Return a count and a word. Add an s when the count is not one."""
    return "%g %s%s" % (n, word, "" if n == 1 else "s")


def note_name(n):
    return "%s%d" % (NAMES[n % 12], n // 12 - 1)


def hz(n):
    return 440.0 * (2.0 ** ((n - 69) / 12.0))


def hz_end(part, n):
    """The pitch a note falls to. It is the start pitch when sweep is zero."""
    return hz(n) * (2.0 ** (part.sweep / 12.0))


SETTINGS = ("gain", "lp", "gate", "sus", "atk", "mod", "depth", "sweep")


class Part:
    """One voice. A pattern part repeats with the groups. A free part does not.

    A pattern part holds slots, and R in a slot means the root of the group. Use
    it for the parts that follow the roots.

    A free part holds one event for each note, at a beat from the start of the
    score. Use it for a melody, which does not repeat.
    """

    def __init__(self, name, kind="pattern"):
        self.name = name
        self.kind = kind
        self.wave = "square"
        self.gain = 0.25
        self.lp = 3000.0
        self.gate = 0.6         # part of the slot that sounds
        self.sus = 0.15         # part of the life held at full
        self.atk = 0.004
        self.mod = 0.0
        # Semitones the pitch falls across the life of a note. Zero holds the
        # pitch. A DRUM NEEDS THIS. The speaker is one centimetre across and gives
        # no weight from a low pitch alone, so weight comes from a pitch that
        # falls fast. vg_sfx.cpp builds its explosion the same way, at 90 Hz down
        # to 28, and says so.
        self.sweep = 0.0
        self.depth = 0.0
        self.mute = False       # a muted part is drawn, but it does not sound
        self.pattern = []       # pattern parts: a list of slots
        self.events = []        # free parts: a list of (beat, midi note, beats)


# The parts you can add. A preset gives the SOUND of a part and NOT its rhythm.
# Every pattern preset arrives empty, as rests, so the timing is yours to write.
# The drum settings come from vg_sfx.cpp, which was tuned by ear on the device.
#
# A KICK IS NOT A LOW NOTE. It is a tone that falls fast. The speaker is one
# centimetre across and gives nothing from a low pitch on its own.
PRESETS = {
    "melody":  dict(kind="free", wave="square", gain=0.20, lp=4200.0, gate=0.95,
                    sus=0.35),
    "bass":    dict(kind="pattern", wave="square", gain=0.30, lp=900.0, gate=0.70,
                    sus=0.30, slots=4),
    "kick":    dict(kind="pattern", wave="square", gain=0.55, lp=700.0, gate=0.50,
                    sus=0.0, atk=0.002, sweep=-18.0, slots=4),
    "snare":   dict(kind="pattern", wave="noise", gain=0.35, lp=2600.0, gate=0.35,
                    sus=0.05, atk=0.002, slots=4),
    "hat":     dict(kind="pattern", wave="noise", gain=0.12, lp=7000.0, gate=0.10,
                    sus=0.0, atk=0.001, slots=8),
    "chord":   dict(kind="pattern", wave="square", gain=0.26, lp=2800.0, gate=0.55,
                    sus=0.20, slots=4),
    "empty":   dict(kind="pattern", wave="square", slots=4),
}

# One line of advice for each preset, for a status line. It says what the sound
# wants, and never writes a note.
HINTS = {
    "melody": "Click the grid to write it. It does not repeat.",
    "bass":   "R-12 is the root, one octave down.",
    "kick":   "It falls 18 semitones. Put its notes near G2.",
    "snare":  "Noise, so every hit is X wherever you click.",
    "hat":    "Noise, so every hit is X wherever you click.",
    "chord":  "R follows the root. A note name is fixed in every group.",
    "empty":  "A plain square. Change its settings in the file.",
}


def make_part(preset, name=None, taken=(), kind=None):
    """Build a part from a preset.

    A PRESET GIVES THE SOUND. `kind` gives the shape, and it is the caller's
    choice: "pattern" for a cell that repeats with the groups, "free" for notes
    that are written one at a time and repeat nothing.
    """
    if preset not in PRESETS:
        raise ValueError("unknown part %r" % preset)
    spec = dict(PRESETS[preset])
    p = Part(name or preset, kind or spec.pop("kind"))
    spec.pop("kind", None)
    # Rests, not a rhythm. The timing belongs to whoever writes the score.
    p.pattern = ["."] * int(spec.pop("slots", 4))
    if p.kind == "free":
        p.pattern = []
    for key, val in spec.items():
        setattr(p, key, val)
    base, k = p.name, 2
    while p.name in taken:
        p.name = "%s%d" % (base, k)
        k += 1
    return p


def all_parts(s):
    """Every distinct part: the base parts, then the parts of each section.

    The live engine and the studio key a part by its place in this list, so the
    list must be built the same way everywhere and must not change order.
    """
    out = list(s.parts)
    for sec in s.sections:
        for p in sec.parts:
            if not any(p is q for q in out):
                out.append(p)
    return out


def to_free(s, sec, part):
    """Turn a pattern part into a free part that sounds the same.

    The cell is written out once for every group and every rep, with R resolved
    against the root of its group. After this the notes can be moved one at a
    time, because nothing repeats any more.
    """
    if part.kind == "free":
        return 0
    roots = sec_roots(s, sec) if sec else s.roots
    reps = sec_reps(s, sec) if sec else s.reps
    slots = len(part.pattern)
    if not slots:
        part.kind, part.pattern, part.events = "free", [], []
        return 0
    step = s.cell / float(slots)
    events = []
    for g, root in enumerate(roots):
        for r in range(reps):
            base = (g * reps + r) * s.cell
            held = None
            for i, tok in enumerate(part.pattern):
                at = base + i * step
                if tok == ".":
                    held = None
                    continue
                if tok == "-":
                    if held is not None:
                        a, n, ln = events[held]
                        events[held] = (a, n, ln + step)
                    continue
                if tok == "X":
                    n = HIT
                else:
                    m = ROOT_RE.match(tok)
                    if m:
                        n = root + (int(m.group(1)) if m.group(1) else 0)
                    else:
                        n = note_num(tok)
                events.append((at, n, step))
                held = len(events) - 1
    part.kind, part.pattern, part.events = "free", [], events
    return len(events)


def audible(notes):
    """Only the notes that sound. A muted part is drawn but never heard."""
    return [n for n in notes if not n["part"].mute]


class Section:
    """One piece of the arrangement.

    A section plays the base parts of the score. A part of its own REPLACES a base
    part with the same name, and that is how one section gets its own melody while
    the beats and the lead go on unchanged.

    A field left as None takes the value from the score. Give a section its own
    roots and it plays in another key. Give it its own reps and it runs longer.
    """

    def __init__(self, name):
        self.name = name
        self.transpose = None
        self.roots = None
        self.reps = None
        self.parts = []


class Score:
    def __init__(self):
        self.name = "score"
        self.tempo = 120.0
        self.cell = 2.0         # beats in one group
        self.reps = 4
        self.transpose = 0
        self.roots = []
        self.parts = []         # the base parts. Every section plays them.
        self.sections = []      # Section, in the order they were written
        self.order = []         # the arrangement. A name may appear many times.


def turn_name(entry):
    """The section a turn plays. A minus before the name means it is SKIPPED:
    the turn keeps its place in the order and makes no sound and no time."""
    return entry[1:] if entry.startswith("-") else entry


def turn_on(entry):
    return not entry.startswith("-")


def section(s, name):
    """Find a section by name, or None."""
    for sec in s.sections:
        if sec.name == name:
            return sec
    return None


def sec_roots(s, sec):
    return sec.roots if sec.roots else s.roots


def sec_reps(s, sec):
    return sec.reps if sec.reps else s.reps


def sec_transpose(s, sec):
    return s.transpose if sec.transpose is None else sec.transpose


def sec_parts(s, sec):
    """The parts a section plays. Its own replace a base part of the same name."""
    mine = {p.name: p for p in sec.parts}
    out = [mine.get(p.name, p) for p in s.parts]
    base = {p.name for p in s.parts}
    out.extend(p for p in sec.parts if p.name not in base)
    return out


def sec_beats(s, sec):
    """How many beats one turn of a section takes."""
    return len(sec_roots(s, sec)) * sec_reps(s, sec) * s.cell


def song_beats(s):
    """How many beats the whole arrangement takes."""
    total = 0.0
    for entry in s.order:
        if not turn_on(entry):
            continue
        sec = section(s, turn_name(entry))
        if sec is not None:
            total += sec_beats(s, sec)
    return total


def strip_comment(line):
    """Remove a comment. A number sign is a sharp when a letter comes before it."""
    for i, c in enumerate(line):
        if c == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i]
    return line


def read_score(path):
    """Read a score file. Return a Score.

    Lines belong to the base until a `section` line. After that they belong to
    that section, until the next `section` line or the `order` line.
    """
    s = Score()
    cur = None                  # None while the lines belong to the base
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = strip_comment(raw).strip()
            if not line:
                continue
            try:
                if line.startswith("section "):
                    nm = line.split(None, 1)[1].strip()
                    if section(s, nm):
                        raise ValueError("there are two sections named %r" % nm)
                    cur = Section(nm)
                    s.sections.append(cur)
                    continue
                if line.startswith("order "):
                    s.order = line.split()[1:]
                    cur = None
                    continue

                where = cur if cur is not None else s
                if line.startswith("part "):
                    where.parts.append(read_part(line))
                    continue
                if line.startswith("free "):
                    where.parts.append(read_part(line, kind="free"))
                    continue
                if line.startswith("at "):
                    if not where.parts or where.parts[-1].kind != "free":
                        raise ValueError("an 'at' line must follow a 'free' part")
                    w = line.split()
                    if len(w) != 4:
                        raise ValueError("an 'at' line needs a beat, a note and a length")
                    where.parts[-1].events.append((float(w[1]), note_num(w[2]), float(w[3])))
                    continue

                key, _, rest = line.partition(" ")
                rest = rest.strip()
                if cur is not None:
                    if key == "transpose":
                        cur.transpose = int(rest)
                    elif key == "reps":
                        cur.reps = int(rest)
                    elif key == "roots":
                        cur.roots = [note_num(t) for t in rest.split()]
                    else:
                        raise ValueError("a section cannot set %r" % key)
                    continue
                if key == "name":
                    s.name = rest
                elif key == "tempo":
                    s.tempo = float(rest)
                elif key == "cell":
                    s.cell = float(rest)
                elif key == "reps":
                    s.reps = int(rest)
                elif key == "transpose":
                    s.transpose = int(rest)
                elif key == "roots":
                    s.roots = [note_num(t) for t in rest.split()]
                else:
                    raise ValueError("unknown setting %r" % key)
            except ValueError as e:
                sys.exit("%s line %d: %s" % (path, lineno, e))

    if not s.sections:
        # A file with no sections is one section, so the rest of the tool has one
        # shape to work with.
        s.sections = [Section("main")]
    if not s.order:
        s.order = [sec.name for sec in s.sections]
    for nm in [turn_name(x) for x in s.order]:
        if section(s, nm) is None:
            sys.exit("%s: the order names %r, and there is no such section" % (path, nm))
    if not s.roots:
        sys.exit("%s: no roots. Add a line such as: roots C4 B3 A#3 G#3" % path)
    if not s.parts and not any(sec.parts for sec in s.sections):
        sys.exit("%s: no parts. Add a line that starts with 'part'." % path)
    return s


def read_part(line, kind="pattern"):
    """Read one part line. The settings come first. A pattern part then has a
    colon and its slots. A free part ends after its settings."""
    head, sep, tail = line.partition(":")
    if kind == "pattern" and not sep:
        raise ValueError("a part line needs a colon before the pattern")
    words = head.split()[1:]
    if not words:
        raise ValueError("a part line needs a name")
    p = Part(words[0], kind)
    rest = words[1:]
    if rest and rest[0] in WAVES:
        p.wave = rest.pop(0)
    if len(rest) % 2:
        raise ValueError("each setting needs a value")
    for i in range(0, len(rest), 2):
        key, val = rest[i], rest[i + 1]
        if key == "mute":
            p.mute = bool(int(float(val)))
        elif key not in SETTINGS:
            raise ValueError("unknown part setting %r" % key)
        else:
            setattr(p, key, float(val))
    if kind == "pattern":
        p.pattern = tail.split()
        if not p.pattern:
            raise ValueError("part %s has an empty pattern" % p.name)
    return p


def emit_part(out, p, pad=""):
    """Write one part, and the notes of a free part under it."""
    sets = " ".join("%s %g" % (k, getattr(p, k)) for k in SETTINGS
                    if getattr(p, k) != getattr(Part("x"), k))
    if p.kind == "pattern":
        out.append("%spart %s %s %s : %s" % (pad, p.name, p.wave, sets,
                                             " ".join(p.pattern)))
    else:
        out.append("%sfree %s %s %s" % (pad, p.name, p.wave, sets))
        for beat, n, length in sorted(p.events):
            out.append("%s    at %-8g %-5s %g" % (pad, beat, note_name(n), length))
    out.append("")


def write_score(s, path):
    """Write a Score back to a score file."""
    out = ["name        %s" % s.name,
           "tempo       %g" % s.tempo,
           "cell        %g" % s.cell,
           "reps        %d" % s.reps]
    if s.transpose:
        out.append("transpose   %d" % s.transpose)
    out.append("roots       %s" % " ".join(note_name(r) for r in s.roots))
    out.append("")
    for p in s.parts:
        emit_part(out, p)

    plain = (len(s.sections) == 1 and not s.sections[0].parts
             and s.sections[0].roots is None and s.sections[0].reps is None
             and s.sections[0].transpose is None
             and s.order == [s.sections[0].name])
    if not plain:
        for sec in s.sections:
            out.append("section %s" % sec.name)
            if sec.transpose is not None:
                out.append("    transpose %d" % sec.transpose)
            if sec.reps is not None:
                out.append("    reps %d" % sec.reps)
            if sec.roots:
                out.append("    roots %s" % " ".join(note_name(r) for r in sec.roots))
            out.append("")
            for p in sec.parts:
                emit_part(out, p, "    ")
        out.append("order       %s" % " ".join(s.order))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out).rstrip() + "\n")


def build_one(s, parts, roots, reps, transpose, at, oi, name, notes):
    """Add the notes of one turn of one section, starting `at` seconds."""
    beat = 60.0 / s.tempo
    for part in parts:
        if part.kind == "free":
            for k, (b, n, length) in enumerate(part.events):
                notes.append({"t": at + b * beat, "n": n + transpose,
                              "life": length * beat * part.gate, "part": part,
                              "src": k, "slot": -1, "group": -1, "tok": "",
                              "sec": oi, "secname": name})
            continue
        slots = len(part.pattern)
        if not slots:
            continue
        step = s.cell * beat / slots
        life = step * part.gate
        for g, root in enumerate(roots):
            for r in range(reps):
                base = at + (g * reps + r) * s.cell * beat
                held = None
                for i, tok in enumerate(part.pattern):
                    t = base + i * step
                    if tok == ".":
                        held = None
                        continue
                    if tok == "-":
                        if held is not None:
                            held["life"] += step
                        continue
                    row = {"t": t, "life": life, "part": part, "src": -1,
                           "slot": i, "group": g, "tok": tok,
                           "sec": oi, "secname": name}
                    if tok == "X":
                        row["n"] = HIT
                        notes.append(row)
                        held = None
                        continue
                    m = ROOT_RE.match(tok)
                    if m:
                        row["n"] = root + (int(m.group(1)) if m.group(1) else 0) + transpose
                    else:
                        row["n"] = note_num(tok) + transpose
                    held = row
                    notes.append(row)


def build(s):
    """Turn a Score into notes, across the whole arrangement, in time order.

    Each note carries `sec`, its place in the order, so an editor can tell which
    turn of which section a note belongs to.
    """
    beat = 60.0 / s.tempo
    notes = []
    at = 0.0
    for oi, entry in enumerate(s.order):
        if not turn_on(entry):
            continue                       # skipped: no notes and no time
        name = turn_name(entry)
        sec = section(s, name)
        if sec is None:
            continue
        build_one(s, sec_parts(s, sec), sec_roots(s, sec), sec_reps(s, sec),
                  sec_transpose(s, sec), at, oi, name, notes)
        at += sec_beats(s, sec) * beat
    notes.sort(key=lambda x: (x["t"], x["part"].name))
    return notes


def speaker_check(wave, f, lp):
    """Say what the device speaker does with one note. Return (verdict, why)."""
    if lp > NYQUIST:
        return "BAD", "lp %.0f Hz is above the %d Hz limit of the sample rate" % (lp, NYQUIST)
    if wave == "sine":
        if f < SPEAKER_FLOOR:
            return "SILENT", "a sine under %.0f Hz has no harmonics to carry it" % SPEAKER_FLOOR
        return "ok", ""
    if wave == "square" and f < SPEAKER_FLOOR:
        if lp < 3.0 * f:
            return "SILENT", "lp %.0f Hz cuts the 3rd harmonic at %.0f Hz" % (lp, 3.0 * f)
        return "thin", "under %.0f Hz; the 3rd harmonic at %.0f Hz carries it" % (SPEAKER_FLOOR, 3.0 * f)
    return "ok", ""


def peak_voices(notes):
    """Return the largest number of notes that sound at the same time."""
    edges = []
    for n in notes:
        edges.append((n["t"], 1))
        edges.append((n["t"] + n["life"], -1))
    edges.sort(key=lambda e: (e[0], e[1]))
    live = peak = 0
    for _, d in edges:
        live += d
        peak = max(peak, live)
    return peak


def report(s, notes):
    beat = 60.0 / s.tempo
    total = song_beats(s) * beat
    bars = song_beats(s) / 4.0
    print("score      %s" % s.name)
    print("form       %s, %s, %.1f s at %g BPM"
          % (plural(len(s.order), "turn"), plural(round(bars, 4), "bar"),
             total, s.tempo))
    print("order      %s" % " ".join(s.order))
    if any(not turn_on(x) for x in s.order):
        print("skipped    %s"
              % " ".join(turn_name(x) for x in s.order if not turn_on(x)))
    print("roots      %s" % " ".join(note_name(r + s.transpose) for r in s.roots))
    if s.transpose:
        print("transpose  %+d semitones" % s.transpose)
    print("notes      %d" % len(notes))

    fixed = []

    def one_part(part, pad=""):
        off = "   MUTED, it does not sound" if part.mute else ""
        if part.kind == "free":
            print("%sfree %-8s %-6s %s, gain %.2f, lp %.0f Hz%s"
                  % (pad, part.name, part.wave, plural(len(part.events), "note"),
                     part.gain, part.lp, off))
            return
        slots = len(part.pattern)
        print("%spart %-8s %-6s %s, %.4g ms each, gain %.2f, lp %.0f Hz%s"
              % (pad, part.name, part.wave, plural(slots, "slot"),
                 s.cell * beat / max(1, slots) * 1000, part.gain, part.lp, off))
        # Only the parts that carry harmony. A noise part has no pitch, and a part
        # that sweeps is percussion, so neither one stands against the root.
        if part.wave == "noise" or part.sweep:
            return
        for tok in part.pattern:
            if tok not in (".", "-", "X") and not ROOT_RE.match(tok):
                if tok not in fixed:
                    fixed.append(tok)

    for part in s.parts:
        one_part(part)

    plain = (len(s.sections) == 1 and not s.sections[0].parts
             and s.sections[0].roots is None and s.sections[0].reps is None
             and s.sections[0].transpose is None)
    if not plain:
        print("")
        print("arrangement")
        for sec in s.sections:
            turns = [turn_name(x) for x in s.order].count(sec.name)
            extra = []
            if sec.transpose is not None:
                extra.append("transpose %+d" % sec.transpose)
            if sec.reps is not None:
                extra.append("reps %d" % sec.reps)
            if sec.roots:
                extra.append("roots " + " ".join(note_name(r) for r in sec.roots))
            print("  section %-9s %-8s played %s%s"
                  % (sec.name, plural(sec_beats(s, sec) / 4.0, "bar"),
                     plural(turns, "time"),
                     (", " + ", ".join(extra)) if extra else ""))
            for part in sec.parts:
                one_part(part, "    ")

    if fixed and len(s.roots) > 1:
        print("")
        print("harmony    the fixed notes are %s. The root moves under them."
              % " ".join(fixed))
        head = "  %-6s" % "root"
        for tok in fixed:
            head += "  %-22s" % ("against %s" % tok)
        head += "  gap moves by"
        print(head)
        prev = None
        for r in s.roots:
            row = "  %-6s" % note_name(r + s.transpose)
            for tok in fixed:
                d = note_num(tok) - r
                row += "  %-22s" % ("%2d st  %s" % (d, IVAL[d] if 0 <= d <= 12 else "wide"))
            gap = note_num(fixed[0]) - r
            row += "  %s" % ("start" if prev is None else "%+d" % (gap - prev))
            prev = gap
            print(row)

    peak = peak_voices(audible(notes))
    print("")
    print("voices     peak %d of 10 at once" % peak)
    if peak > 4:
        print("  WARNING: vg_synth has 10 voices and no priority. It takes the voice")
        print("  with the least time left. Above 4 voices the music starves an alert.")

    print("")
    print("speaker    the device driver gives very little under %.0f Hz" % SPEAKER_FLOOR)
    seen = {}
    for n in audible(notes):
        seen.setdefault((n["part"].name, n["n"]), n)
    bad = 0
    for (pname, num), n in sorted(seen.items(), key=lambda kv: -kv[0][1]):
        part = n["part"]
        f = hz(num)
        verdict, why = speaker_check(part.wave, f, part.lp)
        if verdict != "ok":
            bad += 1
        # A noise part has no pitch, so a note name for it would be a lie.
        if part.wave == "noise":
            print(("  %-8s %-5s %7s     %-7s %s" % (pname, "hit", "-", verdict, why)).rstrip())
        elif part.sweep:
            print(("  %-8s %-5s %7.2f Hz  %-7s falls to %.0f Hz. %s"
                   % (pname, note_name(num), f, verdict, hz_end(part, num), why)).rstrip())
        else:
            print(("  %-8s %-5s %7.2f Hz  %-7s %s" % (pname, note_name(num), f, verdict, why)).rstrip())
    if bad:
        print("  %d of %d notes are weak or silent on the device." % (bad, len(seen)))
        print("  To fix this, lift the notes an octave. Add this line: transpose 12")


def write_mid(s, notes, path):
    """Write a MIDI file. One track for each part."""
    def vlq(n):
        b = [n & 0x7F]
        n >>= 7
        while n:
            b.append((n & 0x7F) | 0x80)
            n >>= 7
        return bytes(reversed(b))

    def chunk(ev):
        return b"MTrk" + struct.pack(">I", len(ev)) + ev

    beat = 60.0 / s.tempo
    tracks = []
    ev = vlq(0) + b"\xff\x51\x03" + int(60000000 / s.tempo).to_bytes(3, "big")
    ev += vlq(0) + b"\xff\x58\x04" + bytes([4, 2, 24, 8])
    ev += vlq(0) + b"\xff\x2f\x00"
    tracks.append(chunk(ev))

    for ch, part in enumerate(s.parts):
        mine = [n for n in notes if n["part"] is part]
        stamps = []
        for n in mine:
            stamps.append((int(round(n["t"] / beat * PPQ)), 0x90 | (ch & 15), n["n"], 100))
            stamps.append((int(round((n["t"] + n["life"]) / beat * PPQ)), 0x80 | (ch & 15), n["n"], 0))
        stamps.sort(key=lambda x: (x[0], x[1] & 0xF0))
        ev = vlq(0) + b"\xff\x03" + vlq(len(part.name)) + part.name.encode()
        ev += vlq(0) + bytes([0xC0 | (ch & 15), 0])
        last = 0
        for t, st, a, b in stamps:
            ev += vlq(t - last) + bytes([st, a, b])
            last = t
        ev += vlq(0) + b"\xff\x2f\x00"
        tracks.append(chunk(ev))

    head = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), PPQ)
    with open(path, "wb") as f:
        f.write(head + b"".join(tracks))
    print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))


def bake(s, notes, spec_path):
    """Write the table that the device plays. A muted part is left out."""
    tag = re.sub(r"[^A-Za-z0-9]", "_", s.name).upper()
    muted = [p.name for p in s.parts if p.mute]
    notes = audible(notes)
    rows, index = [], {}
    for n in notes:
        key = (n["part"].name, n["n"], round(n["life"], 4))
        if key not in index:
            index[key] = len(rows)
            rows.append(n)
    if len(rows) > 255:
        sys.exit("this score needs %d rows and the step index holds 255" % len(rows))

    seconds = song_beats(s) * 60.0 / s.tempo
    out = []
    out.append("// GENERATED by tools/score.py from %s -- do not edit."
               % rel(spec_path))
    out.append("//")
    out.append("// %d notes, %.1f seconds, peak %d of 10 voices."
               % (len(notes), seconds, peak_voices(notes)))
    if muted:
        out.append("//")
        out.append("// WARNING: these parts were MUTED and are NOT in this table: %s"
                   % ", ".join(muted))
    out.append("//")
    out.append("// The player must not read the seeded random stream and must not set any")
    out.append("// state that the simulation reads. vg_synth.h states that rule, and the")
    out.append("// replay capture depends on it.")
    out.append("#pragma once")
    out.append('#include "../vg_synth.h"')
    out.append("")
    out.append("#ifndef VG_SCORE_STEP")
    out.append("#define VG_SCORE_STEP")
    out.append("// One note that plays, at t_ms from the start of the score.")
    out.append("//")
    out.append("// t_ms IS 32 BIT and must stay that way. It was 16, which stops at 65.5")
    out.append("// seconds, and a longer score wrapped round and played its second half")
    out.append("// over its first.")
    out.append("struct VgScoreStep { uint32_t t_ms; uint8_t note; };")
    out.append("#endif")
    out.append("")
    out.append("// One row for each different note. f0 and f1 differ when the part sweeps,")
    out.append("// which is how a drum gets its weight out of a small speaker.")
    out.append("//              wave        f0      f1     life    atk     sus    gain   lp_hz  delay  mod  depth")
    out.append("static const SynthLayer SCORE_%s_NOTES[] = {" % tag)
    for n in rows:
        p = n["part"]
        f, f1 = hz(n["n"]), hz_end(p, n["n"])
        out.append("    { %-11s %7.2f, %7.2f, %6.3ff, %.4ff, %.2ff, %.2ff, %6.0f, 0, %4.0f, %.2ff },"
                   "  // %s %s"
                   % (WAVES[p.wave] + ",", f, f1, n["life"], p.atk, p.sus, p.gain, p.lp,
                      p.mod, p.depth, p.name,
                      "hit" if p.wave == "noise" else note_name(n["n"])))
    out.append("};")
    out.append("")
    out.append("// One row for each note that plays, in time order.")
    out.append("static const VgScoreStep SCORE_%s_STEPS[] = {" % tag)
    for i in range(0, len(notes), 6):
        line = "    "
        for n in notes[i:i + 6]:
            key = (n["part"].name, n["n"], round(n["life"], 4))
            line += "{%6d,%3d}, " % (int(round(n["t"] * 1000)), index[key])
        out.append(line.rstrip())
    out.append("};")
    out.append("")
    out.append("#define SCORE_%s_LEN_MS %d" % (tag, int(round(seconds * 1000))))
    out.append("")

    path = os.path.join(GEN, "score_%s.h" % s.name.lower())
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    if muted:
        print("WARNING: %s muted and left out: %s"
              % (plural(len(muted), "part"), ", ".join(muted)))
    print("wrote %s (%d rows, %d steps, %d bytes)"
          % (rel(path), len(rows), len(notes), os.path.getsize(path)))


def read_mid(path):
    """Read a MIDI file and print a score file."""
    data = open(path, "rb").read()
    if data[:4] != b"MThd":
        sys.exit("%s is not a MIDI file" % path)
    ntrk, div = struct.unpack(">HH", data[10:14])
    if div & 0x8000:
        sys.exit("this tool reads ticks for each beat. %s uses SMPTE time." % path)
    p = 8 + struct.unpack(">I", data[4:8])[0]
    tempo, played = 500000, []
    for _ in range(ntrk):
        tlen = struct.unpack(">I", data[p + 4:p + 8])[0]
        q, end, tick, status, on = p + 8, p + 8 + tlen, 0, None, {}
        while q < end:
            d = 0
            while True:
                b = data[q]
                q += 1
                d = (d << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            tick += d
            if data[q] & 0x80:
                status = data[q]
                q += 1
            if status == 0xFF:
                mt = data[q]
                q += 1
                L = 0
                while True:
                    b = data[q]
                    q += 1
                    L = (L << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                if mt == 0x51:
                    tempo = int.from_bytes(data[q:q + L], "big")
                q += L
            elif status in (0xF0, 0xF7):
                L = 0
                while True:
                    b = data[q]
                    q += 1
                    L = (L << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                q += L
            else:
                hi = status & 0xF0
                n = 1 if hi in (0xC0, 0xD0) else 2
                pl = data[q:q + n]
                q += n
                if hi == 0x90 and pl[1] > 0:
                    on[pl[0]] = tick
                elif hi == 0x80 or (hi == 0x90 and pl[1] == 0):
                    if pl[0] in on:
                        start = on.pop(pl[0])
                        played.append((start, pl[0], tick - start))
        p = end
    if not played:
        sys.exit("%s has no notes" % path)

    played.sort()
    bpm = 60000000.0 / tempo
    starts = sorted({t for t, _, _ in played})
    gaps = [b - a for a, b in zip(starts, starts[1:])]
    step = min(gaps) if gaps else div
    seq = [n for _, n, _ in played]
    durs = sorted(d for _, _, d in played)
    gate = durs[len(durs) // 2] / float(step) if step else 0.6

    # Find the group. In this music some notes move with the root and some hold
    # still. Take the first note of each block as the root. A block length is
    # correct when every position either keeps the same distance from the root in
    # every block, or holds the same pitch in every block.
    cell, groups, kind = len(seq), None, None
    for c in range(2, len(seq) // 2 + 1):
        if len(seq) % c:
            continue
        blocks = [seq[i:i + c] for i in range(0, len(seq), c)]
        shape = []
        for i in range(c):
            deltas = {b[i] - b[0] for b in blocks}
            values = {b[i] for b in blocks}
            if len(deltas) == 1:
                shape.append(("root", deltas.pop()))
            elif len(values) == 1:
                shape.append(("fixed", values.pop()))
            else:
                shape = None
                break
        if shape:
            cell, groups, kind = c, [b[0] for b in blocks], shape
            break

    base = os.path.basename(path).lower().split(".")[0]
    print("# Made by tools/score.py --from-mid from %s" % os.path.basename(path))
    print("# %d notes at %.4g BPM. Look at the roots and reps before you use this."
          % (len(seq), bpm))
    print("")
    print("name        %s" % (re.sub(r"[^a-z0-9]", "", base) or "imported"))
    print("tempo       %g" % round(bpm, 3))
    print("cell        %g" % (cell * step / float(div)))
    if groups is None:
        print("reps        1")
        print("roots       %s" % note_name(seq[0]))
        print("")
        print("part lead  square gain 0.26 lp 2800 gate %.2f sus 0.20 : %s"
              % (gate, " ".join(note_name(n) for n in seq)))
        return

    roots = [groups[0]]
    for g in groups[1:]:
        if g != roots[-1]:
            roots.append(g)
    reps = len(groups) // len(roots)
    if reps * len(roots) != len(groups):
        reps, roots = 1, groups
    print("reps        %d" % reps)
    print("roots       %s" % " ".join(note_name(r) for r in roots))
    print("")
    toks = ["R" if d == 0 else "R%+d" % d if k == "root" else note_name(d)
            for k, d in kind]
    print("part lead  square gain 0.26 lp 2800 gate %.2f sus 0.20 : %s"
          % (gate, " ".join(toks)))


def main():
    ap = argparse.ArgumentParser(description="Build the game's music from a score file.")
    ap.add_argument("spec", nargs="?", help="the score file to read")
    ap.add_argument("--mid", metavar="PATH", help="write a MIDI file to listen to")
    ap.add_argument("--bake", action="store_true", help="write the table for the device")
    ap.add_argument("--from-mid", metavar="PATH", help="read a MIDI file and print a score file")
    ap.add_argument("--transpose", type=int, metavar="N", help="move every note N semitones")
    a = ap.parse_args()

    if a.from_mid:
        read_mid(a.from_mid)
        return
    if not a.spec:
        ap.error("give a score file, or use --from-mid")

    s = read_score(a.spec)
    if a.transpose is not None:
        s.transpose = a.transpose
    notes = build(s)
    report(s, notes)
    if a.mid:
        print("")
        write_mid(s, notes, a.mid)
    if a.bake:
        print("")
        bake(s, notes, a.spec)


if __name__ == "__main__":
    main()
