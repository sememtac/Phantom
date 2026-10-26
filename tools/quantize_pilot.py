"""Make an int8 version of a trained pilot network, and run it the way the board does.

The float network is too slow on the board: about 1 ms a call in a live fight,
against a budget of 0.5 ms a frame. Most of that time is the 28 KB of float
weights that the canopy pushes out of the data cache. An int8 network is a quarter
of the size, and its weights go in internal RAM (DRAM_ATTR), where the cache does
not matter.

THE ARITHMETIC IS INTEGER FROM END TO END. The inputs are put on an integer scale
with integer arithmetic, the sums are int32, the scales are fixed-point multipliers,
and tanh is a table. So with the same observation, the PC and the board give the
same bits. This script holds the reference of that arithmetic. The firmware must
agree with it exactly, and --check tests that.

    python tools/quantize_pilot.py captures/ai_eval/m2/pilot_net_mixed32.h --out q.h
    python tools/quantize_pilot.py q.h --check captures/m1_aegis_jrn_9.obs --exe phantom.exe

The output header holds the float network too, unchanged, so that the inspector
and the ship gate still read it. The firmware uses the int8 arrays when the header
defines PILOT_NET_INT8.
"""
import argparse
import math
import os
import struct
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inspect_pilot as ip  # noqa: E402  defines(), arrays()

# THE FIXED POINTS. They must agree with vg_net.cpp, which reads them from the header.
IN_RANGE = 6.0        # an input of +-6 standard deviations is the int8 limit
IN_SCALE = 127.0 / IN_RANGE
IN_FIX = 65536        # an observation becomes an integer at this scale first
IN_SHIFT = 32         # the shift of the input multiplier
SHIFT = 24            # the shift of every layer multiplier
LUT_K = 64            # tanh table entries for each unit of the pre-activation
LUT_HALF = 256        # the table covers -4 .. 4, so 2 * 256 + 1 entries
STICK_OUT = 4


def read_header(path):
    text = open(path, encoding="utf-8").read()
    return text, ip.defines(text), ip.arrays(text)


def quantize(d, a):
    """Give the integer arrays of a float network, as a dictionary."""
    n_in, nh, n_out = d["PILOT_NET_IN"], d["PILOT_NET_H"], d["PILOT_NET_OUT"]
    n_l = d.get("PILOT_NET_L", 2)
    q = {"n_in": n_in, "nh": nh, "n_out": n_out, "n_l": n_l}

    mean = a["PILOT_IN_MEAN"].astype(np.float64)
    std = a["PILOT_IN_STD"].astype(np.float64)
    q["in_mean"] = np.round(mean * IN_FIX).astype(np.int64)
    mul = np.round(IN_SCALE / std / IN_FIX * (1 << IN_SHIFT)).astype(np.int64)
    if mul.max() >= 2 ** 31:
        raise ValueError("an input has a deviation too small for the input multiplier")
    q["in_mul"] = mul

    xs = 1.0 / IN_SCALE                      # the real value of one input step
    for l in range(n_l + 1):
        n_o = n_out if l == n_l else nh
        w = a["PILOT_W%d" % l].astype(np.float64).reshape(n_o, -1)
        b = a["PILOT_B%d" % l].astype(np.float64)
        ws = np.abs(w).max(axis=1) / 127.0
        ws[ws == 0.0] = 1e-12
        q["w%d" % l] = np.clip(np.round(w / ws[:, None]), -127, 127).astype(np.int64)
        q["b%d" % l] = np.round(b / (ws * xs)).astype(np.int64)
        # The multiplier that turns a sum into a table index: real * LUT_K.
        q["m%d" % l] = np.round(ws * xs * LUT_K * (1 << SHIFT)).astype(np.int64)
        if l == n_l:
            # The throttle is a sigmoid, tanh of half the value.
            q["m_half"] = int(round(ws[2] * xs * LUT_K / 2.0 * (1 << SHIFT)))
            # The trigger: sigmoid(z) >= T is z >= logit(T), as a sum.
            t = float(d.get("PILOT_FIRE_T", 0.5))
            logit = math.log(t / (1.0 - t))
            q["fire_acc"] = int(math.ceil(logit / (ws[3] * xs)))
            # One cut for each ship, when the header has them (PILOT_FIRE_T_SHIP).
            if "PILOT_FIRE_T_SHIP" in a:
                q["fire_acc_ship"] = np.array(
                    [int(math.ceil(math.log(tk / (1.0 - tk)) / (ws[3] * xs)))
                     for tk in a["PILOT_FIRE_T_SHIP"]], dtype=np.int64)
            # The modes: the largest real value wins, so compare sum * scale.
            q["m_mode"] = np.round(ws[STICK_OUT:] * xs * (1 << SHIFT)).astype(np.int64)
        else:
            xs = 1.0 / 127.0                 # a hidden unit is tanh * 127
        for k in ("m%d" % l,):
            if q[k].max() >= 2 ** 31:
                raise ValueError("layer %d: a multiplier does not fit in 32 bits" % l)
    q["lut"] = np.round(np.tanh(np.arange(-LUT_HALF, LUT_HALF + 1) / LUT_K)
                        * 127.0).astype(np.int64)
    # The ship table, to find which cut a row uses. The same rule as the gate.
    if "PILOT_SHIP_SEEN" in a:
        n = int(d.get("PILOT_SHIP_N", 11))
        q["ships"] = a["PILOT_SHIP_SEEN"].astype(np.float32).reshape(-1, n)
        q["ship_tol"] = np.float32(d.get("PILOT_SHIP_TOL", 0.005))
    return q


def ship_rows(q, obs):
    """The first ship in the table within the tolerance on every field, or -1.

    In float32, as the firmware compares. The ships are far apart, so the answer
    does not depend on the last bit.
    """
    if "ships" not in q:
        return np.full(len(obs), -1, dtype=np.int64)
    n = q["ships"].shape[1]
    mine = np.asarray(obs, dtype=np.float32)[:, -n:]
    out = np.full(len(obs), -1, dtype=np.int64)
    for k in range(len(q["ships"]) - 1, -1, -1):        # the first match wins
        worst = np.abs(mine - q["ships"][k]).max(axis=1)
        out[worst <= q["ship_tol"]] = k
    return out


def lut_index(acc, m):
    """The table index of a sum: round(acc * m / 2^SHIFT), held inside the table.

    A sum is under 2^22 and a multiplier under 2^31, so the product fits in int64.
    numpy's >> on a negative int64 rounds down, the same as the C shift.
    """
    v = (np.asarray(acc, dtype=np.int64) * np.asarray(m, dtype=np.int64)
         + (1 << (SHIFT - 1))) >> SHIFT
    return np.clip(v, -LUT_HALF, LUT_HALF) + LUT_HALF


def forward_q(q, obs):
    """The board's forward pass, exactly. Give the integer outputs, one row each:
    pitch, yaw and throttle as table values (-127..127), the trigger (0 or 1) and
    the mode (-1 if the network has none)."""
    obs = np.asarray(obs, dtype=np.float32)
    # float32 times a power of two is exact, and the C cast truncates toward zero.
    xi = np.trunc(obs * np.float32(IN_FIX)).astype(np.int64)
    v = (xi - q["in_mean"]) * q["in_mul"]                  # fits in int64
    x = np.clip((v + (1 << (IN_SHIFT - 1))) >> IN_SHIFT, -127, 127)
    for l in range(q["n_l"]):
        acc = x @ q["w%d" % l].T + q["b%d" % l]
        x = q["lut"][lut_index(acc, q["m%d" % l])]
    acc = x @ q["w%d" % q["n_l"]].T + q["b%d" % q["n_l"]]
    m = q["m%d" % q["n_l"]]
    pitch = q["lut"][lut_index(acc[:, 0], m[0])]
    yaw = q["lut"][lut_index(acc[:, 1], m[1])]
    thr = q["lut"][lut_index(acc[:, 2], q["m_half"])]
    cut = np.full(len(obs), q["fire_acc"], dtype=np.int64)
    if "fire_acc_ship" in q:
        k = ship_rows(q, obs)
        cut[k >= 0] = q["fire_acc_ship"][k[k >= 0]]
    fire = (acc[:, 3] >= cut).astype(np.int64)
    if q["n_out"] > STICK_OUT:
        # The first of equal scores wins, as in the C loop and in np.argmax.
        score = acc[:, STICK_OUT:].astype(np.int64) * q["m_mode"]
        mode = np.argmax(score, axis=1).astype(np.int64)
    else:
        mode = np.full(len(obs), -1, dtype=np.int64)
    return np.stack([pitch, yaw, thr, fire, mode], axis=1)


def to_float(out):
    """The integer outputs as the floats the game uses: pitch, yaw, throttle."""
    p = out[:, 0] / 127.0
    y = out[:, 1] / 127.0
    t = 0.5 * (out[:, 2] / 127.0 + 1.0)
    return np.stack([p, y, t], axis=1)


def emit(name, values, ctype, add, attr=True, per_line=16):
    flat = np.asarray(values).reshape(-1)
    add("%sstatic const %s %s[] = {" % ("DRAM_ATTR " if attr else "", ctype, name))
    for i in range(0, flat.size, per_line):
        add("    %s," % ", ".join("%d" % int(v) for v in flat[i:i + per_line]))
    add("};")


def write(path, text, q):
    """Write the float header with the int8 arrays added at the end."""
    lines = []
    add = lines.append
    add("")
    add("// ---- THE INT8 NETWORK. GENERATED BY tools/quantize_pilot.py. ----")
    add("// vg_net.cpp uses these arrays instead of the float ones. DRAM_ATTR puts")
    add("// them in internal RAM on the board, out of the reach of the flash cache.")
    # stdint.h gives int8_t and int32_t. The PC build gets them through its forced
    # prelude, but the board does not, so the header must include it.
    add("#include <stdint.h>")
    add('#include "esp_attr.h"')
    add("#define PILOT_NET_INT8   1")
    add("#define PILOT_Q_IN_FIX   %d" % IN_FIX)
    add("#define PILOT_Q_IN_SHIFT %d" % IN_SHIFT)
    add("#define PILOT_Q_SHIFT    %d" % SHIFT)
    add("#define PILOT_Q_LUT_HALF %d" % LUT_HALF)
    add("#define PILOT_Q_M_HALF   %d" % q["m_half"])
    add("#define PILOT_Q_FIRE_ACC (%d)" % q["fire_acc"])
    emit("PILOT_Q_IN_MEAN", q["in_mean"], "int32_t", add)
    emit("PILOT_Q_IN_MUL", q["in_mul"], "int32_t", add)
    for l in range(q["n_l"] + 1):
        emit("PILOT_QW%d" % l, q["w%d" % l], "int8_t", add)
        emit("PILOT_QB%d" % l, q["b%d" % l], "int32_t", add)
        emit("PILOT_QM%d" % l, q["m%d" % l], "int32_t", add)
    if q["n_out"] > STICK_OUT:
        emit("PILOT_Q_M_MODE", q["m_mode"], "int32_t", add)
    emit("PILOT_Q_TANH", q["lut"], "int8_t", add)
    if "fire_acc_ship" in q:
        emit("PILOT_Q_FIRE_ACC_SHIP", q["fire_acc_ship"], "int32_t", add)
    size = sum(q["w%d" % l].size for l in range(q["n_l"] + 1)) + q["lut"].size + 4 * (
        2 * q["n_in"] + sum(2 * q["b%d" % l].size for l in range(q["n_l"] + 1))
        + (q["n_out"] - STICK_OUT))
    add("// Internal RAM for all of the above: %d bytes." % size)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text.rstrip("\n") + "\n" + "\n".join(lines) + "\n")
    return size


def read_obs(path):
    with open(path, "rb") as f:
        _, packed, act_n = struct.unpack("<4sII", f.read(12))
        raw = np.fromfile(f, dtype=np.float32)
    n = packed & 0xFFFF
    return raw[: raw.size // (n + act_n) * (n + act_n)].reshape(-1, n + act_n)[:, :n]


def write_blob(path, q, n_mode):
    """Write the int8 network as a file that the game loads with --net-file.

    The layout is written down in host/src/host_netfile.cpp, which reads it. The
    game refuses a file that does not fit, so a change here must go there too.
    """
    out = bytearray(b"PHNQ")
    out += struct.pack("<i", 1)
    ships = q.get("ships")
    n_ships = 0 if ships is None else ships.shape[0]
    ship_n = 0 if ships is None else ships.shape[1]
    has_fire_ship = 1 if "fire_acc_ship" in q else 0
    out += struct.pack("<14i", q["n_in"], q["nh"], q["n_l"], q["n_out"], n_mode,
                       IN_FIX, IN_SHIFT, SHIFT, LUT_HALF, q["m_half"], q["fire_acc"],
                       n_ships, ship_n, has_fire_ship)
    out += struct.pack("<f", float(q.get("ship_tol", 0.0)))

    def i32(v):
        return np.asarray(v, dtype="<i4").tobytes()

    out += i32(q["in_mean"]) + i32(q["in_mul"])
    for l in range(q["n_l"] + 1):
        out += np.asarray(q["w%d" % l], dtype=np.int8).tobytes()
        out += i32(q["b%d" % l]) + i32(q["m%d" % l])
    if n_mode:
        out += i32(q["m_mode"])
    out += np.asarray(q["lut"], dtype=np.int8).tobytes()
    if n_ships:
        out += np.asarray(ships, dtype="<f4").tobytes()
    if has_fire_ship:
        out += i32(q["fire_acc_ship"])
    out += b"END!"
    with open(path, "wb") as f:
        f.write(bytes(out))
    return len(out)


def check(header, obs_path, exe, rows, blob=None):
    """Run the game's own forward pass on recorded observations and compare.

    With `blob`, the game loads that file with --net-file and checks the loaded
    network's arithmetic instead of the compiled one's.
    """
    text, d, a = read_header(header)
    q = quantize(d, a)
    obs = read_obs(obs_path)[:rows]
    want = forward_q(q, obs)
    cmd = [exe, "--net-check", obs_path, "--frames", str(rows)]
    if blob:
        cmd += ["--net-file", blob]
    r = subprocess.run(cmd, capture_output=True, text=True)
    got = np.array([[int(v) for v in line.split()[1:]] for line in r.stdout.splitlines()
                    if line.startswith("q ")], dtype=np.int64)
    if got.shape != want.shape:
        print("error: the game gave %s rows, the reference %s" % (got.shape, want.shape))
        return 1
    bad = np.flatnonzero((got != want).any(axis=1))
    print("%d rows. %d rows differ from the reference." % (len(want), len(bad)))
    for i in bad[:5]:
        print("  row %d: game %s, reference %s" % (i, got[i].tolist(), want[i].tolist()))
    return 1 if len(bad) else 0


def main():
    ap = argparse.ArgumentParser(description="Make and test an int8 pilot network.")
    ap.add_argument("header", help="a float header from train_pilot.py")
    ap.add_argument("--out", help="where to write the int8 header")
    ap.add_argument("--check", metavar="OBS",
                    help="compare the game's forward pass with this script's on a recording")
    ap.add_argument("--exe", help="the game to check, built with the int8 header")
    ap.add_argument("--rows", type=int, default=20000, help="rows to check (default 20000)")
    ap.add_argument("--blob", metavar="FILE",
                    help="write the int8 network as a file for the game's --net-file. "
                         "With --check, check the game's run of that file instead")
    args = ap.parse_args()
    if args.check:
        if not args.exe:
            print("error: --check needs --exe")
            return 1
        return check(args.header, args.check, args.exe, args.rows, args.blob)
    if args.blob:
        text, d, a = read_header(args.header)
        q = quantize(d, a)
        size = write_blob(args.blob, q, int(d.get("PILOT_MODE_N", 0)))
        print("wrote %s: %d bytes" % (args.blob, size))
        return 0
    if not args.out:
        print("error: give --out, --blob or --check")
        return 1
    text, d, a = read_header(args.header)
    if "PILOT_NET_INT8" in text:
        print("error: %s is already an int8 header" % args.header)
        return 1
    q = quantize(d, a)
    size = write(args.out, text, q)
    print("wrote %s: %d bytes of internal RAM" % (args.out, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
