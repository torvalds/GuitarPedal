#!/usr/bin/env python3
#
# Everything wanted from the pedal at one setting, in one visit.
#
# The knobs on an analog pedal are physical, so a bench session is a
# sequence of things somebody has to do by hand, and the expensive part
# is not the measuring - it is the walking back and forth.  So this
# takes the lot at whatever the pedal is currently set to: the
# small-signal response, the level ladder with its harmonics, and the
# waveforms worth drawing.
#
# It writes one JSON holding all of it, and the pot positions it was
# taken at, because a number without its conditions cannot be
# reproduced and this whole file exists because of one that could not
# be - see ISSUES.md 335.
#
# Not committed to the repository: the captures are a few megabytes of
# samples each and they belong to one pedal on one evening.  What is
# committed is this, so that another one can be made.
#
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import audio
import loop
import pedal


def main():
    ap = argparse.ArgumentParser(
        description="Capture one Helios setting: sweep, ladder and waveforms.")
    ap.add_argument("out", help="where to write the JSON")
    ap.add_argument("--setting", default="Distortion noon, Filter noon, "
                                         "Sweep min, Level max, one silicon",
                    help="what the pedal is set to - recorded verbatim, "
                         "because nothing else can know")
    ap.add_argument("--guitar", default=None,
                    help="a recording to push through as well")
    ap.add_argument("--level", type=float, default=-57.0,
                    help="dBFS peak for the small-signal sweep; it has to be "
                         "low enough that nothing clips, which at full "
                         "Distortion is lower than you would guess")
    ap.add_argument("--ladder", default="-66:0:6", metavar="FROM:TO:STEP",
                    help="the level ladder in dBFS.  The default walks the "
                         "whole range in 6 dB steps, which is right when the "
                         "gain puts the whole ladder into the clamp.  At "
                         "unity gain the input drives the diodes directly and "
                         "all the bending is in the top dozen decibels, so "
                         "that wants a finer step over a shorter range")
    ap.add_argument("--waves-hz", default="220", metavar="A,B,C",
                    help="frequencies to capture burst waveforms at.  The "
                         "default is the 220 Hz everything else here uses.  "
                         "More than one is how C7's effective value gets "
                         "measured: the charge it takes per half cycle scales "
                         "with the period, so the droop across a clipped "
                         "plateau does too, and an effective capacitance that "
                         "falls with frequency is an electrolytic rather than "
                         "a value that is simply wrong")
    args = ap.parse_args()

    try:
        waves_hz = [float(v) for v in args.waves_hz.split(",")]
        if not waves_hz or any(f <= 0 for f in waves_hz):
            raise ValueError
    except ValueError:
        raise SystemExit("capture-rat: --waves-hz wants a comma-separated "
                         "list of frequencies, e.g. 82,110,165,220,330,440")

    try:
        lo, hi, step = (float(v) for v in args.ladder.split(":"))
        if step <= 0 or hi < lo:
            raise ValueError
    except ValueError:
        raise SystemExit("capture-rat: --ladder wants FROM:TO:STEP in dBFS, "
                         "low to high, e.g. -24:0:1")
    ladder = [lo + i * step for i in range(int((hi - lo) / step) + 1)]

    p = pedal.port()
    card = audio.find_card("Pedal")
    print("firmware:", loop.refuse_if_stale(p))
    loop.configure(p, "hardware")
    store = {"setting": args.setting, "firmware": pedal.elf_build()}

    print("\n[1] small signal at %+.0f dBFS" % args.level)
    hz = [40, 60, 110, 220, 440, 880, 1250, 1760, 2500, 3520, 5000, 7000,
          10000, 12000, 14000, 16000]
    gain = []
    for f in hz:
        sent, back = loop.measure_tone(card, f, args.level, report=print)
        gain.append(loop.tone_db(back, f) - loop.tone_db(sent, f))
        print("   %6d Hz  %+7.2f dB" % (f, gain[-1]))
    store["ac"] = {"hz": hz, "gain_db": gain, "level_dbfs": args.level}

    print("\n[2] level ladder at 220 Hz, %g to %g in %g dB steps"
          % (ladder[0], ladder[-1], step))
    rungs = []
    for d in ladder:
        sent, back = loop.measure_tone(card, 220.0, d, report=print)
        f0 = loop.tone_db(back, 220.0)
        h = [loop.tone_db(back, 220.0 * k) - f0 for k in (2, 3, 4, 5)]
        rungs.append(dict(dbfs=d, out_db=f0, peak=float(np.abs(back).max()),
                          rms=float(np.sqrt(np.mean(back ** 2))), h=h))
        print("   in %+6.1f  out %+7.2f  H2 %+6.1f  H3 %+6.1f"
              % (d, f0, h[0], h[1]))

    store["ladder"] = rungs

    print("\n[3] waveforms")
    waves = {}
    for f in waves_hz:
        for dbfs in (-36, -24, -12):
            x = loop.tone(f, float(dbfs), int(0.25 * loop.FS))
            sent, back = loop.play_capture(card, x)
            #
            # 220 Hz keeps the name it has always had, so a capture
            # taken with the default reads back exactly like every one
            # taken before this option existed.
            #
            key = ("burst%d" % dbfs if f == 220.0
                   else "burst%d@%g" % (dbfs, f))
            waves[key] = dict(sent=sent.tolist(), back=back.tolist(), hz=f)
            print("   %-14s peak in %.4f  out %.4f"
                  % (key, np.abs(sent).max(), np.abs(back).max()))

    if args.guitar:
        gx, _ = audio.decode(args.guitar, seconds=1.0, offset=12.0)
        gx = gx / max(np.abs(gx).max(), 1e-9) * 0.35
        sent, back = loop.play_capture(card, gx.astype(float))
        waves["guitar"] = dict(sent=sent.tolist(), back=back.tolist(),
                               source=os.path.basename(args.guitar))
        print("   guitar    peak in %.4f  out %.4f"
              % (np.abs(sent).max(), np.abs(back).max()))
    store["waves"] = waves

    json.dump(store, open(args.out, "w"))
    print("\nwrote", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
