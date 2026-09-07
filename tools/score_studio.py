#!/usr/bin/env python3
"""Write the game's music in a window.

    python tools/score_studio.py
    python tools/score_studio.py design/score/motif.score

The window shows the notes on a grid. You can add a note, move a note and remove
a note. You can play the score with the game's own synthesiser.

There are two kinds of part.

- A PATTERN part repeats with the groups. Its slots hold R for the root of the
  group. Use it for the parts that follow the roots.

A slot of a pattern part is one of two things, and the colour on the grid says
which. Drag a GREEN note to move the root of its group, and only that group
moves. Drag a BLUE note to change that slot in EVERY group, because all the
groups share one pattern.
- A FREE part does not repeat. Each note has its own beat and length. Use it for
  a melody.

The part list shows the kind of each part. Pick a part, then edit on the grid.
Each part has a box beside it. Clear the box to turn the part off and hear the
rest without it. That is for listening: it changes no note, makes no undo step,
and is not written to the file.

Drag a box along the song strip to move that turn to another place in the order.
`Skip` keeps a turn in its place but does not play it, for holding a section back.

`New turn` adds a turn playing a new, empty section: the base parts play in it
and its free parts are blank. `Duplicate` copies the section of this turn and
plays the copy next. The copy
gets its OWN melody, so you can rewrite it without touching the section it came
from. The beats and the lead stay shared. `Repeat` plays the SAME section again,
and an edit then changes every turn of it.

`Add` adds a part from a preset. A PRESET GIVES THE SOUND AND NOTHING ELSE: the
part arrives empty, and the box beside the preset chooses its SHAPE. A `free`
part repeats nothing and every note stands on its own. A `pattern` part is a cell
that repeats in every group. Take `free` unless you want the repeat.

`New` starts a blank song. `Copy this part, to make a variant` copies the part
you picked, notes and all, so a second lead or a second set of drums can be built
from one that works. `groups` and `reps` set how long a turn is.

`Stop this part repeating` writes a pattern part out as separate notes, so it can
be edited one note at a time. A new part goes in the base; `Make this part local
to the section` narrows it to one. `Remove`, or a right click on a part row,
removes the part you picked. Undo puts it back.

`slots in a group` sets the timing grid of a pattern part. Each part has its own
count, so a drum can run in sixteenths while the bass runs in quarters.

The sound comes from src/vg/vg_synth.cpp, the file the firmware runs. See
tools/score_audio.py. `Play as device` adds a filter that APPROXIMATES the
device speaker. That filter is a guess with the right shape. It is not a
measurement, so do not judge level or tone from it.

Mouse:
    left click on an empty cell     add a note
    left drag on a note             move the note
    left drag on its right edge     hold the note longer
    middle click on the grid        put the playhead there
    right drag across the grid      pick every note in the box
    left drag on a picked note      move every picked note together
    shift and that drag             keep the pitches, move in time only
    right click on a note           remove the note
    control and the mouse wheel     zoom

The note names are in a canvas of their own and do not scroll sideways, so they
stay in view however far along the song you are.

Keys:
    space                           play, and again to pause
    backspace, delete               remove the picked notes
    control and c                   copy the picked notes
    control and v                   paste them at the playhead
    control and z                   undo
    control and y                   redo

The tempo slider changes the speed. The notes keep their beats, so nothing moves.
The volume slider under the part list changes the volume of the part you picked.

`Loop` plays the score again and again. A loop stops at the end of the last
group, with no tail, so that there is no gap.
"""
import copy
import os
import sys
import tempfile
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score
import score_audio
import score_live

BG = "#14161a"
GRID = "#242832"
GRID_BAR = "#39404f"
GRID_GROUP = "#5a6478"
BLACK_ROW = "#191c22"
TEXT = "#c8cede"
DIM = "#7b8496"
PATTERN_FILL = "#3d6ea5"      # a slot written as a note name: fixed in every group
PATTERN_EDGE = "#6ea3dd"
ROOT_FILL = "#3f7d6a"         # a slot written as R: it follows the root of its group
ROOT_EDGE = "#6fc0a4"
FREE_FILL = "#b5713a"
FREE_EDGE = "#e6a163"
SEL_EDGE = "#ffffff"
MUTE_FILL = "#2a2f38"         # a part turned off: drawn, but it does not sound
MUTE_EDGE = "#4a515e"
ROW_SEL = "#2f3b4d"
TURN_BG = "#232a36"           # a turn of the arrangement, in the strip
TURN_SEL = "#3d6ea5"
SEC_LINE = "#8a94a8"
# One colour for each section. Two turns of the SAME section share a colour, so
# the eye can see at once which turns move together when one of them is edited.
SECTION_COLOURS = ["#3d6ea5", "#7a5aa8", "#3f7d6a", "#a5763d",
                   "#a53d5e", "#3d8fa5", "#7d8a3d", "#8a4a3d"]
LANE_BG = "#1b2029"           # the strip a noise part is drawn on
NOISE_FILL = "#7a5aa8"        # a hit with no pitch
NOISE_EDGE = "#a98ad4"
PLAYHEAD = "#e05a5a"
WARN = "#e0a54a"
BAD = "#e05a5a"
OK = "#6fbf73"

LEFT_W = 54            # width of the note names on the left
ROW_H = 13             # height of one semitone
RULER_H = 15           # the band of times, in the frozen bar along the top
HEAD_H = 20            # the band of turn names, under the times
TOPBAR_H = RULER_H + HEAD_H
# The grid canvas holds notes and nothing else, so it starts at its own top. The
# times and the turn names live in a canvas that does not scroll up and down,
# the same way the note names live in one that does not scroll sideways.
TOP_H = 0
EDGE_PX = 6            # how near the right edge a drag makes a note longer
UNDO_DEPTH = 100       # how many steps back undo can go
SHIFT = 0x0001         # the shift bit in the state of a mouse event
MARQUEE_PX = 4         # a right press that moves less than this is a plain click

# The grid always covers this range of pitches, C1 to C7. It used to stop a few
# semitones above the highest note that was already written, and then there was
# nowhere to click for a higher one. A fixed range also means the rows never move
# under the pointer during a drag.
LOW_NOTE = 24          # C1
HIGH_NOTE = 96         # C7


def blend(a, b, t):
    """Mix two colours. t of 0 gives a, t of 1 gives b."""
    pa = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    pb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(x + (y - x) * t) for x, y in zip(pa, pb))


def rel(path):
    """Return a short path for a message. Another drive has no short path."""
    try:
        return os.path.relpath(path, score.ROOT).replace("\\", "/")
    except ValueError:
        return path


class Studio:
    def __init__(self, root, path=None):
        self.root = root
        self.path = None
        self.score = None
        self.notes = []
        self.sel_part = 0
        self.sel_note = None
        self.drag = None
        self.undo_stack = []
        self.redo_stack = []
        self.pending = None    # a copy kept while a drag runs. See hold().
        self.low = None        # the lowest pitch drawn. See rebuild.
        self.high = None
        self.rows = 0
        self.px_per_beat = 64.0
        self.playing = False
        self.head_ms = 0.0     # where the playhead is, playing or not
        self.play_len = 0.0
        self.wav = os.path.join(tempfile.gettempdir(), "phantom_studio.wav")
        self.speaker = tk.BooleanVar(value=False)
        self.length = tk.StringVar(value="1")
        self.loop = tk.BooleanVar(value=False)
        self.last_device = False
        self.tempo_var = tk.DoubleVar(value=120.0)
        self.gain_var = tk.DoubleVar(value=0.25)
        self.syncing = False       # True while the code sets a slider, not the user
        self.slider_moved = False
        # The live engine. It keeps the sound running while the score changes, so
        # a part can go off and a note can be heard with no stop and no re-render.
        self.live = score_live.Live()
        self.audition = tk.BooleanVar(value=True)
        self.slots_var = tk.StringVar(value="4")
        self.kind_var = tk.StringVar(value="free")
        self.groups_var = tk.StringVar(value="4")
        self.reps_var = tk.StringVar(value="4")
        self.sel_turn = 0          # which place in the order is being edited
        self.turn_rows = []
        self.turn_drag = None      # a turn being dragged along the strip
        self.sel_notes = set()     # keys of the notes picked with a right drag
        self.marquee = None
        self.clip = []             # copied notes: (part name, beat, pitch, length)
        self.build_ui()
        self.load(path or os.path.join(score.ROOT, "design", "score", "motif.score"))
        # START THE SOUND ENGINE NOW, not on the first Play. Opening the sound
        # card takes a moment, and doing it here means the first Play is instant.
        # It runs after the window is up, so a build never holds the window back.
        self.root.after(120, self.start_engine)
        # After the window is mapped, or the window manager takes it back.
        self.root.after(150, self.canvas.focus_set)

    # ---------------------------------------------------------------- the window

    def build_ui(self):
        self.root.title("Phantom score studio")
        self.root.configure(bg=BG)
        self.root.geometry("1180x760")

        bar = tk.Frame(self.root, bg=BG)
        bar.pack(side="top", fill="x", padx=8, pady=6)
        for label, cmd in (("New", self.on_new), ("Open", self.on_open),
                           ("Save", self.on_save), ("Save as", self.on_save_as)):
            tk.Button(bar, text=label, command=cmd, width=8).pack(side="left", padx=2)
        self.undo_btn = tk.Button(bar, text="Undo", command=self.on_undo,
                                  width=6, state="disabled")
        self.undo_btn.pack(side="left", padx=(10, 2))
        self.redo_btn = tk.Button(bar, text="Redo", command=self.on_redo,
                                  width=6, state="disabled")
        self.redo_btn.pack(side="left", padx=2)
        tk.Label(bar, text=" ", bg=BG).pack(side="left")
        tk.Button(bar, text="Play", command=lambda: self.on_play(False),
                  width=8).pack(side="left", padx=2)
        tk.Button(bar, text="Play as device", command=lambda: self.on_play(True),
                  width=14).pack(side="left", padx=2)
        tk.Button(bar, text="Pause", command=self.on_pause, width=6).pack(side="left", padx=2)
        tk.Button(bar, text="Stop", command=self.on_stop, width=6).pack(side="left", padx=2)
        tk.Checkbutton(bar, text="Hear clicks", variable=self.audition,
                       bg=BG, fg=TEXT, activebackground=BG, activeforeground=TEXT,
                       selectcolor=GRID, highlightthickness=0,
                       bd=0).pack(side="left", padx=6)
        tk.Checkbutton(bar, text="Loop", variable=self.loop, command=self.on_loop,
                       bg=BG, fg=TEXT, activebackground=BG, activeforeground=TEXT,
                       selectcolor=GRID, highlightthickness=0, bd=0).pack(side="left", padx=6)
        tk.Button(bar, text="Bake", command=self.on_bake, width=6).pack(side="left", padx=8)

        bar2 = tk.Frame(self.root, bg=BG)
        bar2.pack(side="top", fill="x", padx=8, pady=(0, 6))

        tk.Label(bar2, text="tempo", bg=BG, fg=DIM).pack(side="left", padx=(2, 2))
        tempo = tk.Scale(bar2, variable=self.tempo_var, from_=40, to=200, resolution=1,
                         orient="horizontal", length=120, showvalue=False,
                         command=self.on_tempo, bg=BG, fg=TEXT, troughcolor=GRID,
                         highlightthickness=0, bd=0, sliderrelief="flat")
        tempo.pack(side="left")
        tempo.bind("<Button-1>", self.slider_press, add="+")
        tempo.bind("<ButtonRelease-1>", self.slider_release, add="+")
        self.tempo_label = tk.Label(bar2, text="120 BPM", bg=BG, fg=TEXT, width=8,
                                    anchor="w")
        self.tempo_label.pack(side="left", padx=(4, 0))

        tk.Label(bar2, text="groups", bg=BG, fg=DIM).pack(side="left", padx=(18, 2))
        self.groups_box = ttk.Combobox(bar2, textvariable=self.groups_var, width=3,
                                       state="readonly",
                                       values=tuple(str(i) for i in range(1, 13)))
        self.groups_box.pack(side="left")
        self.groups_box.bind("<<ComboboxSelected>>", self.on_groups)
        tk.Label(bar2, text="reps", bg=BG, fg=DIM).pack(side="left", padx=(10, 2))
        self.reps_box = ttk.Combobox(bar2, textvariable=self.reps_var, width=3,
                                     state="readonly",
                                     values=("1", "2", "3", "4", "6", "8"))
        self.reps_box.pack(side="left")
        self.reps_box.bind("<<ComboboxSelected>>", self.on_reps)

        tk.Label(bar2, text="new note length (beats)", bg=BG, fg=DIM).pack(side="left", padx=(14, 4))
        ttk.Combobox(bar2, textvariable=self.length, width=5, state="readonly",
                     values=("0.25", "0.5", "1", "2", "4")).pack(side="left")
        for text, colour in (("free", FREE_EDGE), ("fixed", PATTERN_EDGE),
                             ("follows root", ROOT_EDGE)):
            tk.Label(bar2, text="■ " + text, bg=BG, fg=colour).pack(side="right", padx=(8, 0))

        tk.Button(bar2, text="-", command=lambda: self.zoom(1 / 1.25), width=3).pack(side="right", padx=2)
        tk.Button(bar2, text="+", command=lambda: self.zoom(1.25), width=3).pack(side="right", padx=2)
        tk.Label(bar2, text="zoom", bg=BG, fg=DIM).pack(side="right", padx=4)

        arr = tk.Frame(self.root, bg=BG)
        arr.pack(side="top", fill="x", padx=8, pady=(0, 4))
        tk.Label(arr, text="song", bg=BG, fg=DIM, width=5, anchor="w").pack(side="left")
        self.turns_frame = tk.Frame(arr, bg=BG)
        self.turns_frame.pack(side="left")
        self.link_label = tk.Label(arr, text="", bg=BG, fg=DIM, anchor="w",
                                   font=("Consolas", 8))
        self.link_label.pack(side="left", padx=(12, 0))
        tk.Button(arr, text="Remove turn", command=self.on_remove_turn,
                  width=11).pack(side="right", padx=2)
        tk.Button(arr, text="Skip", command=self.on_skip_turn,
                  width=6).pack(side="right", padx=2)
        tk.Button(arr, text="Repeat", command=self.on_repeat_turn,
                  width=8).pack(side="right", padx=2)
        tk.Button(arr, text="Duplicate", command=self.on_duplicate_section,
                  width=10).pack(side="right", padx=2)
        tk.Button(arr, text="New turn", command=self.on_new_turn,
                  width=9).pack(side="right", padx=2)

        body = tk.Frame(self.root, bg=BG)
        body.pack(side="top", fill="both", expand=True, padx=8)

        side = tk.Frame(body, bg=BG, width=300)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)
        tk.Label(side, text="PARTS", bg=BG, fg=DIM, anchor="w").pack(fill="x")
        self.parts_frame = tk.Frame(side, bg=GRID)
        self.parts_frame.pack(fill="x", pady=(2, 8))
        self.part_rows = []
        self.gain_label = tk.Label(side, text="part volume", bg=BG, fg=DIM, anchor="w")
        self.gain_label.pack(fill="x")
        gain = tk.Scale(side, variable=self.gain_var, from_=0.0, to=1.0, resolution=0.01,
                        orient="horizontal", showvalue=False, command=self.on_gain,
                        bg=BG, fg=TEXT, troughcolor=GRID, highlightthickness=0, bd=0,
                        sliderrelief="flat")
        gain.pack(fill="x")
        gain.bind("<Button-1>", self.slider_press, add="+")
        gain.bind("<ButtonRelease-1>", self.slider_release, add="+")
        slots = tk.Frame(side, bg=BG)
        slots.pack(fill="x", pady=(4, 0))
        self.slots_label = tk.Label(slots, text="slots in a group", bg=BG, fg=DIM,
                                    anchor="w")
        self.slots_label.pack(side="left")
        self.slots_box = ttk.Combobox(slots, textvariable=self.slots_var, width=4,
                                      state="readonly",
                                      values=("1", "2", "3", "4", "6", "8", "12", "16"))
        self.slots_box.pack(side="left", padx=(6, 0))
        self.slots_box.bind("<<ComboboxSelected>>", self.on_slots)

        add = tk.Frame(side, bg=BG)
        add.pack(fill="x", pady=(8, 0))
        self.preset = tk.StringVar(value="kick")
        ttk.Combobox(add, textvariable=self.preset, state="readonly", width=8,
                     values=sorted(score.PRESETS)).pack(side="left")
        ttk.Combobox(add, textvariable=self.kind_var, state="readonly", width=7,
                     values=("free", "pattern")).pack(side="left", padx=(4, 0))
        tk.Button(add, text="Add", command=self.on_add_part,
                  width=5).pack(side="left", padx=(4, 0))
        tk.Button(add, text="Remove", command=self.on_remove_part,
                  width=8).pack(side="left", padx=(4, 0))
        self.local_btn = tk.Button(side, text="Make this part local to the section",
                                   command=self.on_make_local)
        self.local_btn.pack(fill="x", pady=(6, 0))
        self.free_btn = tk.Button(side, text="Stop this part repeating",
                                  command=self.on_make_free)
        self.free_btn.pack(fill="x", pady=(4, 0))
        tk.Button(side, text="Copy this part, to make a variant",
                  command=self.on_copy_part).pack(fill="x", pady=(4, 0))
        tk.Label(side, text="REPORT", bg=BG, fg=DIM, anchor="w").pack(fill="x", pady=(10, 2))
        self.info = tk.Text(side, bg=GRID, fg=TEXT, width=32, height=30, bd=0,
                            highlightthickness=0, font=("Consolas", 8), wrap="none")
        self.info.pack(fill="both", expand=True, pady=(0, 8))

        wrap = tk.Frame(body, bg=BG)
        wrap.pack(side="left", fill="both", expand=True, padx=(8, 0))
        # THE NOTE NAMES LIVE IN A CANVAS OF THEIR OWN, which never scrolls
        # sideways. In one canvas they slid away as soon as you moved along the
        # song, and then there was nothing to aim a note at.
        self.keys = tk.Canvas(wrap, bg=BG, highlightthickness=0, width=LEFT_W)
        self.ruler = tk.Canvas(wrap, bg=BG, highlightthickness=0, height=TOPBAR_H)
        # takefocus, so the grid can hold the keyboard. Without it the keys went
        # to whatever was clicked last, and space belonged to a button or a box.
        self.canvas = tk.Canvas(wrap, bg=BG, highlightthickness=0, takefocus=1)
        hbar = tk.Scrollbar(wrap, orient="horizontal", command=self.on_xview)
        vbar = tk.Scrollbar(wrap, orient="vertical", command=self.on_yview)
        self.canvas.configure(xscrollcommand=self.on_xscroll,
                              yscrollcommand=self.on_yscroll)
        self.vbar, self.hbar = vbar, hbar
        wrap.grid_rowconfigure(1, weight=1)
        wrap.grid_columnconfigure(1, weight=1)
        tk.Frame(wrap, bg=BG, width=LEFT_W, height=TOPBAR_H).grid(row=0, column=0)
        self.ruler.grid(row=0, column=1, sticky="ew")
        self.keys.grid(row=1, column=0, sticky="ns")
        self.canvas.grid(row=1, column=1, sticky="nsew")
        vbar.grid(row=1, column=2, sticky="ns")
        hbar.grid(row=2, column=1, sticky="ew")
        for w in (self.keys, self.ruler):
            w.bind("<MouseWheel>",
                   lambda e: self.canvas.yview_scroll(-e.delta // 120, "units"))
        self.ruler.bind("<Button-1>", self.on_set_head)
        self.canvas.bind("<Button-1>", self.on_click, add="+")
        self.canvas.bind("<Button-1>", lambda _e: self.canvas.focus_set(), add="+")
        self.canvas.bind("<B1-Motion>", self.on_move)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Button-3>", self.on_right_press)
        self.canvas.bind("<B3-Motion>", self.on_right_motion)
        self.canvas.bind("<ButtonRelease-3>", self.on_right_release)
        self.canvas.bind("<Motion>", self.on_hover)
        self.canvas.bind("<Button-2>", self.on_set_head)
        self.canvas.bind("<Control-MouseWheel>", lambda e: self.zoom(1.25 if e.delta > 0 else 1 / 1.25))
        self.canvas.bind("<MouseWheel>", lambda e: self.canvas.yview_scroll(-e.delta // 120, "units"))
        self.canvas.bind("<Shift-MouseWheel>", lambda e: self.canvas.xview_scroll(-e.delta // 120, "units"))

        # GIVE THE KEYBOARD BACK TO THE GRID after a click on anything that does
        # not need it. A combobox keeps focus once used, and then space belonged
        # to the box for ever: pressing it did nothing and there was no way to
        # tell why. A button keeps focus too, and Tk's own binding then makes
        # space press that button again.
        self.root.bind("<ButtonRelease-1>", self.refocus, add="+")
        # ONE binding for every combobox, present and future. A virtual event
        # travels up the bind tags, so the toplevel sees them all; and after_idle,
        # because ttk puts the focus back on the box during its own handling.
        self.root.bind("<<ComboboxSelected>>",
                       lambda _e: self.root.after_idle(self.canvas.focus_set),
                       add="+")
        self.root.bind("<space>", self.on_space)
        self.root.bind("<BackSpace>", self.on_delete_notes)
        self.root.bind("<Delete>", self.on_delete_notes)
        self.root.bind("<Control-c>", self.on_copy)
        self.root.bind("<Control-v>", self.on_paste)
        self.root.bind("<Control-z>", self.on_undo)
        self.root.bind("<Control-y>", self.on_redo)
        self.root.bind("<Control-Z>", self.on_redo)     # control and shift and z

        self.status = tk.Label(self.root, text="", bg=BG, fg=DIM, anchor="w")
        self.status.pack(side="bottom", fill="x", padx=10, pady=(0, 6))

    # ------------------------------------------------------------ the arrangement

    def cur_section(self):
        """The section the selected turn plays."""
        if not self.score.order:
            return None
        self.sel_turn = min(self.sel_turn, len(self.score.order) - 1)
        return score.section(self.score,
                             score.turn_name(self.score.order[self.sel_turn]))

    def panel_parts(self):
        """The parts the selected section plays, base parts included."""
        sec = self.cur_section()
        return score.sec_parts(self.score, sec) if sec else list(self.score.parts)

    def is_local(self, part):
        """True if the part belongs to the selected section, not to the base."""
        sec = self.cur_section()
        return bool(sec) and any(part is q for q in sec.parts)

    def turn_start(self, i):
        """The beat one turn of the arrangement starts on."""
        at = 0.0
        for k, entry in enumerate(self.score.order):
            if k == i:
                return at
            if not score.turn_on(entry):
                continue                  # a skipped turn takes no time
            sec = score.section(self.score, score.turn_name(entry))
            if sec:
                at += score.sec_beats(self.score, sec)
        return at

    def turn_at(self, beat):
        """Which turn a beat falls in."""
        at = 0.0
        last = 0
        for k, entry in enumerate(self.score.order):
            if not score.turn_on(entry):
                continue
            last = k
            sec = score.section(self.score, score.turn_name(entry))
            span = score.sec_beats(self.score, sec) if sec else 0
            if beat < at + span:
                return k
            at += span
        return last

    def part_here(self, name, turn):
        """The object of a part by name, in the section that turn plays."""
        sec = score.section(self.score, score.turn_name(self.score.order[turn]))
        if sec is None:
            return None
        for q in score.sec_parts(self.score, sec):
            if q.name == name:
                return q
        return None

    def section_colour(self, name):
        """The colour of a section. Every turn of it gets the same one."""
        for i, sec in enumerate(self.score.sections):
            if sec.name == name:
                return SECTION_COLOURS[i % len(SECTION_COLOURS)]
        return TURN_BG

    def linked_turns(self, name):
        """Every place in the order that plays this section."""
        return [i for i, n in enumerate(self.score.order)
                if score.turn_name(n) == name]

    def show_turns(self):
        """Draw the order, one box for each turn.

        A box carries the colour of its section, so two turns of one section look
        the same. The turns of the SELECTED section are drawn bright and the rest
        are dimmed, which shows exactly which turns an edit will reach.

        The boxes are REUSED while their number is the same. A drag along the
        strip redraws on every step, and destroying the box under the pointer
        would take the drag with it.
        """
        if len(self.turn_rows) != len(self.score.order):
            for w in self.turn_rows:
                w.destroy()
            self.turn_rows = []
            for i in range(len(self.score.order)):
                b = tk.Label(self.turns_frame, font=("Consolas", 8), padx=3, bd=2)
                b.pack(side="left", padx=1)
                b.bind("<Button-1>", lambda e, k=i: self.turn_press(k))
                b.bind("<B1-Motion>", self.turn_motion)
                b.bind("<ButtonRelease-1>", self.turn_release)
                self.turn_rows.append(b)

        cur = score.turn_name(self.score.order[self.sel_turn])             if self.score.order else None
        for i, entry in enumerate(self.score.order):
            name = score.turn_name(entry)
            on = score.turn_on(entry)
            col = self.section_colour(name)
            linked = (name == cur)
            self.turn_rows[i].configure(
                text=(" %d %s " % (i + 1, name)) if on else (" %d (%s) " % (i + 1, name)),
                bg=(col if linked else blend(col, BG, 0.62)) if on
                   else blend(col, BG, 0.85),
                fg=(TEXT if linked else DIM) if on else DIM,
                font=("Consolas", 8, "bold" if i == self.sel_turn else "normal"),
                relief="solid" if i == self.sel_turn else "flat",
                highlightbackground=SEL_EDGE)
        n = len(self.linked_turns(cur)) if cur else 0
        self.link_label.configure(
            text=("%s plays %s. One edit changes them all."
                  % (cur, score.plural(n, "turn"))) if n > 1
            else ("%s plays once. It is on its own." % cur if cur else ""),
            fg=WARN if n > 1 else DIM)

    def turn_at_x(self, x):
        """Which box is at this place along the strip."""
        for i, w in enumerate(self.turn_rows):
            if w.winfo_x() <= x < w.winfo_x() + w.winfo_width():
                return i
        if not self.turn_rows:
            return None
        if x < self.turn_rows[0].winfo_x():
            return 0
        return len(self.turn_rows) - 1

    def turn_press(self, i):
        self.pick_turn(i)
        self.turn_drag = {"moved": False}
        self.hold()

    def turn_motion(self, ev):
        """Drag a turn along the strip to put it somewhere else in the order."""
        if self.turn_drag is None:
            return
        x = self.turns_frame.winfo_pointerx() - self.turns_frame.winfo_rootx()
        j = self.turn_at_x(x)
        if j is None or j == self.sel_turn:
            return
        self.commit()
        order = self.score.order
        order.insert(j, order.pop(self.sel_turn))
        self.sel_turn = j
        self.turn_drag["moved"] = True
        self.show_turns()
        self.rebuild()
        self.say("moved it to place %d. The order is now %s."
                 % (j + 1, " ".join(order)))

    def turn_release(self, _ev=None):
        moved = bool(self.turn_drag and self.turn_drag["moved"])
        self.turn_drag = None
        self.pending = None
        if moved:
            self.rebuild()

    def pick_turn(self, i):
        if not (0 <= i < len(self.score.order)):
            return
        self.sel_turn = i
        self.sel_note = None
        self.show_turns()
        self.show_parts()
        self.show_tempo()
        self.draw()
        sec = self.cur_section()
        if sec is None:
            return
        others = [str(k + 1) for k in self.linked_turns(sec.name) if k != i]
        if others:
            names = others[0] if len(others) == 1 else                 ", ".join(others[:-1]) + " and " + others[-1]
            self.say("turn %d plays %s, and so %s %s %s. An edit changes them all."
                     % (i + 1, sec.name,
                        "does" if len(others) == 1 else "do",
                        "turn" if len(others) == 1 else "turns", names))
        else:
            self.say("turn %d plays %s, which plays nowhere else." % (i + 1, sec.name))

    def on_new_turn(self):
        """Add a turn playing a NEW, empty section, after the one selected.

        The three buttons are three different things. `Repeat` plays the same
        section again, so an edit changes both turns. `Duplicate` copies this
        section with its notes, to vary something that already works. This makes
        a section with nothing in it.

        It gets an EMPTY free part for each free part in the base, for the same
        reason Duplicate gets a full copy of them: a melody is what a section is
        for, and a section with none of its own would play the base melody and
        the button would look as though it had done nothing.
        """
        self.mark()
        base, k = "new", 2
        name = base
        while score.section(self.score, name):
            name = "%s%d" % (base, k)
            k += 1
        sec = score.Section(name)
        blank = []
        for p in self.score.parts:
            if p.kind == "free":
                q = copy.deepcopy(p)
                q.events = []
                sec.parts.append(q)
                blank.append(q.name)
        self.score.sections.append(sec)
        self.score.order.insert(self.sel_turn + 1, name)
        self.sel_turn += 1
        self.show_turns()
        self.show_parts()
        self.rebuild()
        if not blank:
            note = "there is no free part to write into yet."
        else:
            names = (blank[0] if len(blank) == 1
                     else ", ".join(blank[:-1]) + " and " + blank[-1])
            note = "%s %s blank and yours to write." % (
                names, "is" if len(blank) == 1 else "are")
        self.say("added %s, an empty turn. The base parts play; %s" % (name, note))

    def on_duplicate_section(self):
        """Copy the section of this turn under a new name, and play it next.

        THE COPY GETS ITS OWN FREE PARTS. A melody is the reason to duplicate a
        section, and a base melody is one object shared by every section, so
        without this the copy would play the same notes and the button would
        appear to do nothing. A pattern part stays shared, because the beats and
        the lead are meant to run through the whole song.
        """
        sec = self.cur_section()
        if sec is None:
            return
        self.mark()
        new = copy.deepcopy(sec)
        base, k = sec.name, 2
        while score.section(self.score, "%s%d" % (base, k)):
            k += 1
        new.name = "%s%d" % (base, k)

        have = {p.name for p in new.parts}
        took = []
        for p in score.sec_parts(self.score, sec):
            if p.kind == "free" and p.name not in have:
                new.parts.append(copy.deepcopy(p))
                took.append(p.name)

        self.score.sections.append(new)
        self.score.order.insert(self.sel_turn + 1, new.name)
        self.sel_turn += 1
        self.show_turns()
        self.show_parts()
        self.rebuild()
        if took:
            self.say("made %s and put it next. %s %s local to it, so edit away."
                     % (new.name, " and ".join(took),
                        "is" if len(took) == 1 else "are"))
        else:
            self.say("made %s and put it next. It has no melody of its own yet, "
                     "so add one or the beats alone will play." % new.name)

    def on_repeat_turn(self):
        """Play the same section once more, right after this turn."""
        sec = self.cur_section()
        if sec is None:
            return
        self.mark()
        self.score.order.insert(self.sel_turn + 1, sec.name)
        self.sel_turn += 1
        self.show_turns()
        self.rebuild()
        self.say("%s plays again. It is the same section, so an edit changes both."
                 % sec.name)

    def on_skip_turn(self):
        """Keep this turn in the order but do not play it.

        Use it to hold a section back for later without taking it out of the
        song. A skipped turn makes no sound and takes no time.
        """
        if not self.score.order:
            return
        self.mark()
        entry = self.score.order[self.sel_turn]
        on = score.turn_on(entry)
        self.score.order[self.sel_turn] = ("-" + entry) if on else entry[1:]
        self.show_turns()
        self.rebuild()
        self.say("turn %d (%s) is %s" % (self.sel_turn + 1, score.turn_name(entry),
                                         "skipped, and kept for later" if on
                                         else "played again"))

    def on_remove_turn(self):
        if len(self.score.order) < 2:
            self.say("a song needs one turn. Add another before you remove this.")
            return
        self.mark()
        name = self.score.order.pop(self.sel_turn)
        self.sel_turn = max(0, self.sel_turn - 1)
        self.show_turns()
        self.show_parts()
        self.rebuild()
        self.say("removed one turn of %s. The section itself is kept." % name)

    def on_make_free(self):
        """Write a pattern part out as separate notes, so it stops repeating."""
        parts = self.panel_parts()
        if not parts:
            return
        part = parts[min(self.sel_part, len(parts) - 1)]
        if part.kind == "free":
            self.say("%s is already free. Every note of it stands on its own."
                     % part.name)
            return
        self.mark()
        n = score.to_free(self.score, self.cur_section(), part)
        self.show_parts()
        self.rebuild()
        self.say("%s is free now: %s written out, and nothing repeats. Move them "
                 "one at a time." % (part.name, score.plural(n, "note")))

    def on_make_local(self):
        """Give this section its own copy of the selected part."""
        sec = self.cur_section()
        parts = self.panel_parts()
        if sec is None or not parts:
            return
        part = parts[min(self.sel_part, len(parts) - 1)]
        if self.is_local(part):
            self.say("%s already belongs to %s." % (part.name, sec.name))
            return
        self.mark()
        sec.parts.append(copy.deepcopy(part))
        self.show_parts()
        self.rebuild()
        self.say("%s is now local to %s. Editing it here changes no other section."
                 % (part.name, sec.name))

    # ---------------------------------------------------------------- the history

    def mark(self):
        """Keep the score as it is now, so that undo can put it back. Call this
        BEFORE a change, and once for each change the user makes."""
        self.undo_stack.append(copy.deepcopy(self.score))
        del self.undo_stack[:-UNDO_DEPTH]
        self.redo_stack = []
        self.pending = None
        self.show_history()

    def hold(self):
        """Keep a copy for a drag, but do not put it on the stack yet.

        A drag sends many move events, and each one changes the score. Without
        this, one drag of the mouse would fill the undo stack. commit() puts the
        copy on the stack, and only for the first change of the drag.
        """
        self.pending = copy.deepcopy(self.score)

    def commit(self):
        """Put the copy from hold() on the undo stack. Later calls do nothing."""
        if self.pending is None:
            return
        self.undo_stack.append(self.pending)
        del self.undo_stack[:-UNDO_DEPTH]
        self.redo_stack = []
        self.pending = None
        self.show_history()

    def slider_press(self, _):
        """A slider drag starts. One drag makes one undo step, as a note drag does."""
        self.hold()
        self.slider_moved = False

    def slider_release(self, _):
        if self.slider_moved:
            self.commit()
        self.pending = None
        self.slider_moved = False

    def on_tempo(self, _value):
        if self.syncing or self.score is None:
            return
        bpm = float(self.tempo_var.get())
        if bpm == self.score.tempo:
            return
        self.slider_moved = True
        self.score.tempo = bpm
        self.tempo_label.configure(text="%g BPM" % bpm)
        self.rebuild()
        self.say("tempo %g BPM. The notes keep their beats." % bpm)

    def on_gain(self, _value):
        if self.syncing or self.score is None or not self.score.parts:
            return
        part = self.panel_parts()[self.sel_part]
        g = round(float(self.gain_var.get()), 4)
        if g == part.gain:
            return
        self.slider_moved = True
        part.gain = g
        self.show_parts()
        self.say("%s volume %.2f" % (part.name, g))

    def show_parts(self):
        """Build the part list. Each part gets a box that turns it on and off."""
        self.syncing = True
        for row, _, _ in self.part_rows:
            row.destroy()
        self.part_rows = []
        parts = self.panel_parts()
        if parts:
            self.sel_part = min(self.sel_part, len(parts) - 1)
        for i, part in enumerate(parts):
            row = tk.Frame(self.parts_frame, bg=GRID)
            row.pack(fill="x")
            var = tk.BooleanVar(value=not part.mute)
            # selectcolor fills the box when the part is ON, and fg draws the
            # tick. Both must contrast or every part looks switched off.
            tk.Checkbutton(row, variable=var, bg=GRID, activebackground=GRID,
                           selectcolor=OK, fg=BG, activeforeground=BG,
                           highlightthickness=0, bd=0,
                           command=lambda k=i: self.on_mute(k)).pack(side="left")
            label = tk.Label(row, bg=GRID, anchor="w", font=("Consolas", 8),
                             text="%-8s %-5s %-6s %.2f"
                                  % (part.name[:8],
                                     "local" if self.is_local(part) else "base",
                                     part.wave, part.gain))
            label.pack(side="left", fill="x", expand=True)
            for w in (row, label):
                w.bind("<Button-1>", lambda _e, k=i: self.pick_part(k))
                w.bind("<Button-3>", lambda _e, k=i: self.remove_at(k))
            self.part_rows.append((row, var, label))
        self.show_selected()
        self.syncing = False

    def show_selected(self):
        """Mark the part you picked, and point the volume slider at it."""
        parts = self.panel_parts()
        for i, (row, _, label) in enumerate(self.part_rows):
            bg = ROW_SEL if i == self.sel_part else GRID
            row.configure(bg=bg)
            label.configure(bg=bg, fg=DIM if parts[i].mute else TEXT)
        if parts:
            part = parts[self.sel_part]
            self.gain_var.set(part.gain)
            self.gain_label.configure(text="volume of %s" % part.name)
            self.local_btn.configure(
                text=("%s is local to this section" % part.name)
                if self.is_local(part) else "Make this part local to the section",
                state="disabled" if self.is_local(part) else "normal")
            self.free_btn.configure(
                text=("%s already stands on its own" % part.name)
                if part.kind == "free" else "Stop this part repeating",
                state="disabled" if part.kind == "free" else "normal")
            if part.kind == "pattern":
                self.slots_var.set(str(len(part.pattern)))
                self.slots_box.configure(state="readonly")
                self.slots_label.configure(text="slots in a group", fg=DIM)
            else:
                # A free part has no slots. Each of its notes has its own length.
                self.slots_var.set("")
                self.slots_box.configure(state="disabled")
                self.slots_label.configure(text="a free part has no slots", fg=DIM)
        else:
            self.gain_label.configure(text="part volume")

    def on_remove_part(self):
        """Remove the part you picked. Undo puts it back."""
        parts = self.panel_parts()
        if len(score.all_parts(self.score)) < 2:
            self.say("a score needs one part. Add another before you remove this.")
            return
        part = parts[min(self.sel_part, len(parts) - 1)]
        name = part.name
        self.mark()
        sec = self.cur_section()
        if sec is not None and any(part is q for q in sec.parts):
            sec.parts = [q for q in sec.parts if q is not part]
        else:
            self.score.parts = [q for q in self.score.parts if q is not part]
        self.sel_part = max(0, self.sel_part - 1)
        self.show_parts()
        # The engine keys a part by its place in the list, and the places moved,
        # so it needs the whole score again. rebuild sends it.
        self.rebuild()
        self.say("removed %s. Undo puts it back." % name)

    def on_slots(self, _=None):
        """Change how many slots a group has, which is the timing grid."""
        if self.syncing or not self.score.parts:
            return
        part = self.panel_parts()[self.sel_part]
        if part.kind != "pattern" or not self.slots_var.get():
            return
        new, old = int(self.slots_var.get()), len(part.pattern)
        if new == old:
            return
        self.mark()
        pattern = ["."] * new
        for i, tok in enumerate(part.pattern):
            if tok == ".":
                continue
            j = int(round(i * new / float(old)))
            if 0 <= j < new:
                pattern[j] = tok
        part.pattern = pattern
        self.rebuild()
        beat = 60.0 / self.score.tempo
        self.say("%s has %s in a group, one every %.0f ms"
                 % (part.name, score.plural(new, "slot"),
                    self.score.cell * beat / new * 1000))

    def pick_part(self, i):
        self.sel_part = i
        self.sel_note = None
        self.syncing = True
        self.show_selected()
        self.syncing = False
        self.draw()

    def remove_at(self, i):
        """Right click on a part row removes it."""
        self.pick_part(i)
        self.on_remove_part()

    def on_mute(self, i):
        """Turn one part on or off. This is a listening control. It changes no
        note, it makes no undo step, and it is not written to the file."""
        if self.syncing:
            return
        part = self.panel_parts()[i]
        part.mute = not self.part_rows[i][1].get()
        self.show_selected()
        self.rebuild(push=False)        # no note changed, so send no notes
        if self.live.alive:
            # The engine keys a part by its place in score.all_parts, which is not
            # the place it has in this panel.
            for k, q in enumerate(score.all_parts(self.score)):
                if q is part:
                    self.live.set_mute(k, part.mute)
                    break
        self.say("%s is %s" % (part.name, "off" if part.mute else "on"))

    def show_tempo(self):
        self.syncing = True
        self.tempo_var.set(self.score.tempo)
        self.tempo_label.configure(text="%g BPM" % self.score.tempo)
        sec = self.cur_section()
        self.groups_var.set(str(len(score.sec_roots(self.score, sec)) if sec
                                else len(self.score.roots)))
        self.reps_var.set(str(score.sec_reps(self.score, sec) if sec
                              else self.score.reps))
        self.syncing = False

    def on_undo(self, *_):
        if not self.undo_stack:
            self.say("there is nothing to undo")
            return
        self.redo_stack.append(copy.deepcopy(self.score))
        self.set_score(self.undo_stack.pop())
        self.say("undo. %s to go back." % score.plural(len(self.undo_stack), "step"))

    def on_redo(self, *_):
        if not self.redo_stack:
            self.say("there is nothing to redo")
            return
        self.undo_stack.append(copy.deepcopy(self.score))
        self.set_score(self.redo_stack.pop())
        self.say("redo. %s to go forward." % score.plural(len(self.redo_stack), "step"))

    def show_history(self):
        self.undo_btn.configure(state="normal" if self.undo_stack else "disabled")
        self.redo_btn.configure(state="normal" if self.redo_stack else "disabled")

    def set_score(self, s):
        """Put a score in the window. Undo and redo use this, and so does load."""
        # Mute is a listening control, not part of the score. Carry it across an
        # undo by name, or an undo would switch parts back on behind you.
        if self.score is not None:
            was = {q.name: q.mute for q in self.score.parts}
            for q in s.parts:
                q.mute = was.get(q.name, q.mute)
        self.score = s
        self.sel_part = min(self.sel_part, max(0, len(s.parts) - 1))
        self.sel_note = None
        self.drag = None
        self.pending = None
        self.sel_notes = set()           # the ids of the old score are gone
        self.low = self.high = None      # a new score gets a fresh range
        self.sel_turn = min(self.sel_turn, max(0, len(s.order) - 1))
        self.show_turns()
        self.show_parts()
        self.show_tempo()
        self.rebuild()
        self.show_history()

    def on_xview(self, *args):
        """Scroll the grid and the band of times together."""
        self.canvas.xview(*args)
        self.ruler.xview(*args)

    def on_xscroll(self, first, last):
        self.hbar.set(first, last)
        self.ruler.xview_moveto(first)

    def on_yview(self, *args):
        """Scroll the grid and the note names together."""
        self.canvas.yview(*args)
        self.keys.yview(*args)

    def on_yscroll(self, first, last):
        self.vbar.set(first, last)
        self.keys.yview_moveto(first)

    # ------------------------------------------------------------------- the file

    def load(self, path):
        try:
            s = score.read_score(path)
        except SystemExit as e:
            messagebox.showerror("Cannot read the score", str(e))
            return
        self.path = path
        self.undo_stack, self.redo_stack = [], []
        self.sel_part = 0
        self.set_score(s)
        self.root.update_idletasks()
        self.center_on_notes()
        self.say("opened %s" % rel(path))

    def on_new(self):
        """Start a blank song. Anything not saved is gone, so ask first."""
        if not messagebox.askokcancel(
                "Start a new song",
                "This clears the window. Anything not saved is lost.\n\n"
                "Carry on?"):
            return
        self.path = None
        self.undo_stack, self.redo_stack = [], []
        self.sel_part = self.sel_turn = 0
        self.set_score(score.new_score())
        self.root.update_idletasks()
        self.center_on_notes()
        self.say("a new song. Save it into design/score/ when it is worth keeping.")

    def on_copy_part(self):
        """Copy the picked part, notes and all, so a variant can be made from it.

        The copy lands beside the original, in the same place: a base part stays
        base, a part of this section stays in this section.
        """
        parts = self.panel_parts()
        if not parts:
            return
        part = parts[min(self.sel_part, len(parts) - 1)]
        self.mark()
        made = copy.deepcopy(part)
        taken = {q.name for q in score.all_parts(self.score)}
        base, k = part.name, 2
        while made.name in taken:
            made.name = "%s%d" % (base, k)
            k += 1
        sec = self.cur_section()
        if sec is not None and any(part is q for q in sec.parts):
            sec.parts.append(made)
            where = "in %s" % sec.name
        else:
            self.score.parts.append(made)
            where = "in every section"
        self.show_parts()
        self.sel_part = [q.name for q in self.panel_parts()].index(made.name)
        self.show_parts()
        self.rebuild()
        self.say("copied %s to %s, %s. Change the copy and keep the original."
                 % (part.name, made.name, where))

    def on_groups(self, _=None):
        """Change how many groups a turn has, which is how many roots it plays."""
        if self.syncing or not self.score.order:
            return
        sec = self.cur_section()
        want = int(self.groups_var.get())
        roots = score.sec_roots(self.score, sec)
        if want == len(roots):
            return
        self.mark()
        out = list(roots)
        while len(out) < want:
            out.append(out[-1] - 1)     # one semitone below the last, as a start
        del out[want:]
        # A section that sets its own roots keeps them to itself. One that does
        # not is reading the score's, so the score's are what change.
        if sec is not None and sec.roots:
            sec.roots = out
            where = sec.name
        else:
            self.score.roots = out
            where = "every section that has none of its own"
        self.show_turns()
        self.rebuild()
        self.say("%s now has %s. Drag a green note to move one."
                 % (where, score.plural(want, "group")))

    def on_reps(self, _=None):
        """Change how many times each group repeats."""
        if self.syncing or not self.score.order:
            return
        sec = self.cur_section()
        want = int(self.reps_var.get())
        if want == score.sec_reps(self.score, sec):
            return
        self.mark()
        if sec is not None and sec.reps:
            sec.reps = want
            where = sec.name
        else:
            self.score.reps = want
            where = "every section that has none of its own"
        self.show_turns()
        self.rebuild()
        self.say("%s now plays each group %s." % (where, score.plural(want, "time")))

    def on_open(self):
        p = filedialog.askopenfilename(
            initialdir=os.path.join(score.ROOT, "design", "score"),
            filetypes=[("Score files", "*.score"), ("All files", "*.*")])
        if p:
            self.load(p)

    def on_save(self):
        if not self.path:
            return self.on_save_as()
        score.write_score(self.score, self.path)
        self.say("saved %s" % rel(self.path))

    def on_save_as(self):
        p = filedialog.asksaveasfilename(
            defaultextension=".score", initialdir=os.path.join(score.ROOT, "design", "score"),
            filetypes=[("Score files", "*.score")])
        if p:
            self.path = p
            self.on_save()

    def on_bake(self):
        if not self.path:
            messagebox.showinfo("Save first", "Save the score before you bake it.")
            return
        self.on_save()
        try:
            score.bake(self.score, self.notes, self.path)
        except SystemExit as e:
            messagebox.showerror("Cannot bake", str(e))
            return
        self.say("baked src/vg/generated/score_%s.h" % self.score.name.lower())

    # ------------------------------------------------------------------ the sound

    def start_engine(self):
        """Open the sound engine when the window appears, so Play is instant."""
        if self.ensure_live(quiet=True):
            self.say("sound ready. Press play, or click a note to hear it.")

    def ensure_live(self, quiet=False):
        """Start the engine if it is not running. Return True if it is running."""
        if self.live.alive:
            return True
        try:
            self.live.start()
        except score_audio.BuildError as e:
            if quiet:
                # A box in the face at start up is wrong. Say it and carry on.
                # Pressing Play tries again, and then it may say more.
                self.say("no sound engine: %s" % str(e).splitlines()[0])
            else:
                messagebox.showerror("Cannot start the sound engine", str(e))
            return False
        self.live.set_speaker(self.last_device)
        self.live.set_loop(self.loop.get())
        self.live.load(self.score, self.notes)
        return True

    def on_play(self, as_device):
        """Play from where the playhead is. Pause leaves it, so this resumes."""
        if not self.ensure_live():
            return
        self.last_device = as_device
        # PLAY FROM THE END DID NOTHING. The engine reaches the end, stops, and
        # leaves the playhead there; playing from that point stops again at once.
        # Play from the top instead, which is what the button is for.
        end = score.song_beats(self.score) * 60.0 / self.score.tempo * 1000.0
        if self.head_ms >= end - 1:
            self.head_ms = 0.0
        self.live.set_speaker(as_device)
        self.live.set_loop(self.loop.get())
        self.live.load(self.score, self.notes)
        self.live.seek(self.head_ms)
        self.live.play()
        self.playing = True
        self.say(("device preview, approximate. " if as_device else "")
                 + "playing from %s. Edit it while it plays." % self.head_text())
        self.tick()

    def refocus(self, ev=None):
        """Put the keyboard back on the grid, unless the click wants to type."""
        w = getattr(ev, "widget", None)
        cls = ""
        try:
            cls = w.winfo_class() if w is not None else ""
        except Exception:
            cls = ""
        if cls in ("Entry", "TEntry", "Text", "Listbox"):
            return
        self.canvas.focus_set()

    def typing(self):
        """True while a box or a list has the keyboard. A key press then belongs
        to that widget and not to the grid."""
        w = self.root.focus_get()
        return w is not None and w.winfo_class() in ("Entry", "TEntry", "Text",
                                                     "TCombobox", "Listbox")

    def picked_notes(self):
        """The picked notes as (part, index, beat, pitch, length), in time order.

        A beat here is the one stored in the part, which is counted from the start
        of its own SECTION. Copy and paste both work in that frame.
        """
        out = []
        for pid, idx in self.sel_notes:
            part = self.part_by_id(pid)
            if part is None or idx >= len(part.events):
                continue
            at, pitch, length = part.events[idx]
            out.append((part, idx, at, pitch, length))
        out.sort(key=lambda r: (r[2], r[3]))
        return out

    def on_delete_notes(self, _=None):
        """Backspace removes every picked note."""
        if self.typing():
            return None
        rows = self.picked_notes()
        if not rows:
            self.say("nothing is picked. Drag with the right button to pick notes.")
            return "break"
        self.mark()
        by = {}
        for part, idx, _a, _p, _l in rows:
            by.setdefault(id(part), (part, []))[1].append(idx)
        gone = 0
        for part, idxs in by.values():
            # Highest index first, or removing one would move the next.
            for i in sorted(idxs, reverse=True):
                if i < len(part.events):
                    part.events.pop(i)
                    gone += 1
        self.sel_notes = set()
        self.rebuild()
        self.say("removed %s" % score.plural(gone, "note"))
        return "break"

    def on_copy(self, _=None):
        """Keep the picked notes, measured from the earliest of them."""
        if self.typing():
            return None
        rows = self.picked_notes()
        if not rows:
            self.say("nothing is picked to copy.")
            return "break"
        first = rows[0][2]
        self.clip = [(part.name, at - first, pitch, length)
                     for part, _i, at, pitch, length in rows]
        self.say("copied %s. Put the playhead where you want them and paste."
                 % score.plural(len(self.clip), "note"))
        return "break"

    def on_paste(self, _=None):
        """Paste at the playhead, into the section the playhead is in."""
        if self.typing():
            return None
        if not self.clip:
            self.say("nothing has been copied.")
            return "break"
        beat = self.head_ms / 1000.0 * self.score.tempo / 60.0
        turn = self.turn_at(beat)
        sec = score.section(self.score, score.turn_name(self.score.order[turn]))
        if sec is None:
            return "break"
        # SNAP THE PASTE. The playhead can sit anywhere, because you may want to
        # hear from anywhere, but a note pasted at an odd fraction of a beat is
        # never wanted and it makes a file that no longer lines up with itself.
        snap = self.snap()
        start = max(0.0, round((beat - self.turn_start(turn)) / snap) * snap)
        parts = {p.name: p for p in score.sec_parts(self.score, sec)
                 if p.kind == "free"}
        self.mark()
        added, missing = [], set()
        for name, dbeat, pitch, length in self.clip:
            part = parts.get(name)
            if part is None:
                missing.add(name)
                continue
            part.events.append((start + dbeat, pitch, length))
            added.append((id(part), len(part.events) - 1))
        if turn != self.sel_turn:
            self.pick_turn(turn)
        self.sel_notes = set(added)      # picked, so they can be moved at once
        self.rebuild()
        if not added:
            self.say("%s has no free part named %s to paste into."
                     % (sec.name, " or ".join(sorted(missing))))
        else:
            self.say("pasted %s into %s at bar %.1f%s"
                     % (score.plural(len(added), "note"), sec.name, start / 4.0 + 1,
                        ".  %s was not there to paste into."
                        % " and ".join(sorted(missing)) if missing else ""))
        return "break"

    def on_space(self, _=None):
        """Space plays, and plays again to pause. It does nothing while a box or a
        list has the keyboard, or it would type into them."""
        if self.typing():
            self.say("a box has the keyboard. Click the grid, then space.")
            return None
        if self.playing:
            self.on_pause()
        else:
            self.on_play(self.last_device)
        return "break"

    def on_pause(self):
        """Silence the sound and leave the playhead where it is."""
        if self.live.alive:
            self.head_ms = max(0, self.live.pos_ms)
            self.live.stop()
        self.playing = False
        self.draw_head()
        self.say("paused at %s. Play goes on from there." % self.head_text())

    def on_stop(self):
        """Silence the sound and go back to the start of the song."""
        if self.live.alive:
            self.live.stop()
            self.live.seek(0)
        self.playing = False
        self.head_ms = 0.0
        self.draw_head()
        self.say("stopped, and back to the start.")

    def on_loop(self):
        """The loop goes on or off with no stop."""
        if self.live.alive:
            self.live.set_loop(self.loop.get())
        self.say("loop is %s" % ("on" if self.loop.get() else "off"))

    def hear(self, part, midi):
        """Sound one note now, so that a click is audible at once."""
        if self.audition.get() and self.ensure_live():
            self.live.hit(part, midi)

    def head_text(self):
        """Where the playhead is, as a turn and a bar."""
        beat = self.head_ms / 1000.0 * self.score.tempo / 60.0
        turn = self.turn_at(beat)
        bar = (beat - self.turn_start(turn)) / 4.0 + 1
        name = score.turn_name(self.score.order[turn]) if self.score.order else "?"
        return "%s  turn %d (%s), bar %.1f" % (self.clock(self.head_ms / 1000.0),
                                              turn + 1, name, bar)

    def draw_head(self):
        """Draw the playhead. It is drawn when the sound is stopped too, so you
        can see where Play will start."""
        self.canvas.delete("playhead")
        self.ruler.delete("playhead")
        beat = self.head_ms / 1000.0 * self.score.tempo / 60.0
        x = self.x_of(beat)
        self.canvas.create_line(x, TOP_H, x, self.grid_bottom(),
                                fill=PLAYHEAD, width=2, tags="playhead")
        self.ruler.create_line(x, 0, x, TOPBAR_H, fill=PLAYHEAD, width=2,
                               tags="playhead")

    def tick(self):
        """Follow the engine while it plays."""
        if not self.playing:
            return
        if not self.live.alive:
            self.playing = False
            return
        if self.live.pos_ms >= 0:
            self.head_ms = self.live.pos_ms
        self.draw_head()
        if not self.live.going:
            # The engine reached the end with the loop off.
            self.playing = False
            self.say("reached the end. Play starts again from the top.")
            return
        self.root.after(40, self.tick)

    # ------------------------------------------------------------------ the grid

    def total_beats(self):
        """The beats the whole arrangement takes, plus any note that overhangs."""
        s = self.score
        beats = score.song_beats(s)
        at = 0.0
        for name in s.order:
            sec = score.section(s, name)
            if sec is None:
                continue
            for p in score.sec_parts(s, sec):
                for b, _, length in p.events:
                    beats = max(beats, at + b + length)
            at += score.sec_beats(s, sec)
        return beats

    def rebuild(self, push=True):
        """Work out the notes again, then draw.

        `push` false leaves the engine alone. Use it for a change that alters no
        note, such as turning a part off, so that the engine is not sent a whole
        score it already holds.
        """
        self.notes = score.build(self.score)
        # A NOISE PART HAS NO PITCH, so it is not drawn among the pitched rows. It
        # gets a lane of its own under them. Its notes all sat on C4 before, which
        # read as a part locked to one note when it is a part with no note at all.
        self.lanes = [p for p in self.score.parts if p.wave == "noise"]
        for sec in self.score.sections:
            for p in sec.parts:
                if p.wave == "noise" and not any(p is q for q in self.lanes):
                    self.lanes.append(p)
        self.low, self.high = LOW_NOTE, HIGH_NOTE
        self.rows = self.high - self.low + 1
        self.draw()
        self.report()
        # Send the new notes to the engine, which keeps its position, so an edit
        # is heard on the next turn of the loop. Not during a drag: that would be
        # a reload for every frame of the mouse, and the preview note covers it.
        if push and self.live.alive and not self.drag:
            self.live.load(self.score, self.notes)

    def center_on_notes(self):
        """Scroll so the notes are in view. The grid covers C1 to C7, and most
        music uses a small part of it."""
        pitched = [n["n"] for n in self.notes if n["part"].wave != "noise"]
        if not pitched:
            pitched = [60]
        mid = (min(pitched) + max(pitched)) / 2.0
        y = TOP_H + (self.high - mid) * ROW_H
        h = max(1, self.grid_bottom() + 10)
        view = self.canvas.winfo_height() or 400
        self.canvas.yview_moveto(max(0.0, (y - view / 2.0) / h))

    def x_of(self, beat):
        return beat * self.px_per_beat

    def y_of(self, pitch):
        return TOP_H + (self.high - pitch) * ROW_H

    def lane_y(self, part):
        """The top of the lane of a noise part, or None if it has none."""
        if part not in self.lanes:
            return None
        return TOP_H + (self.rows + self.lanes.index(part)) * ROW_H

    def note_y(self, n):
        """Where a note is drawn. A noise note goes in the lane of its part."""
        y = self.lane_y(n["part"])
        return self.y_of(n["n"]) if y is None else y

    def grid_bottom(self):
        return TOP_H + (self.rows + len(self.lanes)) * ROW_H

    def beat_of(self, x):
        return self.canvas.canvasx(x) / self.px_per_beat

    def pitch_of(self, y):
        return self.high - int((self.canvas.canvasy(y) - TOP_H) // ROW_H)

    @staticmethod
    def clock(seconds):
        """A time as minutes and seconds, such as 1:37."""
        seconds = max(0.0, seconds)
        return "%d:%02d" % (int(seconds) // 60, int(seconds) % 60)

    def draw_ruler(self, w, end_beats):
        """The frozen bar: times along the top, turn names under them.

        It is a canvas of its own, so it never scrolls out of view when the grid
        scrolls down.
        """
        c = self.ruler
        c.delete("all")
        c.configure(scrollregion=(0, 0, w, TOPBAR_H))
        c.create_rectangle(0, 0, w, RULER_H, fill=GRID, outline="")
        c.create_line(0, RULER_H, w, RULER_H, fill=GRID_BAR)
        for x0, x1, ti, name, col, linked in getattr(self, "heads", []):
            c.create_rectangle(x0, RULER_H, x1, TOPBAR_H,
                               fill=col if linked else blend(col, BG, 0.62),
                               outline="")
            if ti == self.sel_turn:
                c.create_rectangle(x0 + 1, RULER_H + 1, x1 - 1, TOPBAR_H - 1,
                                   outline=SEL_EDGE, width=2)
            c.create_line(x0, 0, x0, TOPBAR_H, fill=SEC_LINE, width=3)
            repeats = len(self.linked_turns(name))
            c.create_text(x0 + 6, RULER_H + 10, anchor="w",
                          fill=TEXT if linked else DIM,
                          text="%d  %s%s" % (ti + 1, name,
                                             "  linked" if repeats > 1 else ""),
                          font=("Consolas", 8, "bold"))
        per_sec = self.px_per_beat * self.score.tempo / 60.0
        # A mark every so many seconds, chosen so that the labels never crowd.
        step = next((v for v in (1, 2, 5, 10, 15, 30, 60, 120)
                     if v * per_sec >= 70), 120)
        total = end_beats * 60.0 / self.score.tempo
        t = 0.0
        while t <= total + 1e-6:
            x = t * per_sec
            c.create_line(x, RULER_H - 5, x, RULER_H, fill=DIM)
            c.create_text(x + 3, 7, anchor="w", text=self.clock(t), fill=DIM,
                          font=("Consolas", 7))
            t += step
        c.create_text(self.x_of(end_beats) + 6, 7, anchor="w",
                      text="end  %s" % self.clock(total), fill=TEXT,
                      font=("Consolas", 7, "bold"))
        c.create_line(self.x_of(end_beats), 0, self.x_of(end_beats), TOPBAR_H,
                      fill=SEC_LINE, width=3)
        c.xview_moveto(self.canvas.xview()[0])

    def draw_keys(self, h):
        """Draw the note names. They are in a canvas that does not scroll sideways,
        so they stay in view however far along the song you are."""
        k = self.keys
        k.delete("all")
        k.configure(scrollregion=(0, 0, LEFT_W, h))
        for i in range(self.rows):
            pitch = self.high - i
            y = TOP_H + i * ROW_H
            if pitch % 12 in (1, 3, 6, 8, 10):
                k.create_rectangle(0, y, LEFT_W, y + ROW_H, fill=BLACK_ROW, outline="")
            if pitch % 12 == 0:
                k.create_line(0, y + ROW_H, LEFT_W, y + ROW_H, fill=GRID_BAR)
            k.create_text(LEFT_W - 6, y + ROW_H / 2, text=score.note_name(pitch),
                          anchor="e", fill=DIM if pitch % 12 else TEXT,
                          font=("Consolas", 7))
        for i, part in enumerate(self.lanes):
            y = TOP_H + (self.rows + i) * ROW_H
            k.create_rectangle(0, y, LEFT_W, y + ROW_H, fill=LANE_BG, outline="")
            k.create_line(0, y, LEFT_W, y, fill=GRID_BAR)
            k.create_text(LEFT_W - 6, y + ROW_H / 2, text=part.name[:8], anchor="e",
                          fill=NOISE_EDGE, font=("Consolas", 7))
        k.yview_moveto(self.canvas.yview()[0])

    def draw(self):
        c = self.canvas
        c.delete("all")
        s = self.score
        beats = self.total_beats()
        w = self.x_of(beats) + 40
        h = self.grid_bottom() + 10
        c.configure(scrollregion=(0, 0, w, h))
        self.draw_keys(h)

        for i in range(self.rows):
            pitch = self.high - i
            y = TOP_H + i * ROW_H
            if pitch % 12 in (1, 3, 6, 8, 10):
                c.create_rectangle(0, y, w, y + ROW_H, fill=BLACK_ROW, outline="")
            if pitch % 12 == 0:
                c.create_line(0, y + ROW_H, w, y + ROW_H, fill=GRID_BAR)

        for i, part in enumerate(self.lanes):
            y = TOP_H + (self.rows + i) * ROW_H
            c.create_rectangle(0, y, w, y + ROW_H, fill=LANE_BG, outline="")
            c.create_line(0, y, w, y, fill=GRID_BAR)

        cur_name = score.turn_name(s.order[self.sel_turn]) if s.order else None
        self.heads = []          # filled below, drawn in the frozen bar
        at = 0.0
        for ti, entry in enumerate(s.order):
            if not score.turn_on(entry):
                continue                  # a skipped turn is not on the grid
            name = score.turn_name(entry)
            sec = score.section(s, name)
            if sec is None:
                continue
            roots = score.sec_roots(s, sec)
            reps = score.sec_reps(s, sec)
            tr = score.sec_transpose(s, sec)
            span = score.sec_beats(s, sec)
            x0, x1 = self.x_of(at), self.x_of(at + span)
            col = self.section_colour(name)
            linked = (name == cur_name)
            if linked:
                # A faint wash over every turn of the selected section, so the
                # reach of an edit is visible in the song and not only in the strip.
                c.create_rectangle(x0, TOP_H, x1, self.grid_bottom(),
                                   fill=blend(col, BG, 0.88), outline="")
            c.create_line(x0, TOP_H, x0, self.grid_bottom(), fill=SEC_LINE, width=3)
            self.heads.append((x0, x1, ti, name, col, linked))
            group_beats = reps * s.cell
            b = at
            g = 0
            while b < at + span - 1e-9:
                x = self.x_of(b)
                if abs((b - at) % group_beats) < 1e-6:
                    c.create_line(x, TOP_H, x, self.grid_bottom(), fill=GRID_GROUP, width=2)
                    if g < len(roots):
                        c.create_text(x + 4, TOP_H + 8, anchor="w", fill=GRID_GROUP,
                                      text=score.note_name(roots[g] + tr),
                                      font=("Consolas", 7))
                    g += 1
                elif abs((b - at) % 4) < 1e-6:
                    c.create_line(x, TOP_H, x, self.grid_bottom(), fill=GRID_BAR)
                else:
                    c.create_line(x, TOP_H, x, self.grid_bottom(), fill=GRID)
                b += min(1.0, s.cell)
            at += span
        c.create_line(self.x_of(at), TOP_H, self.x_of(at), self.grid_bottom(),
                      fill=SEC_LINE, width=3)
        self.draw_ruler(w, at)

        # The part being edited is drawn solid and the others are dimmed. It comes
        # from the panel, which shows the parts of the selected section.
        self.draw_head()
        panel = self.panel_parts()
        chosen = panel[self.sel_part] if 0 <= self.sel_part < len(panel) else None

        for i, n in enumerate(self.notes):
            beat = n["t"] * s.tempo / 60.0
            length = n["life"] * s.tempo / 60.0
            mine = n["part"] is chosen
            wide = False
            x0, y0 = self.x_of(beat), self.note_y(n)
            x1 = max(x0 + 3, self.x_of(beat + length))
            # The colour says what a note IS, which decides what dragging it does.
            if n["part"].mute:
                fill, edge = MUTE_FILL, MUTE_EDGE
            elif n["part"].wave == "noise":
                fill, edge = NOISE_FILL, NOISE_EDGE
            elif n["part"].kind == "free":
                fill, edge = FREE_FILL, FREE_EDGE
            elif n["tok"].startswith("R"):
                fill, edge = ROOT_FILL, ROOT_EDGE
            else:
                fill, edge = PATTERN_FILL, PATTERN_EDGE
            if self.note_key(n) in self.sel_notes:
                edge, wide = SEL_EDGE, True
            elif self.sel_note == i:
                edge = SEL_EDGE
            c.create_rectangle(x0, y0 + 1, x1, y0 + ROW_H - 1, fill=fill,
                               outline=edge, width=3 if wide else (2 if mine else 1),
                               stipple="" if (mine or wide) else "gray50",
                               tags=("note", "n%d" % i))

    # ----------------------------------------------------------------- the report

    def report(self):
        s, out = self.score, []
        beat = 60.0 / s.tempo
        sec = self.cur_section()
        out.append("%s   %.1f s" % (s.name, self.total_beats() * beat))
        out.append("%s in the order" % score.plural(len(s.order), "turn"))
        if sec is not None:
            turns = self.linked_turns(sec.name)
            out.append("editing %s (%s)"
                       % (sec.name, score.plural(score.sec_beats(s, sec) / 4.0, "bar")))
            if len(turns) > 1:
                out.append("  LINKED: turns %s"
                           % ", ".join(str(k + 1) for k in turns))
                out.append("  an edit changes them all")
            else:
                out.append("  on its own")
            out.append("  %d groups x %d reps"
                       % (len(score.sec_roots(s, sec)), score.sec_reps(s, sec)))
        out.append("%g BPM   %d notes" % (s.tempo, len(self.notes)))
        if s.transpose:
            out.append("transpose %+d" % s.transpose)
        out.append("")
        peak = score.peak_voices(self.notes)
        out.append("VOICES  peak %d of 10" % peak)
        if peak > 4:
            out.append("  too many. An alert")
            out.append("  will lose a voice.")
        out.append("")
        out.append("SPEAKER  under %.0f Hz" % score.SPEAKER_FLOOR)
        seen = {}
        for n in self.notes:
            seen.setdefault((n["part"].name, n["n"]), n)
        # Show only the notes with a problem. A long melody would fill the panel.
        weak = []
        for (pname, num), n in sorted(seen.items(), key=lambda kv: -kv[0][1]):
            v, _ = score.speaker_check(n["part"].wave, score.hz(num), n["part"].lp)
            if v != "ok":
                weak.append("  %-7s %-4s %6.1f %s" % (pname[:7], score.note_name(num),
                                                      score.hz(num), v))
        if not weak:
            out.append("  all %d notes are ok" % len(seen))
        else:
            out.extend(weak[:14])
            if len(weak) > 14:
                out.append("  ... and %d more" % (len(weak) - 14))
            out.append("  %d of %d weak or lost." % (len(weak), len(seen)))
            out.append("  Add: transpose 12")

        fixed = []
        for p in self.panel_parts():
            if p.wave == "noise" or p.sweep:
                continue          # percussion does not stand against the root
            for tok in p.pattern:
                if (tok not in (".", "-", "X") and not score.ROOT_RE.match(tok)
                        and tok not in fixed):
                    fixed.append(tok)
        if fixed and len(score.sec_roots(s, sec) if sec else s.roots) > 1:
            out.append("")
            out.append("HARMONY vs %s" % " ".join(fixed))
            roots = score.sec_roots(s, sec) if sec else s.roots
            tr = score.sec_transpose(s, sec) if sec else s.transpose
            for r in roots:
                gaps = " ".join("%2d" % (score.note_num(t) - r) for t in fixed)
                out.append("  %-4s %s st" % (score.note_name(r + tr), gaps))

        self.info.configure(state="normal")
        self.info.delete("1.0", "end")
        self.info.insert("1.0", "\n".join(out))
        self.info.configure(state="disabled")

    def say(self, text):
        self.status.configure(text=text)

    # ------------------------------------------------------------------- editing

    def on_part(self, _=None):
        """Kept as a name other code can call. pick_part does the work."""
        self.pick_part(self.sel_part)

    def on_add_part(self):
        """Add a part from a preset. score.PRESETS holds the settings."""
        self.mark()
        sec = self.cur_section()
        p = score.make_part(self.preset.get(),
                            taken=[q.name for q in score.all_parts(self.score)],
                            kind=self.kind_var.get())
        # EVERY NEW PART GOES IN THE BASE, whatever its kind. That is one rule
        # instead of a guess, and the part is then visible in every turn. Use
        # `Make this part local to the section` to narrow it to one section.
        self.score.parts.append(p)
        where = "in every section"
        self.show_parts()
        self.sel_part = [q.name for q in self.panel_parts()].index(p.name)
        self.show_parts()
        self.rebuild()
        shape = ("free, so every note stands on its own"
                 if p.kind == "free" else
                 "a pattern, so its cell repeats in every group")
        self.say("added %s, empty, %s. It is %s. %s"
                 % (p.name, where, shape,
                    score.HINTS.get(self.preset.get(), "Click the grid.")))

    def hit(self, ev):
        """Return the index of the note under the pointer, or None."""
        x, y = self.canvas.canvasx(ev.x), self.canvas.canvasy(ev.y)
        for item in reversed(self.canvas.find_overlapping(x, y, x, y)):
            for tag in self.canvas.gettags(item):
                if tag.startswith("n") and tag[1:].isdigit():
                    return int(tag[1:])
        return None

    def snap(self):
        return min(0.25, self.score.cell / 4.0)

    def note_key(self, n):
        """A key for a note that survives a rebuild. Only a free part has one: a
        slot of a pattern part is shared by every group, so a group of them cannot
        be moved as notes."""
        return (id(n["part"]), n["src"]) if n["src"] >= 0 else None

    def part_by_id(self, pid):
        for p in score.all_parts(self.score):
            if id(p) == pid:
                return p
        return None

    def note_span(self, i):
        """Return the left and the right x of the box of one note."""
        n = self.notes[i]
        beat = n["t"] * self.score.tempo / 60.0
        length = n["life"] * self.score.tempo / 60.0
        x0 = self.x_of(beat)
        return x0, max(x0 + 3, self.x_of(beat + length))

    def on_edge(self, i, ev):
        """Return True if the pointer is on the right edge of the note."""
        return abs(self.canvas.canvasx(ev.x) - self.note_span(i)[1]) <= EDGE_PX

    def on_set_head(self, ev):
        """The middle button puts the playhead where you click, so you can play
        one part of the song without waiting for the rest."""
        beat = max(0.0, self.beat_of(ev.x))
        self.head_ms = beat * 60.0 / self.score.tempo * 1000.0
        if self.live.alive:
            self.live.seek(self.head_ms)
        self.draw_head()
        self.say("playhead at %s" % self.head_text())

    def on_hover(self, ev):
        """Show the resize pointer over the right edge of a note."""
        cursor = ""
        if not self.drag and self.score.parts:
            i = self.hit(ev)
            parts = self.panel_parts()
            if (i is not None and parts
                    and self.notes[i]["part"] is parts[min(self.sel_part, len(parts) - 1)]
                    and self.on_edge(i, ev)):
                cursor = "sb_h_double_arrow"
        if self.canvas["cursor"] != cursor:
            self.canvas.configure(cursor=cursor)

    def on_click(self, ev):
        turn = self.turn_at(self.beat_of(ev.x))
        if turn != self.sel_turn:
            self.pick_turn(turn)
        parts = self.panel_parts()
        if not parts:
            return
        self.sel_part = min(self.sel_part, len(parts) - 1)
        part = parts[self.sel_part]
        i = self.hit(ev)
        if i is not None and self.note_key(self.notes[i]) in self.sel_notes:
            # Move every picked note together, keeping what each one is now.
            items = []
            for pid, idx in self.sel_notes:
                q = self.part_by_id(pid)
                if q is not None and idx < len(q.events):
                    at, pitch, length = q.events[idx]
                    items.append([q, idx, at, pitch, length])
            if items:
                self.drag = {"mode": "group", "x": ev.x, "y": ev.y, "items": items}
                self.hold()
                self.sel_note = i
                self.draw()
                return
        if i is not None and self.notes[i]["part"] is part:
            self.sel_note = i
            self.drag = None
            edge = self.on_edge(i, ev)
            if part.kind == "free":
                k = self.notes[i]["src"]
                at, pitch, length = part.events[k]
                self.drag = {"mode": "size" if edge else "move", "x": ev.x, "y": ev.y,
                             "part": part, "k": k, "beat": at, "pitch": pitch,
                             "len": length}
            elif edge:
                # A pattern part holds a note longer with a tie in the next slot.
                self.drag = {"mode": "tie", "x": ev.x, "part": part,
                             "slot": self.notes[i]["slot"],
                             "pattern": list(part.pattern)}
            elif part.wave != "noise":
                # A pattern note is one of two things, and they must not be
                # confused. A note from an R slot FOLLOWS the root of its group,
                # so dragging it moves that one root. A note written as a name is
                # fixed, and it is the same in every group, so dragging it changes
                # every group.
                note = self.notes[i]
                if note["tok"].startswith("R"):
                    sec = score.section(
                        self.score, score.turn_name(self.score.order[note["sec"]]))
                    roots = score.sec_roots(self.score, sec)
                    self.drag = {"mode": "root", "x": ev.x, "y": ev.y, "part": part,
                                 "group": note["group"], "roots": roots,
                                 "root": roots[note["group"]]}
                    self.say("%s follows the root of group %d. Drag it to move that "
                             "root. The other groups stay."
                             % (score.note_name(note["n"]), note["group"] + 1))
                else:
                    self.drag = {"mode": "fixed", "x": ev.x, "y": ev.y, "part": part,
                                 "slot": note["slot"],
                                 "pitch": note["n"] - self.score.transpose}
                    self.say("%s is fixed in EVERY group. Drag it and all four move."
                             % note["tok"])
            if self.drag:
                self.hold()
            self.hear(part, self.notes[i]["n"])
            self.draw()
            return
        if self.sel_notes:
            self.sel_notes = set()
            self.draw()
        beat = max(0.0, round(self.beat_of(ev.x) / self.snap()) * self.snap())
        # A noise part has no pitch, so where the pointer sits up or down does not
        # matter. Anywhere on the grid writes a hit in the slot under the pointer.
        pitch = self.pitch_of(ev.y) - self.score.transpose
        if part.wave != "noise" and self.canvas.canvasy(ev.y) > TOP_H + self.rows * ROW_H:
            self.say("that strip belongs to a noise part. Pick it to write a hit.")
            return
        self.mark()
        if part.kind == "free":
            # An event of a free part is a beat inside ITS SECTION, not the song.
            local = max(0.0, beat - self.turn_start(self.sel_turn))
            part.events.append((local, pitch, float(self.length.get())))
            self.say("added %s at beat %g of %s"
                     % (score.note_name(pitch), local, self.cur_section().name))
            self.hear(part, pitch + self.score.transpose)
        else:
            self.set_slot(part, beat, pitch)
            self.hear(part, pitch + self.score.transpose)
        self.sel_note = None
        self.rebuild()

    def set_slot(self, part, beat, pitch):
        """Put a note in one slot of a pattern part. Every group changes."""
        s = self.score
        sec = self.cur_section()
        roots = score.sec_roots(s, sec) if sec else s.roots
        reps = score.sec_reps(s, sec) if sec else s.reps
        slots = len(part.pattern)
        if not slots:
            return
        local = max(0.0, beat - self.turn_start(self.sel_turn))
        step = s.cell / slots
        idx = int((local % s.cell) / step) % slots
        g = min(int(local // (reps * s.cell)), len(roots) - 1)
        root = roots[g]
        # A noise part has no pitch, so every hit of it is X. A part that sweeps is
        # percussion, and it must NOT follow the root: a kick that changed pitch
        # with each group would be a different drum four times.
        if part.wave == "noise":
            part.pattern[idx] = "X"
        elif part.sweep:
            part.pattern[idx] = score.note_name(pitch)
        else:
            part.pattern[idx] = "R" if pitch == root else score.note_name(pitch)
        self.say("slot %d is now %s. A pattern part changes in every group."
                 % (idx + 1, part.pattern[idx]))

    def on_move(self, ev):
        """Drag a note. The drag holds the note as it was when the drag started,
        so a drag back to the start puts everything back."""
        d = self.drag
        if not d:
            return
        snap = self.snap()
        db = (ev.x - d["x"]) / self.px_per_beat
        part = d.get("part")      # a group drag holds many parts, not one

        if d["mode"] == "move":
            beat = max(0.0, d["beat"] + round(db / snap) * snap)
            pitch = d["pitch"] + int(round((d["y"] - ev.y) / float(ROW_H)))
            if (beat, pitch, d["len"]) == part.events[d["k"]]:
                return
            self.commit()
            part.events[d["k"]] = (beat, pitch, d["len"])
            if pitch != d.get("last", d["pitch"]):
                d["last"] = pitch
                self.hear(part, pitch + self.score.transpose)
            self.rebuild()
            self.say("%s at beat %g"
                     % (score.note_name(pitch + self.score.transpose), beat))

        elif d["mode"] == "size":
            # The box shows what sounds, which is the length times the gate. Take
            # the drag off the box, then turn it back into a length.
            gate = max(0.05, part.gate)
            length = max(snap, round((d["len"] * gate + db) / gate / snap) * snap)
            if (d["beat"], d["pitch"], length) == part.events[d["k"]]:
                return
            self.commit()
            part.events[d["k"]] = (d["beat"], d["pitch"], length)
            self.rebuild()
            self.say("%s is %s long"
                     % (score.note_name(d["pitch"] + self.score.transpose),
                        score.plural(length, "beat")))

        elif d["mode"] == "group":
            db = round(db / snap) * snap
            # SHIFT KEEPS THE PITCH. Held down, the group only moves in time.
            dp = 0 if (ev.state & SHIFT) else int(round((d["y"] - ev.y) / float(ROW_H)))
            want = [(max(0.0, at + db), pitch + dp, length)
                    for _q, _i, at, pitch, length in d["items"]]
            if all(q.events[i] == w for (q, i, _a, _p, _l), w in zip(d["items"], want)):
                return
            self.commit()
            for (q, i, _a, _p, _l), w in zip(d["items"], want):
                q.events[i] = w
            self.rebuild()
            self.say("moved %s by %g beats%s"
                     % (score.plural(len(want), "note"), db,
                        ", pitch held" if (ev.state & SHIFT) else
                        (" and %+d semitones" % dp if dp else "")))

        elif d["mode"] == "root":
            # Move the root of one group. Every part that follows the root moves.
            new = d["root"] + int(round((d["y"] - ev.y) / float(ROW_H)))
            if new == d["roots"][d["group"]]:
                return
            self.commit()
            d["roots"][d["group"]] = new
            if new != d.get("last", d["root"]):
                d["last"] = new
                self.hear(part, new + self.score.transpose)
            self.rebuild()
            self.say("group %d root is %s. Every part that follows the root moved with it."
                     % (d["group"] + 1,
                        score.note_name(new + self.score.transpose)))

        elif d["mode"] == "fixed":
            new = d["pitch"] + int(round((d["y"] - ev.y) / float(ROW_H)))
            tok = score.note_name(new)
            if tok == part.pattern[d["slot"]]:
                return
            self.commit()
            part.pattern[d["slot"]] = tok
            self.rebuild()
            self.say("slot %d is %s, in every group." % (d["slot"] + 1, tok))

        else:
            slot_beats = self.score.cell / len(part.pattern)
            room = len(part.pattern) - 1 - d["slot"]
            want = max(0, int(round(db / slot_beats)))
            extra = min(want, room)
            pattern = list(d["pattern"])
            for j in range(1, extra + 1):
                pattern[d["slot"] + j] = "-"
            if pattern == part.pattern:
                return
            self.commit()
            part.pattern[:] = pattern
            self.rebuild()
            note = "slot %d holds for %s, in every group." % (d["slot"] + 1,
                                                             score.plural(extra + 1, "slot"))
            if want > room:
                note += " A note cannot hold past the end of the group."
            self.say(note)

    def on_release(self, _):
        was = self.drag
        self.drag = None
        self.pending = None
        if was:
            # The drag is over, so the range of pitches can move again and the
            # engine gets the result. rebuild does both.
            self.rebuild()

    def on_right_press(self, ev):
        self.marquee = {"x": ev.x, "y": ev.y, "moved": False}

    def on_right_motion(self, ev):
        m = self.marquee
        if m is None:
            return
        if abs(ev.x - m["x"]) > MARQUEE_PX or abs(ev.y - m["y"]) > MARQUEE_PX:
            m["moved"] = True
        self.canvas.delete("marquee")
        c = self.canvas
        c.create_rectangle(c.canvasx(m["x"]), c.canvasy(m["y"]),
                           c.canvasx(ev.x), c.canvasy(ev.y),
                           outline=SEL_EDGE, dash=(3, 2), tags="marquee")

    def on_right_release(self, ev):
        m, self.marquee = self.marquee, None
        self.canvas.delete("marquee")
        if m is None:
            return
        if not m["moved"]:
            self.remove_note(ev)          # a plain right click still removes
            return

        c = self.canvas
        x0, x1 = sorted((c.canvasx(m["x"]), c.canvasx(ev.x)))
        y0, y1 = sorted((c.canvasy(m["y"]), c.canvasy(ev.y)))
        picked, skipped = set(), 0
        for i, n in enumerate(self.notes):
            key = self.note_key(n)
            nx0, nx1 = self.note_span(i)
            ny0 = self.note_y(n)
            if nx1 < x0 or nx0 > x1 or ny0 + ROW_H < y0 or ny0 > y1:
                continue
            if key is None:
                skipped += 1
            else:
                picked.add(key)
        self.sel_notes = picked
        self.draw()
        if picked:
            self.say("%s picked. Drag one to move them all. Hold shift to keep "
                     "the pitch." % score.plural(len(picked), "note")
                     + ("  %d slot note(s) were left out: a slot is shared by "
                        "every group." % skipped if skipped else ""))
        else:
            self.say("nothing picked. Only the notes of a free part can be picked.")

    def remove_note(self, ev):
        i = self.hit(ev)
        if i is None:
            self.sel_notes = set()
            self.draw()
            return
        n = self.notes[i]
        part = n["part"]
        self.mark()
        if part.kind == "free":
            part.events.pop(n["src"])
            self.sel_notes = set()
            self.say("removed %s" % score.note_name(n["n"]))
        else:
            part.pattern[n["slot"]] = "."
            self.say("slot %d is now a rest, in every group." % (n["slot"] + 1))
        self.sel_note = None
        self.drag = None
        self.rebuild()

    def zoom(self, f):
        self.px_per_beat = max(12.0, min(400.0, self.px_per_beat * f))
        self.draw()


def main():
    # Tell Windows that this program sets its own pixels. Without this the window
    # is blurred on a screen that scales above 100 per cent.
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    path = sys.argv[1] if len(sys.argv) > 1 else None
    root = tk.Tk()
    studio = Studio(root, path)

    def leave():
        studio.live.close()      # the engine holds the sound card, so let it go
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", leave)
    root.mainloop()


if __name__ == "__main__":
    main()
