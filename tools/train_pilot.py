"""Train a small network to fly the ship, and write the weights as a C header.

The game writes a dataset with the --dump option. Each row holds one
observation and the control that the pilot used at that moment. This script
reads that file and fits a network to it.

The network copies the pilot. It does not learn to win. If the recorded pilot
flies badly, the network flies badly in the same way.

The script trains four outputs: pitch, yaw, throttle and the trigger.

WHAT JITTER DOES AND DOES NOT BUY. The airframe fields are nudged while
training, and the same figure becomes how far a ship may be retuned and still be
flown by the network. The second half is the important half. Measured, with one
class's hull moved 11% -- an ordinary tuning pass -- as the hull a trained opponent
takes off the player per minute:

    flown by                        stock    retuned
    hand-written tactics            0.18%      0.18%
    network, no jitter              1.85%      0.18%
    network, jittered               1.45%      0.38%

Without jitter the gate rejects the retuned ship and the class goes back to the
rules -- the policy did not get worse, it stopped being asked. With jitter it is
still asked. But it is much weaker than it was: most of the advantage goes
anyway.

So jitter is insurance against a tuning pass silently switching the network off.
It is NOT the thing that makes a policy survive a retune. Fitting to one table
teaches one table, and an input that is noised is not an environment that varied.

THE TRIGGER IS A QUESTION ABOUT THE NEXT SECOND, not about this frame. A press
is 0.3% of rows in one recording, and a model that never fires is 99.7% correct
on that. Asked instead whether the pilot fires WITHIN the horizon, the same data
answers yes on 16% to 31% of rows, which is an ordinary question.

That is the right question anyway. For a sniper, when to fire is the skill: in
one recording the pilot held a full lock on 69.5% of frames and fired on 0.4% of
them. A lock is permission. The decision is what this learns.

THE NETWORK GIVES A TARGET, NOT A STICK POSITION. It says where the stick must
go over the next half second. The caller then moves the stick toward that
target. A hand moves this way, and the measurements say the game must too.

The stick changes very little between two frames. Frame to frame, the pilot's
next stick position matches the last one to better than 0.998. So a network
that must give the position each frame learns to copy the last position, and
that teaches it nothing about flight.

The same measurement, at a range of horizons, shows where the useful signal
starts. The target is a mean over a window, so the window must reach further
than one moment does. These are the measured results for one recording:

    window    a still stick    the observation
    0.5 s          0.01854            0.03574     the still stick wins
    1.0 s          0.03512            0.02834     the observation wins
    2.0 s          0.04989            0.02382     the observation wins by 52%
    4.0 s          0.06097            0.02028     the observation wins by 67%

A longer window scores better on the test and does not fly better. Measured
properly -- paired across four fixed seeds, since two runs of the same build
differ -- as the hull a trained opponent takes off the player per minute:

    window     result           spread
    0.5 s      0.00%            0.00     dead: it never lands anything
    1.0 s      1.85%            0.72
    2.0 s      1.60%            1.37

Half a second is decisively wrong. One second and two are within noise of each
other, and one second is the default because it holds the same mean with half
the spread.

AN EARLIER SWEEP OF THIS RAN ONE SAMPLE PER HORIZON and reported 2.0s as much
the worst. That was noise. Pick the horizon by flying, not by the loss, and by
more than one flight.

Run this script from the root of the repository:

    python tools/train_pilot.py captures/ballista.obs

The output is src/vg/generated/pilot_net.h by default.
"""

import argparse
import os
import struct
import sys

import numpy as np

# Where the rules read from. These must agree with the enum in src/vg/vg_bot.h.
OBS_MSL_IN    = 17
OBS_MSL_RANGE = 21
OBS_RANGE_W   = 24

MAGIC = b"PHOB"
VERSION = 1

# The number of values a RECORDING holds for the hand: pitch, yaw, throttle,
# trigger. This is the width of the action in a dump file and it does not change.
STICK_OUT = 4

# Where the shared mode list lives. See the note at the top of that file: it is
# the one copy, and this script reads it so that the two cannot drift apart.
MODES_H = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "src", "vg", "vg_modes.h")


def read_modes(path=MODES_H):
    """Return the mode names, in the order the header lists them."""
    import re
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    names = [m[1] for m in re.findall(r'X\((\w+),\s*"([^"]+)"\)', text)]
    if not names:
        raise ValueError("%s holds no modes" % path)
    return names


MODE_NAMES = read_modes()

# WHAT EACH MODE LOOKS LIKE IN A RECORDING.
#
# One rule for each mode. A rule reads the flight and says which frames were that
# mode. It MAY look at the future, because it labels a recording that already
# happened. The network may not: it sees one frame and must predict the mode from
# that, which is what makes this worth learning instead of a rule to copy.
#
# Where two rules are true, the higher number wins. The lowest rule must always
# be true, because every frame needs a label.
#
# TO ADD A MODE: add a row to VG_MODE_LIST in src/vg/vg_modes.h, then add a rule
# here with the same name. A mode with no rule stops this script, on purpose.
#
# `a` is the observation rows, `n` is how many of them get a label, and `w` is
# how many frames ahead a rule may look.
def _drange(a, n, w):
    """How much the range to the target opens over the next w frames."""
    r = a[:, OBS_RANGE_W]
    return r[w:w + n] - r[:n]


MODE_RULES = {
    # Something is tracking us and it is close. This outranks everything: a
    # pilot who is being shot at is not doing anything else.
    "BREAK":  (30, lambda a, n, w: (a[:n, OBS_MSL_IN] > 0.5)
                                   & (a[:n, OBS_MSL_RANGE] < 0.45)),
    # The range closes over the window. They are going in.
    "PRESS":  (20, lambda a, n, w: _drange(a, n, w) < -0.03),
    # The range opens over the window, and nothing is chasing them. They chose
    # to leave.
    "EXTEND": (20, lambda a, n, w: _drange(a, n, w) > 0.03),
    # The default: the range holds. Working the lock, keeping the angle.
    "HOLD":   (0,  lambda a, n, w: np.ones(n, dtype=bool)),
}

missing = [m for m in MODE_NAMES if m not in MODE_RULES]
if missing:
    raise ValueError("these modes have no rule in MODE_RULES: %s" % ", ".join(missing))

# The number of values the NETWORK gives: the hand, then one score for each mode.
ACT_OUT = STICK_OUT + len(MODE_NAMES)


def label_modes(a, look, smooth):
    """Give every row a mode. Return one whole number for each row.

    `look` is how many frames ahead a rule may read. `smooth` is the width of a
    majority filter. The filter matters: without it about a third of the labels
    last less than half a second, and a network fitted to that learns to flicker
    rather than to hold a plan.
    """
    n = a.shape[0] - look
    if n <= 0:
        return np.zeros(0, dtype=np.int64)
    order = sorted(range(len(MODE_NAMES)),
                   key=lambda k: MODE_RULES[MODE_NAMES[k]][0])
    m = np.zeros(n, dtype=np.int64)
    for k in order:                      # lowest rule first, highest wins last
        rule = MODE_RULES[MODE_NAMES[k]][1]
        m[rule(a, n, look)] = k
    if smooth > 1:
        # A majority filter, done as a box sum for each mode. Much faster than a
        # window per row, and the same answer.
        box = np.ones(smooth)
        score = np.stack([np.convolve((m == k).astype(np.float32), box, mode="same")
                          for k in range(len(MODE_NAMES))], axis=1)
        m = score.argmax(axis=1).astype(np.int64)
    return m


def read_dataset(path):
    """Read one dump file. Return the observations and the controls."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12:
            raise ValueError("%s is too short to hold a header" % path)
        magic, packed, act_n = struct.unpack("<4sII", head)
        if magic != MAGIC:
            raise ValueError("%s is not a dump file" % path)
        version = packed >> 16
        obs_n = packed & 0xFFFF
        if version != VERSION:
            raise ValueError(
                "%s uses format version %d. This script reads version %d."
                % (path, version, VERSION))
        raw = np.fromfile(f, dtype=np.float32)

    row = obs_n + act_n
    rows = raw.size // row
    if rows == 0:
        raise ValueError("%s holds no rows" % path)
    if raw.size % row:
        print("warning: %s ends in a part row. The script drops it." % path)
    table = raw[: rows * row].reshape(rows, row)
    return table[:, :obs_n], table[:, obs_n:], obs_n, act_n


def build(obs_n, hidden, layers=2):
    """Stack `layers` hidden layers of `hidden` units."""
    import torch.nn as nn
    seq = [nn.Linear(obs_n, hidden), nn.Tanh()]
    for _ in range(layers - 1):
        seq += [nn.Linear(hidden, hidden), nn.Tanh()]
    seq += [nn.Linear(hidden, ACT_OUT)]
    return nn.Sequential(*seq)


def squash(y):
    """Put each output in the range that the control accepts."""
    import torch
    # Pitch and yaw move both ways from the middle. The throttle does not, and
    # the trigger is a probability.
    pitch_yaw = torch.tanh(y[:, :2])
    rest = torch.sigmoid(y[:, 2:4])
    # The mode scores are a choice between the modes, so they are made to add up
    # to one. The largest is the mode.
    modes = torch.softmax(y[:, STICK_OUT:], dim=1)
    return torch.cat([pitch_yaw, rest, modes], dim=1)


# The eleven airframe fields sit at the end of the observation. See vg_bot.h.
SHIP_FIELDS = 11

# Where the rack count sits in the observation. See the enum in vg_bot.h -- this
# is the one index this script needs to know by name.
OBS_ROUNDS = 14


def seen_ships(X, obs_n):
    """Return the distinct airframes in the data, one row each."""
    # The last SHIP_FIELDS columns do not change during a flight, so each
    # distinct row is one class that was recorded. Rounded, because the values
    # come back through float32 and two rows of the same class must not look
    # like two classes.
    ships = np.unique(np.round(X[:, obs_n - SHIP_FIELDS:], 4), axis=0)
    return ships


def emit_static(name, values, add, per_line=8):
    """Write one float array as a C initialiser."""
    add("static const float %s[] = {" % name)
    flat = np.asarray(values, dtype=np.float32).reshape(-1)
    for i in range(0, flat.size, per_line):
        add("    %s," % ", ".join("%.7ff" % v for v in flat[i:i + per_line]))
    add("};")


def write_header(path, model, mean, std, obs_n, meta, ships=None, fire_t=0.5,
                 ship_tol=0.005):
    """Write the weights as a C header that the firmware can compile."""
    import torch

    layers = [m for m in model if hasattr(m, "weight")]
    lines = []
    add = lines.append

    add("#pragma once")
    add("// GENERATED BY tools/train_pilot.py. DO NOT EDIT BY HAND.")
    add("//")
    add("// A small network that flies the ship. It gives pitch, yaw and")
    add("// throttle. It does not give the trigger.")
    add("//")
    for line in meta:
        add("// %s" % line)
    add("")
    if ships is not None and len(ships):
        add("// WHICH SHIPS THIS NETWORK HAS ACTUALLY SEEN, one row of airframe")
        add("// fields each. A policy asked to fly a class it was never trained on")
        add("// is guessing, and it guesses by flying the class it does know.")
        add("// The firmware compares the ship it is in against these and declines")
        add("// when there is no match.")
        add("// HOW FAR A SHIP MAY HAVE BEEN RETUNED AND STILL COUNT AS ONE OF")
        add("// THESE. It is the jitter the network was trained through, because")
        add("// that is exactly the spread of tables it has seen. Trained with no")
        add("// jitter, the tolerance is a rounding allowance -- and then any tuning")
        add("// pass at all hands the class straight back to the rules.")
        add("#define PILOT_SHIP_TOL %.4ff" % ship_tol)
        add("#define PILOT_SHIPS    %d" % len(ships))
        add("#define PILOT_SHIP_N   %d" % SHIP_FIELDS)
        emit_static("PILOT_SHIP_SEEN", ships, add)
        add("")
    add("// The cut for the trigger output, calibrated so the network fires about")
    add("// as often as the pilot it was fitted to. Not 0.5: the trigger is")
    add("// weighted up in training, which biases it toward saying yes.")
    add("#define PILOT_FIRE_T   %.4ff" % fire_t)
    add("")
    add("#define PILOT_NET_IN   %d" % obs_n)
    add("#define PILOT_NET_H    %d" % layers[0].out_features)
    add("#define PILOT_NET_L    %d" % (len(layers) - 1))
    add("#define PILOT_NET_OUT  %d" % ACT_OUT)
    add("")
    add("// HOW MANY MODES THESE WEIGHTS WERE FITTED FOR, and their order. The")
    add("// game checks this against VG_MODE_N in vg_modes.h and declines when")
    add("// they differ: a mode added since these weights were trained would")
    add("// otherwise be read off an output that means something else.")
    add("#define PILOT_MODE_N   %d" % len(MODE_NAMES))
    add("// %s" % "  ".join("%d=%s" % (k, n) for k, n in enumerate(MODE_NAMES)))
    add("")
    add("// The network needs each input near zero and near unit size. These are")
    add("// the mean and the deviation of the training data. Apply them first.")

    def emit(name, values, per_line=8):
        add("static const float %s[] = {" % name)
        flat = np.asarray(values, dtype=np.float32).reshape(-1)
        for i in range(0, flat.size, per_line):
            part = ", ".join("%.7ff" % v for v in flat[i:i + per_line])
            add("    %s," % part)
        add("};")

    emit("PILOT_IN_MEAN", mean)
    emit("PILOT_IN_STD", std)
    add("")
    for i, layer in enumerate(layers):
        w = layer.weight.detach().cpu().numpy()
        b = layer.bias.detach().cpu().numpy()
        add("// Layer %d: %d inputs, %d outputs." % (i, w.shape[1], w.shape[0]))
        emit("PILOT_W%d" % i, w)
        emit("PILOT_B%d" % i, b)
        add("")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(
        description="Train a small network to fly the ship.")
    ap.add_argument("dataset", nargs="+",
                    help="one or more dump files from the game")
    ap.add_argument("--out", default="src/vg/generated/pilot_net.h",
                    help="where to write the C header")
    ap.add_argument("--hidden", type=int, default=64,
                    help="width of each hidden layer (default 64)")
    ap.add_argument("--layers", type=int, default=2,
                    help="how many hidden layers (default 2)")
    ap.add_argument("--epochs", type=int, default=400,
                    help="how many passes over the data (default 400)")
    ap.add_argument("--batch", type=int, default=256, help="batch size")
    ap.add_argument("--lr", type=float, default=1e-3, help="learning rate")
    ap.add_argument("--val", type=float, default=0.15,
                    help="fraction of the data to hold back for the test")
    ap.add_argument("--seed", type=int, default=0, help="random seed")
    ap.add_argument("--split", choices=["time", "random"], default="time",
                    help="how to hold data back. Use time for an honest score.")
    ap.add_argument("--balance", choices=["ship", "none"], default="ship",
                    help="give every ship class the same weight, whatever share "
                         "of the recordings it happens to be")
    ap.add_argument("--jitter", type=float, default=0.12,
                    help="randomly scale the airframe fields by this much while "
                         "training. Also becomes how far a ship may be retuned "
                         "and still be flown. Default 0.12.")
    ap.add_argument("--mode-smooth", type=int, default=91,
                    help="width of the majority filter on the mode labels, in "
                         "frames (default 91, about 1.5 seconds)")
    ap.add_argument("--horizon", type=int, default=60,
                    help="frames to look ahead for the target (default 60)")
    args = ap.parse_args()

    try:
        import torch
        import torch.nn as nn
    except ImportError:
        print("error: this script needs PyTorch. Install it, then run again.")
        return 1

    xs, ys, obs_n, act_n = [], [], None, None
    bounds = []   # rows per file, so the split can be made inside each one
    for path in args.dataset:
        x, y, n, a = read_dataset(path)
        if obs_n is None:
            obs_n, act_n = n, a
        elif (n, a) != (obs_n, act_n):
            print("error: %s has a different layout from the first file." % path)
            return 1
        print("read %-40s %7d rows" % (path, x.shape[0]))
        xs.append(x)
        ys.append(y)
        bounds.append(x.shape[0])

    X = np.concatenate(xs)
    Yraw = np.concatenate(ys)[:, :ACT_OUT]

    # THE TARGET IS THE MEAN STICK POSITION OVER THE NEXT FEW FRAMES.
    #
    # A mean, and not the position at one moment. A single frame that far ahead
    # holds the hand shake as well as the decision, and the mean removes it.
    h = max(1, args.horizon)
    if X.shape[0] <= h:
        print("error: the data is shorter than the horizon.")
        return 1
    box = np.ones(h) / float(h)
    # THE THREE AXES TAKE A MEAN AND THE TRIGGER TAKES A MAXIMUM, because they
    # are different questions. "Where will the stick be" averages; "will they
    # fire" does not -- one press inside the window is a yes.
    cols = [np.convolve(Yraw[:, i], box, mode="valid") for i in range(3)]

    # THE TRIGGER LABEL IS A LAUNCH, NOT A BUTTON PRESS.
    #
    # The recorded action is what the pilot's hand did, and a hand mashes. In one
    # AEGIS session only 56% of presses had both a lock and a round left -- the
    # class can fire 45 a minute and the recording holds 67. Learning from the
    # button teaches a policy to press when it cannot shoot.
    #
    # The rack count is in the observation, so a launch is visible directly: the
    # rounds went DOWN. That is ground truth and it needs no new recording. Only
    # decreases count, because the same column jumps up on a reload.
    rounds = X[:, OBS_ROUNDS]
    launched = np.zeros(len(rounds), dtype=np.float32)
    launched[1:] = (np.diff(rounds) < -1e-4).astype(np.float32)
    fired = (np.convolve(launched, np.ones(h), mode="valid") > 0.5).astype(np.float32)
    cols.append(fired)

    # THE MODE, which is the plan rather than the hand.
    #
    # It rides as one more column of whole numbers, not as a mean: averaging a
    # decision gives no decision. That is the whole reason a mode can use a long
    # window where the stick cannot -- the mean of "break, then press" is nothing,
    # but the majority of it is still a mode.
    # The window is h - 1, not h, so that a label lines up with each mean above:
    # a box filter of width h over N rows gives N - h + 1 of them.
    modes = label_modes(X, h - 1, args.mode_smooth)
    assert len(modes) == len(cols[0]), (len(modes), len(cols[0]))
    cols.append(modes.astype(np.float32))
    Y = np.stack(cols, axis=1).astype(np.float32)
    # The mean starts at the row it looks forward from, so drop the last rows
    # that have no full window after them.
    X = X[: Y.shape[0]]
    Ynow = Yraw[: Y.shape[0]]
    print("total %d rows, %d inputs, %d outputs" % (X.shape[0], obs_n, ACT_OUT))
    print("target: mean stick over the next %d frames (%.2f s)" % (h, h / 60.0))
    if X.shape[0] < 5000:
        print("warning: this is a small dataset. Record more flying.")

    fire = np.concatenate(ys)[:, 3]
    shots = float(launched.sum())
    print("button presses %d (%.2f%% of rows), actual launches %d -- %.0f%% of presses did nothing"
          % (int(fire.sum()), 100.0 * fire.mean(), int(shots),
             100.0 * (1.0 - shots / max(fire.sum(), 1.0))))

    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)

    # A held back part of the data measures the result.
    #
    # SPLIT BY TIME, NOT AT RANDOM. The rows come from one flown session at 60
    # rows a second, so two rows next to each other are almost the same row. A
    # random split puts one of a pair in the training set and the other in the
    # test set. The network then scores well because it saw the answer, not
    # because it learned to fly.
    #
    # A split by time holds back the END of the session. Every test row is then
    # from a fight that the network never saw.
    # SPLIT INSIDE EACH FILE, not across the set of them.
    #
    # Each recording is one flight of one class. A single cut across the whole
    # set puts the last file entirely in the test set, so the network trains on
    # one class and is tested on another. Measured: BALLISTA and CHARIOT together
    # scored +8% that way, and the same data scores far better when each file
    # gives up its own tail.
    if args.split == "time":
        tr_parts, va_parts, base = [], [], 0
        for m in bounds:
            m = min(m, X.shape[0] - base)
            if m <= 1:
                base += m
                continue
            c = base + int(m * (1.0 - args.val))
            tr_parts.append(np.arange(base, c))
            va_parts.append(np.arange(c, base + m))
            base += m
        tr = np.concatenate(tr_parts)
        va = np.concatenate(va_parts)
    else:
        cut = int(X.shape[0] * (1.0 - args.val))
        order = rng.permutation(X.shape[0])
        tr, va = order[:cut], order[cut:]
    print("split: %s. %d rows to train, %d rows to test."
          % (args.split, tr.size, va.size))

    # THE SCORE TO BEAT IS INERTIA, not the average control.
    #
    # A pilot that holds the stick still already predicts the near future well.
    # A network is only useful if it beats that, so this is the number that
    # matters. The average control is reported too, but it is a weak test.
    # Only the four hand columns. The mode column is a whole number, and a
    # squared error against it would mean nothing.
    hold = float(((Ynow[va] - Y[va][:, :STICK_OUT]) ** 2).mean())

    # BALANCE THE CLASSES, because in a fitted policy the mix of the data IS the
    # policy's priorities.
    #
    # Measured: adding a third recording of one class moved the set from 55/45 to
    # 38/62, and the under-represented class went from taking 1.45% of the
    # player's hull per minute to taking NONE, on four seeds out of four. Its
    # behaviour -- holding a lock through a long missile flight -- is exactly what a
    # set dominated by close-range flying will not teach.
    #
    # So each distinct airframe is weighted to the same total, and a session spent
    # on one class stops quietly outvoting the others.
    if args.balance == "ship":
        key = np.round(X[tr][:, obs_n - SHIP_FIELDS:], 4)
        _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
        share = cnt[inv].astype(np.float64)
        w = share.mean() / share
        # Resampled rather than loss-weighted: it keeps the batch composition even
        # as well as the totals, which matters when one class is a third of the set.
        take = rng.choice(len(tr), size=len(tr), replace=True, p=w / w.sum())
        tr = tr[take]
        print("balanced: %d ships, %d training rows resampled"
              % (len(cnt), len(tr)))

    mean = X[tr].mean(axis=0)
    std = X[tr].std(axis=0)
    # A column that never changes gives a deviation of zero. Division by zero
    # gives NaN, so set those columns to one. They add nothing either way.
    dead = std < 1e-6
    if dead.any():
        print("note: %d input columns never change. The script ignores them."
              % int(dead.sum()))
    std[dead] = 1.0

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    print("device: %s" % dev)

    Xn = ((X - mean) / std).astype(np.float32)
    xt = torch.tensor(Xn[tr], device=dev)
    yt = torch.tensor(Y[tr], device=dev)
    xv = torch.tensor(Xn[va], device=dev)
    yv = torch.tensor(Y[va], device=dev)

    model = build(obs_n, args.hidden, args.layers).to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    mse = nn.MSELoss()

    # THE TRIGGER IS WEIGHTED UP. Even asked about a whole second it is the
    # minority answer, and an unweighted fit gets a good score by rarely saying
    # yes. The weight is the ratio of the two classes, so both carry the same
    # total pull.
    pos = float(Y[tr][:, 3].mean())
    w_pos = (1.0 - pos) / max(pos, 1e-3)
    print("trigger says yes on %.1f%% of training rows, weighted x%.1f"
          % (100.0 * pos, w_pos))
    bce = nn.BCELoss(reduction="none")

    # THE RARE MODES ARE WEIGHTED UP, for the same reason the trigger is. BREAK
    # is about 3% of frames, because the pilot is not shot at often, and an
    # unweighted fit scores well by never choosing it -- which would drop exactly
    # the mode that survival depends on.
    counts = np.bincount(Y[tr][:, 4].astype(np.int64), minlength=len(MODE_NAMES))
    mw = counts.sum() / (len(MODE_NAMES) * np.maximum(counts, 1))
    print("modes in training: " + "  ".join(
        "%s %.1f%% (x%.1f)" % (MODE_NAMES[k], 100.0 * counts[k] / counts.sum(), mw[k])
        for k in range(len(MODE_NAMES))))
    mode_w = torch.tensor(mw, dtype=torch.float32, device=dev)

    def lossf(pred, truth):
        axes = mse(pred[:, :3], truth[:, :3])
        w = 1.0 + (w_pos - 1.0) * truth[:, 3]
        trig = (bce(pred[:, 3].clamp(1e-6, 1 - 1e-6), truth[:, 3]) * w).mean()
        # The mode is a choice, so it is scored by how much weight the network put
        # on the right one.
        mp = pred[:, STICK_OUT:].clamp(1e-6, 1.0)
        idx = truth[:, 4].long()
        mloss = -(torch.log(mp[torch.arange(len(idx), device=mp.device), idx])
                  * mode_w[idx]).mean()
        # Scaled so neither half drowns the other. The axes loss runs around
        # 0.03 and a balanced BCE around 0.7, so the trigger is divided down to
        # sit in the same range rather than dominating the gradient.
        return axes + 0.05 * trig + 0.05 * mloss

    # The score to beat. A pilot that always gives the average control gets
    # this loss. A network above it learned nothing.
    base = float(((yv[:, :4] - yt[:, :4].mean(dim=0)) ** 2).mean())
    print("baseline: always the average control  %.5f" % base)
    print("baseline: hold the stick still        %.5f   <- the one to beat" % hold)

    n = xt.shape[0]
    best = float("inf")
    best_state = None
    for epoch in range(args.epochs):
        model.train()
        perm = torch.randperm(n, device=dev)
        for i in range(0, n, args.batch):
            idx = perm[i:i + args.batch]
            xb = xt[idx]
            if args.jitter > 0.0:
                # THE AIRFRAME FIELDS, MOVED A LITTLE, AND ONLY THOSE.
                #
                # The data holds two ships. Nothing stops a network from reading
                # one field, deciding which of the two it is in, and running two
                # memorised modes -- and a network that did that would fly a third
                # class as one of the first two, and would break the first time
                # the class table is tuned.
                #
                # Nudging those columns each batch makes the exact values
                # unreliable, so the useful thing to learn from them is the
                # direction they point rather than the identity they spell.
                nz = xb.clone()
                k = xb.shape[1] - SHIP_FIELDS
                noise = 1.0 + (torch.rand(xb.shape[0], SHIP_FIELDS, device=dev)
                               * 2.0 - 1.0) * args.jitter
                nz[:, k:] = xb[:, k:] * noise
                xb = nz
            opt.zero_grad()
            loss = lossf(squash(model(xb)), yt[idx])
            loss.backward()
            opt.step()

        model.eval()
        with torch.no_grad():
            vl = float(lossf(squash(model(xv)), yv))
        if vl < best:
            best = vl
            best_state = {k: v.clone() for k, v in model.state_dict().items()}
        if epoch % 25 == 0 or epoch == args.epochs - 1:
            print("epoch %4d  test loss %.5f  best %.5f" % (epoch, vl, best))

    model.load_state_dict(best_state)
    model.eval()
    with torch.no_grad():
        pred = squash(model(xv)).cpu().numpy()
    truth = yv.cpu().numpy()

    print("")
    print("  output      error   spread of the data")
    for i, name in enumerate(["pitch", "yaw", "throttle"]):
        err = np.abs(pred[:, i] - truth[:, i]).mean()
        print("  %-10s %7.4f   %7.4f" % (name, err, truth[:, i].std()))
    # THE THRESHOLD IS CALIBRATED, NOT ASSUMED.
    #
    # Half is the obvious cut and it is the wrong one here. The trigger is
    # weighted up during training so that a rare answer is learned at all, and a
    # weighted fit is deliberately biased toward saying yes. At 0.5 this one said
    # yes on 58% of rows where the pilot said yes on 28%.
    #
    # So the cut is moved until the network fires as OFTEN as the pilot did. That
    # is a weak form of calibration and the right one here: the question is not
    # "is this frame a shot" but "does this pilot shoot about this much", and a
    # trigger that fires twice as often as its teacher is not copying them.
    t = truth[:, 3] >= 0.5
    want = float(t.mean())
    fire_t = 0.5
    best_gap = 1e9
    for cand in np.linspace(0.05, 0.95, 91):
        gap = abs(float((pred[:, 3] >= cand).mean()) - want)
        if gap < best_gap:
            best_gap, fire_t = gap, float(cand)
    print("  trigger     threshold %.2f, chosen so it fires as often as the pilot did"
          % fire_t)
    p = pred[:, 3] >= fire_t
    tp = float((p & t).sum()); fp = float((p & ~t).sum()); fn = float((~p & t).sum())
    print("  trigger     says yes on %.1f%% of rows, truth %.1f%%"
          % (100.0 * p.mean(), 100.0 * t.mean()))
    print("              caught %.0f%% of the shots, %.0f%% of its calls were right"
          % (100.0 * tp / max(tp + fn, 1), 100.0 * tp / max(tp + fp, 1)))
    # THE HAND AND THE PLAN ARE SCORED APART.
    #
    # `best` is the whole loss, and it now holds a mode term as well as the hand.
    # The two baselines below are about the HAND only, so comparing them with the
    # whole loss says the network got worse the moment modes were added, which is
    # not what happened. The hand is scored against the hand.
    stick = float(((pred[:, :STICK_OUT] - truth[:, :STICK_OUT]) ** 2).mean())
    print("")
    print("hand: test error %.5f" % stick)
    print("  against the average control  %.5f  (%+.0f%%)"
          % (base, 100.0 * (1.0 - stick / base)))
    print("  against holding the stick    %.5f  (%+.0f%%)"
          % (hold, 100.0 * (1.0 - stick / hold)))
    if stick >= hold:
        print("The hand does not beat a still stick. It is not ready to fly.")

    # THE PLAN, scored as what it is: a choice, judged by how often it is right,
    # and judged per mode because the rare ones are the whole point. A network
    # that never says BREAK can still look good overall.
    got = pred[:, STICK_OUT:].argmax(axis=1)
    want = truth[:, 4].astype(np.int64)
    print("mode: right on %.0f%% of rows" % (100.0 * (got == want).mean()))
    for k, nm in enumerate(MODE_NAMES):
        m = want == k
        if not m.any():
            continue
        picked = got == k
        print("  %-7s %5.1f%% of rows | caught %3.0f%% | %3.0f%% of its calls right"
              % (nm, 100.0 * m.mean(), 100.0 * (got[m] == k).mean(),
                 100.0 * (want[picked] == k).mean() if picked.any() else 0.0))
    print("  a guess that always said the commonest mode would be right on %.0f%%"
          % (100.0 * np.bincount(want, minlength=len(MODE_NAMES)).max() / len(want)))
    print("best test loss %.5f  (hand and plan together)" % best)

    params = sum(p.numel() for p in model.parameters())
    meta = [
        "Rows: %d. Inputs: %d. Hidden: %d." % (X.shape[0], obs_n, args.hidden),
        "Weights: %d, which is %d bytes as float32." % (params, params * 4),
        "Test loss: %.5f. A still stick scores %.5f." % (best, hold),
        "The output is a target: the mean stick over the next %d frames," % h,
        "and whether the pilot fires within that window.",
    ]
    write_header(args.out, model, mean, std, obs_n, meta, seen_ships(X, obs_n), fire_t,
                 max(0.005, args.jitter))
    print("")
    print("wrote %s" % args.out)
    print("%d weights, %d bytes as float32" % (params, params * 4))
    return 0


if __name__ == "__main__":
    sys.exit(main())
