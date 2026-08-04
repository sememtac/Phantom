#!/usr/bin/env python3
"""Read the telemetry from the board for N seconds and summarise it.

Usage:
    python tools/listen.py COM6 45

The board prints four lines every two seconds:
    1. the frame budget: fps, and the time in each phase
    2. the submit breakdown, per layer
    3. the frame-time distribution: p50, p95, and the worst frame
    4. audio delivery, the mixer peak, and the HUD split

The board prints none of them while a capture or a replay holds the link. Close
every other program that uses the port before you run this.

Read line 3 first. A mean cannot tell "0.5 ms short on every frame" from "8 ms
short once a second", and the two need opposite fixes.

LIMIT: the totals across windows are not reliable. The board resets its frame
counter and its histogram only when it prints a report, and it skips a report if
the serial buffer is full. The next report then includes the skipped frames. One
run reported 35637 frames and 625% blocked time. Use the per-window values.
"""
import re
import sys
import time

import serial

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

ser = serial.Serial(port, 115200, timeout=0.5)
end = time.time() + secs
rows, subs, dist, auds, raw = [], [], [], [], []
while time.time() < end:
    ln = ser.readline().decode("utf-8", "replace").strip()
    if not ln:
        continue
    raw.append(ln)
    if "fps |" in ln:
        rows.append(ln)
    elif "sub = star" in ln:
        subs.append(ln)
    elif " p50 " in ln:
        dist.append(ln)
    elif "aud = " in ln:
        auds.append(ln)
ser.close()

print("\n".join(raw[-15:]))


def avg(lines, keys):
    out = {}
    for k in keys:
        vals = [m.group(1) for m in
                (re.search(re.escape(k) + r"\s+(-?[\d.]+)", ln) for ln in lines)
                if m]
        if vals:
            out[k] = sum(float(v) for v in vals) / len(vals)
    return out


if not rows:
    sys.exit("\nno telemetry seen: is a capture or replay holding the link?")

fps = [m.group(1) for m in (re.match(r"([\d.]+) fps", r) for r in rows) if m]
print("\n--- means over %d reports ---" % len(rows))
if fps:
    m = sum(float(v) for v in fps) / len(fps)
    print("fps  %.1f  (%.2f ms/frame; 60 fps needs 16.67)" % (m, 1000.0 / m))

a = avg(rows, ["in", "upd", "sub", "blit", "rast", "sky", "prim", "scan"])
for k in ["in", "upd", "sub", "blit"]:
    if k in a:
        print("  %-5s %7.0f us" % (k, a[k]))
print("  (rast %.0f = sky %.0f prim %.0f scan %.0f -- all of it hides under DMA)"
      % (a.get("rast", 0), a.get("sky", 0), a.get("prim", 0), a.get("scan", 0)))

b = avg(subs, ["star", "arena", "world", "hud", "mir", "sfx", "sxr"])
if b:
    print("submit: " + "  ".join("%s %.0f" % (k, v) for k, v in b.items()))

# The distribution: which problem is this?
if not dist:
    sys.exit("\nno distribution line -- is this the p95 build?")

tot = late = 0
p50s, p95s = [], []
worst = (0, "")
for ln in dist:
    m = re.search(r"frames (\d+) \| p50 (\d+) p95 (\d+) late (\d+)", ln)
    if not m:
        continue
    tot += int(m.group(1))
    p50s.append(int(m.group(2)))
    p95s.append(int(m.group(3)))
    late += int(m.group(4))
    w = re.search(r"worst (\d+) us = (.*)$", ln)
    if w and int(w.group(1)) > worst[0]:
        worst = (int(w.group(1)), w.group(2))

print("\n--- distribution over %d frames, %d windows ---" % (tot, len(dist)))
if p50s:
    mode50 = max(set(p50s), key=p50s.count)
    print("p50    %5d us   %.1f fps   (most common window value)"
          % (mode50, 1e6 / mode50))
    print("p95    %5d us   %.1f fps   (worst window's 95th percentile)"
          % (max(p95s), 1e6 / max(p95s)))
if tot:
    print("late   %5d frames over 16667 us = %.1f%%" % (late, late * 100.0 / tot))
if worst[0]:
    print("worst  %5d us   %s" % worst)

# Audio delivery. Blocked time is the health signal and HIGH IS GOOD: it is the
# audio task sitting on a full DMA ring with nothing it needs to do.
if auds:
    blocked, short, wins = 0, 0, 0
    for ln in auds:
        m = re.search(r"blocked (\d+) ms/2s short (\d+)", ln)
        if not m:
            continue
        blocked += int(m.group(1))
        short += int(m.group(2))
        wins += 1
    if wins:
        pct = blocked * 100.0 / (wins * 2000.0)
        print("\n--- audio delivery over %d windows ---" % wins)
        print("blocked %5.1f%% of wall time waiting for DMA ring space" % pct)
        print("short   %5d samples refused even after the 50 ms wait" % short)
        if short:
            print("  -> the ring is not draining. Something is wrong with the codec,")
            print("     not with the pacing.")
        elif pct < 40.0:
            print("  -> THIN. The producer is barely keeping up, so the ring is not")
            print("     staying full and a stall has nothing to eat into.")
        else:
            print("  -> HEALTHY. The ring stays full, the codec sets the pace, and a")
            print("     stall on this core has the whole ring as slack.")

print()
print("HOW TO READ IT")
print("  p50 at or under 16667 and late% small  -> the steady state is fine and")
print("     the misses are EVENTS. Find the events; do not trim the budget.")
print("  p50 above 16667                        -> every frame is short. Trim the")
print("     pre-flush phase (in + upd + sub); blit is near its 11.52 ms DMA floor.")
print("  worst frame's split names the phase that owned the slowest frame.")
