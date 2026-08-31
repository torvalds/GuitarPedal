#!/usr/bin/env python3
#
# What is plugged into the expression jack, read straight off the ADC.
#
#   ./exp-probe.py              one reading
#   ./exp-probe.py --watch      keep reading until interrupted
#
# Bringup only.  The firmware side is exp.h, reached by SysEx 0x0e, and
# neither end is wired into anything the pedal does - this exists to find
# out whether the jack is worth building on, on a board where it has
# never been used.
#
# WHAT TO EXPECT.  Both normalling contacts are grounded, so an empty
# jack reads near zero everywhere and that is correct rather than broken.
# The interesting rows are the ones that need something plugged in:
#
#   nothing               all near 0, because the jack grounds itself
#   TRS, nothing pressed  pull-up rows near 4095
#   TRS, tip to sleeve    pull-up tip near 0, ring still high
#   expression pedal      one of the driven rows sweeping with the treadle
#
# The temperature row is not about the jack at all.  It is the one input
# whose answer is known in advance, so a board reading zero everywhere
# can be told from a board whose ADC is not converting.
#
import argparse
import re
import subprocess
import sys
import time

import pedal

# Must match enum in exp.h - order is the wire format.
NAMES = [
    ("float  ring", "hi-Z, no pull"),
    ("float  tip", "hi-Z, no pull"),
    ("pullup ring", "footswitch: low = shorted to sleeve"),
    ("pullup tip", "footswitch: low = shorted to sleeve"),
    ("drive tip -> read ring", "expression, tip = supply"),
    ("drive ring -> read tip", "expression, ring = supply"),
    ("temperature", "ADC self-check, not the jack"),
]

# Must match enum exp_accessory in exp.h.
ACCESSORY = ["nothing", "footswitches", "expression pedal", "something unrecognised"]

FULL_SCALE = 4095
VREF = 3.3


def volts(raw):
    return raw * VREF / FULL_SCALE


def temp_c(raw):
    # RP2350 datasheet: T = 27 - (V - 0.706) / 0.001721
    return 27.0 - (volts(raw) - 0.706) / 0.001721


def probe(port, wait=1.5):
    """Ask for one sweep and decode the reply, or None."""
    dump = subprocess.Popen(["aseqdump", "-p", port],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True)
    try:
        time.sleep(0.4)
        pedal.send(port, 0x0E)
        time.sleep(wait)
    finally:
        dump.terminate()
        text = dump.stdout.read()
        dump.wait()

    blob = bytes.fromhex("".join(
        re.findall(r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)", text)
    ).replace(" ", ""))

    i = blob.find(bytes([0xF0, 0x7D, 0x0E]))
    if i < 0:
        return None
    body = blob[i + 3:]
    end = body.find(0xF7)
    if end < 0:
        return None
    body = body[:end]

    if not body or body[0] < 1:
        return None

    # Append-only: the readings, then whatever later firmware added.
    pairs = body[1:1 + 2 * len(NAMES)]
    vals = [(pairs[j] << 7) | pairs[j + 1] for j in range(0, len(pairs) - 1, 2)]
    tail = body[1 + 2 * len(NAMES):]
    return vals, tail


def name_of(v):
    return ACCESSORY[v] if v < len(ACCESSORY) else f"? ({v})"


def show(vals, tail):
    for (name, note), raw in zip(NAMES, vals):
        extra = f"  ({temp_c(raw):.1f} C)" if name == "temperature" else ""
        bar = "#" * round(24 * raw / FULL_SCALE)
        print(f"  {name:24} {raw:5}  {volts(raw):5.3f} V  {bar:<24}{extra}  {note}")
    if len(tail) >= 1:
        print(f"\n  probe says:   {name_of(tail[0])}")
    if len(tail) >= 2:
        print(f"  set to:       {name_of(tail[1])}")
        if tail[0] != tail[1]:
            print("  they disagree - the setting is what the pedal runs from")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--watch", action="store_true",
                    help="keep probing; move the treadle and watch it track")
    ap.add_argument("--interval", type=float, default=0.0,
                    help="extra seconds between sweeps in --watch")
    args = ap.parse_args()

    found = pedal.discover()
    if not found:
        print("exp-probe: SKIPPED - no pedal found")
        return 0
    d = found[0]
    print(f"exp-probe: {d['label']} (midi {d['port']})")

    if not args.watch:
        got = probe(d["port"])
        if got is None:
            print("  no reply - is this firmware built with the probe in it?")
            return 1
        print()
        show(*got)
        return 0

    #
    # Only the two driven rows move with a treadle, so the watch view is
    # those plus whichever pull-up row is doing something.  Printed as a
    # line each time rather than redrawn, because what matters when you
    # sweep a pedal is the range and the steps, and a scrolling log keeps
    # both where you can see them.
    #
    print("  drive-tip->ring   drive-ring->tip   pullup ring/tip     (^C to stop)")
    lo = [FULL_SCALE] * 2
    hi = [0] * 2
    try:
        while True:
            got = probe(d["port"], wait=0.35)
            if got is None:
                print("  (no reply)")
                continue
            vals = got[0]
            a, b = vals[4], vals[5]
            lo = [min(lo[0], a), min(lo[1], b)]
            hi = [max(hi[0], a), max(hi[1], b)]
            print(f"  {a:5} ({volts(a):5.3f}V)     {b:5} ({volts(b):5.3f}V)"
                  f"     {vals[2]:5} {vals[3]:5}"
                  f"     span {hi[0]-lo[0]:5} {hi[1]-lo[1]:5}")
            if args.interval:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print()
        for i, (name, _) in enumerate(NAMES[4:6]):
            span = hi[i] - lo[i]
            levels = span / 32.0    # 4096/128, one pot step
            print(f"  {name:24} {lo[i]:5} .. {hi[i]:5}  span {span:5}"
                  f"  = {levels:5.1f} pot steps of 128")
    return 0


if __name__ == "__main__":
    sys.exit(main())
