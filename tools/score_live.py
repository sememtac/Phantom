#!/usr/bin/env python3
"""Play a score LIVE, and change it while it plays.

    python tools/score_live.py design/score/example-beats.score

This module talks to tools/host/score_live.exe, which compiles
src/vg/vg_synth.cpp and runs it in real time. There is no synthesiser here and
none in the engine either. There is one copy, built twice. tools/README.md gives
the reason.

score_audio.py is the other way to hear a score. It renders a WAV file, which is
right for an export and wrong for work: every change means a new file and a new
start. This module keeps the sound running.

Three things happen without a stop.

- `set_mute` turns a part off or on. The engine holds every note of every part
  and decides at the moment each note starts, so nothing is rendered again.
- `load` gives the engine a new set of notes and keeps the position, so an edit
  reaches the next turn of the loop.
- `hit` sounds one note now. Use it to hear a note the moment it is clicked.

WARNING: the synthesiser has 10 voices. A preview note takes one, the same as a
note of the score. The device behaves the same way, so what you hear is honest.
"""
import os
import subprocess
import sys
import threading

import score
import score_audio

WAVE_ID = {"square": 0, "noise": 1, "sine": 2}


class Live:
    """The live engine. Start it once and keep it."""

    def __init__(self):
        self.proc = None
        self.alive = False
        self.pos_ms = -1
        self.going = False       # True while the engine is actually playing
        self.parts = []          # the part list the engine was loaded with

    # ------------------------------------------------------------------ control

    def start(self):
        """Build the engine if it is out of date, then start it."""
        if self.alive:
            return True
        score_audio.build()
        if not os.path.exists(score_audio.EXE_LIVE):
            raise score_audio.BuildError("cannot find %s" % score_audio.EXE_LIVE)
        self.proc = subprocess.Popen(
            [score_audio.EXE_LIVE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.alive = True
        threading.Thread(target=self._read, daemon=True).start()
        return True

    def _read(self):
        """Keep the position the engine reports. It prints it ten times a second."""
        try:
            for line in self.proc.stdout:
                if line.startswith("pos "):
                    try:
                        bits = line.split()
                        self.pos_ms = int(bits[1])
                        if len(bits) > 2:
                            self.going = bits[2] == "1"
                    except (ValueError, IndexError):
                        pass
        except (OSError, ValueError):
            pass
        self.alive = False

    def send(self, line):
        """Send one command. `line` may hold several, separated by newlines."""
        if not self.alive:
            return
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError):
            self.alive = False

    def close(self):
        if self.alive:
            self.send("quit")
            try:
                self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()
        self.alive = False

    # -------------------------------------------------------------------- score

    def load(self, s, notes):
        """Give the engine every note, muted parts included.

        The muted ones go too. The engine decides at the moment a note starts,
        which is what lets a part be turned off with the sound still running.
        """
        if not self.alive:
            return
        # Every part, base and section, so a note of a section part is keyed
        # correctly. Keying by s.parts alone sent them all to part 0.
        self.parts = score.all_parts(s)
        index = {id(p): i for i, p in enumerate(self.parts)}
        rows = []
        for n in sorted(notes, key=lambda x: x["t"]):
            p = n["part"]
            rows.append("%d %d %d %.4f %.4f %.5f %.5f %.4f %.4f %.1f %.2f %.4f"
                        % (int(round(n["t"] * 1000)), index.get(id(p), 0),
                           WAVE_ID[p.wave], score.hz(n["n"]), score.hz_end(p, n["n"]),
                           n["life"], p.atk, p.sus, p.gain, p.lp, p.mod, p.depth))
        # ONE write for the whole score. A write and a flush for every note was
        # hundreds of trips through the pipe for a long song.
        beat = 60.0 / s.tempo
        out = ["load %d" % len(rows)] + rows
        out.append("len %d" % int(round(score.song_beats(s) * beat * 1000)))
        out.extend("mute %d %d" % (i, 1 if p.mute else 0)
                   for i, p in enumerate(self.parts))
        self.send("\n".join(out))

    def set_mute(self, i, off):
        self.send("mute %d %d" % (i, 1 if off else 0))

    def set_loop(self, on):
        self.send("loop %d" % (1 if on else 0))

    def set_speaker(self, on):
        self.send("speaker %d" % (1 if on else 0))

    def set_mix(self, mix):
        self.send("mix %.4f" % mix)

    def play(self):
        self.going = True
        self.send("play")

    def stop(self):
        """Silence the sound and KEEP the position, which is a pause. Use seek(0)
        as well to go back to the start."""
        self.going = False
        self.send("stop")

    def seek(self, ms):
        self.pos_ms = int(ms)     # so the playhead moves before the engine answers
        self.send("seek %d" % int(ms))

    def hit(self, part, midi):
        """Sound one note now, with the settings of its part."""
        life = max(0.08, min(1.2, 0.35))
        self.send("hit %d %.4f %.4f %.5f %.5f %.4f %.4f %.1f %.2f %.4f"
                  % (WAVE_ID[part.wave], score.hz(midi), score.hz_end(part, midi),
                     life, part.atk, part.sus, part.gain, part.lp,
                     part.mod, part.depth))


def main():
    if len(sys.argv) < 2:
        sys.exit("give a score file")
    s = score.read_score(sys.argv[1])
    live = Live()
    live.start()
    live.load(s, score.build(s))
    live.set_loop(True)
    live.play()
    print("playing %s. Type a part number to turn it off or on. Enter to stop."
          % sys.argv[1])
    for i, p in enumerate(s.parts):
        print("   %d  %s" % (i, p.name))
    off = set()
    while True:
        try:
            line = input("> ").strip()
        except EOFError:
            break
        if not line:
            break
        if line.isdigit() and int(line) < len(s.parts):
            i = int(line)
            if i in off:
                off.discard(i)
            else:
                off.add(i)
            live.set_mute(i, i in off)
            print("   %s is %s" % (s.parts[i].name, "off" if i in off else "on"))
    live.close()


if __name__ == "__main__":
    main()
