#!/usr/bin/env python3
"""Render a score to sound with the game's own synthesiser.

    python tools/score_audio.py design/score/motif.score
    python tools/score_audio.py design/score/motif.score --speaker
    python tools/score_audio.py design/score/motif.score -o out.wav --no-play

There is no synthesiser in this file. This module builds tools/host/, which
compiles src/vg/vg_synth.cpp, the same file the firmware runs. One copy of the
synthesiser, built twice. tools/README.md gives the reason.

The build needs the C++ tools of Visual Studio. The build runs once. After that
this module rebuilds only when a source file changes.

Two kinds of sound come out.

- The plain render is exact. It is what the firmware synthesiser makes.
- `--speaker` adds a filter that APPROXIMATES the device driver. The driver is
  one centimetre across and gives very little under about 300 Hz. Nobody
  measured it to make this filter. Use it to find notes that the device loses.
  Do not use it to judge level or tone.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import score

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "host")
EXE = os.path.join(HOST, "score_render.exe")        # writes a WAV file
EXE_LIVE = os.path.join(HOST, "score_live.exe")     # plays through the sound card
BAT = os.path.join(HOST, "build.bat")
SRC = [os.path.join(HOST, "score_render.cpp"),
       os.path.join(HOST, "score_live.cpp"),
       os.path.join(HOST, "speaker.h"),
       os.path.join(os.path.dirname(HERE), "src", "vg", "vg_synth.cpp")]

WAVE_ID = {"square": 0, "noise": 1, "sine": 2}
MIX = 0.49              # vg_game.cpp sets music to 0.70, and vg_synth squares it
SPEAKER_CORNER = 300.0  # the floor in vg_sfx.cpp, as a filter corner


class BuildError(Exception):
    pass


def stale():
    """Return True if either program needs a build."""
    for exe in (EXE, EXE_LIVE):
        if not os.path.exists(exe):
            return True
        made = os.path.getmtime(exe)
        if any(os.path.getmtime(p) > made for p in SRC if os.path.exists(p)):
            return True
    return False


def build(force=False):
    """Build the renderer if it is out of date. Return the path of the program."""
    if not force and not stale():
        return EXE
    if not os.path.exists(BAT):
        raise BuildError("cannot find %s" % BAT)
    os.makedirs(os.path.join(HOST, "obj"), exist_ok=True)
    os.makedirs(os.path.join(HOST, "objlive"), exist_ok=True)
    r = subprocess.run([BAT], cwd=HOST, capture_output=True, text=True, shell=True)
    if r.returncode != 0 or not os.path.exists(EXE) or not os.path.exists(EXE_LIVE):
        raise BuildError("the build failed.\n%s\n%s" % (r.stdout[-2000:], r.stderr[-2000:]))
    return EXE


def write_job(s, notes, path, speaker=False, mix=MIX, tail_ms=900, loop=False):
    """Write the job file that the renderer reads.

    A loop needs an exact end. The score stops at the end of the last group, and
    the file must stop there too. A tail would put a gap in the loop, and a note
    that runs past the end would sound over the start, so both are cut.
    """
    beat = 60.0 / s.tempo
    notes = score.audible(notes)      # a muted part is drawn, never heard
    end = score.song_beats(s) * beat
    if loop:
        tail_ms = 0
    else:
        for n in notes:
            end = max(end, n["t"] + n["life"])
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("rate %d\n" % score.AUDIO_RATE)
        f.write("mix %.4f\n" % mix)
        f.write("len_ms %d\n" % int(round(end * 1000)))
        f.write("tail_ms %d\n" % tail_ms)
        f.write("speaker %d\n" % (1 if speaker else 0))
        f.write("loop %d\n" % (1 if loop else 0))
        f.write("corner %.1f\n" % SPEAKER_CORNER)
        for n in sorted(notes, key=lambda x: x["t"]):
            p = n["part"]
            hz = score.hz(n["n"])
            hz1 = score.hz_end(p, n["n"])       # the pitch it falls to, for a drum
            f.write("note %d %d %.4f %.4f %.5f %.5f %.4f %.4f %.1f 0 %.2f %.4f\n"
                    % (int(round(n["t"] * 1000)), WAVE_ID[p.wave], hz, hz1,
                       n["life"], p.atk, p.sus, p.gain, p.lp, p.mod, p.depth))
    return path


def render(s, notes, wav, speaker=False, mix=MIX, loop=False):
    """Render a score to a WAV file. Return the text the renderer printed."""
    exe = build()
    job = os.path.join(tempfile.gettempdir(), "phantom_score_job.txt")
    write_job(s, notes, job, speaker=speaker, mix=mix, loop=loop)
    r = subprocess.run([exe, job, wav], capture_output=True, text=True)
    if r.returncode != 0:
        raise BuildError("the renderer failed.\n%s\n%s" % (r.stdout, r.stderr))
    return r.stdout.strip()


def play(wav, loop=False):
    """Play a WAV file and return at once. Windows only.

    Windows does the loop itself, so there is no gap between the end and the
    start. stop() ends it.
    """
    import winsound
    flags = winsound.SND_FILENAME | winsound.SND_ASYNC
    if loop:
        flags |= winsound.SND_LOOP
    winsound.PlaySound(wav, flags)


def stop():
    """Stop the sound that plays now."""
    import winsound
    winsound.PlaySound(None, winsound.SND_PURGE)


def main():
    ap = argparse.ArgumentParser(description="Render a score with the game's synthesiser.")
    ap.add_argument("spec", help="the score file to read")
    ap.add_argument("-o", "--out", metavar="PATH", help="where to write the WAV file")
    ap.add_argument("--speaker", action="store_true",
                    help="add the filter that approximates the device driver")
    ap.add_argument("--loop", action="store_true", help="play it again and again")
    ap.add_argument("--no-play", action="store_true", help="write the file and do not play it")
    ap.add_argument("--rebuild", action="store_true", help="build the renderer again")
    a = ap.parse_args()

    if a.rebuild:
        build(force=True)
        print("built %s" % os.path.relpath(EXE, os.path.dirname(HERE)).replace("\\", "/"))

    s = score.read_score(a.spec)
    notes = score.build(s)
    wav = a.out or os.path.join(tempfile.gettempdir(), "phantom_score.wav")
    try:
        print(render(s, notes, wav, speaker=a.speaker, loop=a.loop))
    except BuildError as e:
        sys.exit(str(e))
    print("wrote %s" % wav)
    if not a.no_play:
        play(wav, loop=a.loop)
        input("playing. Press Enter to stop. ")
        stop()


if __name__ == "__main__":
    main()
