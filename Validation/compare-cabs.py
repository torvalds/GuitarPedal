#!/usr/bin/env python3
#
# Listening to [CAB], for ears rather than for the FFT.
#
# One source through every cabinet row, or one row at five Drive
# settings.  shootout.py does the level matching and the page; what is
# here is the routing and the source - which rows exist, what goes in
# front of them, and what the passage actually contains.
#
# The source defaults to Inputs/BassForLinus.mp3, which is a DI
# recording - no cab on it already - so it is a fair thing to put a cab
# sim in front of.  It is also a bass, which is not what most of this is
# voiced for; pass --source for something better.
#
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio
import bench as B
import effectmap
import pots as P
import shootout as S

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = B.quiet()


def rows():
    """The cabinets this build has, in the order the enum declares them."""
    return effectmap.pot_info("Cabinet", "Cabinet")["enum"]


def cab(row, **over):
    """One cabinet row at its defaults, except for what is named."""
    a = B.route("CAB", 120) + P.arg("CAB", "Cabinet", row)
    for k, val in over.items():
        a += P.arg("Cabinet", k, val)
    return GATE + a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--offset", default="auto",
                    help="where in the recording to start, in seconds; "
                         "'auto' skips the quiet lead-in")
    ap.add_argument("--peak", type=float, default=0.35,
                    help="input peak, in the pedal's own scale")
    ap.add_argument("--out", default="cab-shootout",
                    help="prefix for the wavs and the page")
    ap.add_argument("--blind", action="store_true",
                    help="shuffle the labels and print the key at the end")
    ap.add_argument("--source", default=os.path.join(HERE, "Inputs",
                                                     "BassForLinus.mp3"))
    ap.add_argument("--pre", metavar="EFFECT", default=None,
                    help="route a drive in front of the cab - a cab sim's "
                         "stated job is taming the fizz off a distorted amp, "
                         "and a clean signal never asks it to")
    ap.add_argument("--pre-pot", action="append", default=[],
                    metavar="POT=VAL", help="a raw 0..120 pot on --pre")
    ap.add_argument("--trim", type=float, default=None,
                    help="[CHAIN] Trim in dB, for a guitar that is not hot")
    ap.add_argument("--row", default=None, choices=rows(),
                    help="with --ladder, which cabinet to walk Drive on")
    ap.add_argument("--drive", type=float, default=15.0,
                    help="Drive for the row comparison, in dB")
    ap.add_argument("--ladder", action="store_true",
                    help="one row at five Drive settings, which is where "
                         "it stops being a filter and starts breaking up")
    args = ap.parse_args()

    #
    # The drive goes in front of every take including the dry one, so
    # that what is being compared is still only the cab.
    #
    if args.trim is not None:
        GATE.extend(P.arg("Signal Chain", "Trim", args.trim))

    pre = []
    if args.pre:
        pre = ["--route", args.pre, "--mix", f"{args.pre}=120"]
        for kv in args.pre_pot:
            pre += ["--pot", f"{args.pre}:{kv}"]

    off = None if args.offset == "auto" else float(args.offset)
    dry, off = audio.decode(args.source, args.seconds, off)
    dry = dry / max(np.abs(dry).max(), 1e-9) * args.peak
    print(f"source: {os.path.basename(args.source)} "
          f"{off:.1f}..{off + len(dry) / audio.RATE:.1f}s"
          f"{' (found)' if args.offset == 'auto' else ''}, "
          f"peak {args.peak}, {len(dry)} samples")

    #
    # What is actually in the source, per octave, relative to its
    # loudest octave.  A cab sim's differences above 5kHz do not matter
    # on material that has nothing there, and which of those is true is
    # a measurement rather than an opinion.
    #
    spec = np.abs(np.fft.rfft(dry * np.hanning(len(dry)))) ** 2
    freq = np.fft.rfftfreq(len(dry), 1.0 / audio.RATE)
    edges = [20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480]
    band = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (freq >= lo) & (freq < hi)
        band.append(10 * np.log10(spec[m].sum() + 1e-30))
    top = max(band)
    print("source content, dB re its loudest octave:")
    print("  " + " ".join(f"{lo:>7}" for lo in edges[:-1]))
    print("  " + " ".join(f"{v - top:7.1f}" for v in band))

    if args.ladder:
        row = args.row or "Modern-4x12"
        setups = [("nocab", "no cab at all", GATE + pre)]
        for db in (0.0, 10.0, 18.0, 24.0, 30.0):
            setups.append((f"drive{int(db)}", f"{row}, Drive {db:+.0f} dB",
                           GATE + pre + cab(row, Drive=db)[len(GATE):]))
    else:
        #
        # Every row at the same Drive.  They are all at the same
        # small-signal level by construction - norm() divides the
        # sensitivity out - so what is left is the voicing and how early
        # each one lets go, which is the whole of what a row is.
        #
        setups = [("nocab", "no cab at all", GATE + pre)]
        for row in rows():
            setups.append((row.lower(), f"{row}, Drive {args.drive:+.0f} dB",
                           GATE + pre + cab(row, Drive=args.drive)[len(GATE):]))

    takes, clipped = [], {}
    for key, name, argv in setups:
        y, _, info = B.run(argv, dry.astype(np.float32), warmup=B.settle())
        takes.append((key, name, np.asarray(y, dtype=np.float64)[-len(dry):]))
        n = info.get("clipped", 0)
        clipped[key] = "" if not n else f"{n:.0f} clipped"

    S.publish(takes, args.out,
              title="Cabinet - one source, several speakers",
              ref=S.rms(dry), blind=args.blind, extra=clipped)


if __name__ == "__main__":
    main()
