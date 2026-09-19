#!/usr/bin/env python3
#
# Which component of the pedal under test is wrong?
#
# loop.py already drives an analog pedal and hands back a calibrated
# answer.  compare-spice.py already asks a netlist the same question the
# model is asked.  What neither does is say which PART of the circuit the
# disagreement lives in, and that is the only answer somebody holding a
# soldering iron can act on.
#
# So every single-component fault the netlist could have is simulated,
# and the one that best explains the measurement wins.  The ranking is
# netfault, which is a packaged fault dictionary - the method is old, and
# the survey is Bandler and Salama, Proc. IEEE 73 (1985).  This file is
# the part that is specific to this bench: the legs, the ladder, and the
# refusal to answer when nothing fits.
#
# WHY A LADDER AND NOT A SWEEP
#
# [RAT]'s clamp is a logarithm, so the pedal keeps getting louder after
# it starts clipping and the Distortion pot drags the closed-loop corner
# across most of the audio band.  One sweep at one level characterises
# almost none of that, and the part that sets where it folds does nothing
# at all until the drive reaches it.  So a signature here is one response
# per drive level, and a candidate is compared rung by rung.
#
import argparse
import os
import sys

try:
    import numpy as np
except ImportError:
    print("diagnose: SKIPPED - no numpy")
    sys.exit(0)

try:
    import netfault
except ImportError:
    print("diagnose: SKIPPED - netfault is not installed")
    print("          pip install git+https://github.com/quotentiroler/netfault@v0.2.1")
    sys.exit(0)

import loop
import ngspice
import pedal
import targets

HERE = os.path.dirname(os.path.abspath(__file__))

# Where the pots offer something, and coarse enough that a dictionary is
# minutes rather than an afternoon.
FREQS = [80.0, 160.0, 320.0, 640.0, 1250.0, 2500.0, 5000.0, 10000.0]

# The rungs.  Below the lowest the clamp does nothing and every candidate
# looks alike; above the highest the converter is what is being measured.
LEVELS = (-36.0, -24.0, -12.0)

SECONDS = 0.4


def spice_leg(t, knobs, seconds=SECONDS):
    """simulate(src, freqs, level) -> dB, through ngspice on a given deck.

    A transient rather than an ac sweep, because the circuit this exists
    for stops being linear the moment it is driven properly, and an ac
    sweep answers as though it never does.
    """
    params = t["spice"](knobs)

    def simulate(src, freqs, level=None):
        dbfs = -36.0 if level is None else level
        out = []
        for hz in freqs:
            x = loop.tone(hz, dbfs, int(seconds * loop.FS))
            y = ngspice.tran_src(src, t["node"], x, fs=loop.FS, params=params)
            out.append(loop.tone_db(y, hz) - loop.tone_db(x, hz))
        return np.asarray(out)

    return simulate


def loop_response(card, freqs=FREQS, report=print):
    """What the loop does to each frequency, pedal under test bypassed."""
    _delay, f, h = loop.calibrate(card)
    report("  loop measured, %.2f to %.2f dB across the band"
           % (20.0 * np.log10(np.abs(h)).min(), 20.0 * np.log10(np.abs(h)).max()))
    return np.interp(freqs, f, 20.0 * np.log10(np.abs(h)))


def measured_ladder(card, loop_db, freqs=FREQS, levels=LEVELS,
                    seconds=SECONDS, report=print):
    """The board on the bench, one gain per rung, the loop taken out."""
    rows = []
    for level in levels:
        row = []
        for hz, cal in zip(freqs, loop_db):
            sent, back = loop.measure_tone(card, hz, level, seconds=seconds,
                                           report=report)
            row.append(loop.tone_db(back, hz) - loop.tone_db(sent, hz) - cal)
        report("  %+.0f dBFS rung taken" % level)
        rows.append(row)
    return np.asarray(rows)


def numbers(text, what):
    """A comma separated sweep from the command line."""
    out = []
    for field in text.split(","):
        field = field.strip()
        if not field:
            continue
        try:
            out.append(float(field))
        except ValueError:
            sys.exit("diagnose: --%s has %r in it, which is not a number" % (what, field))
    if not out:
        sys.exit("diagnose: --%s is empty" % what)
    return out


def blameable(parts, exclude):
    """The parts a fault could be in, minus the ones named in --exclude.

    A name that matches nothing is refused rather than ignored: it leaves
    the part it was meant to protect in the dictionary, and says so only
    by naming it half an hour later.
    """
    skip = {r.strip() for r in exclude.split(",") if r.strip()}
    unknown = sorted(skip - set(parts))
    if unknown:
        sys.exit("diagnose: --exclude names %s, which the netlist does not "
                 "have.  It has: %s" % (", ".join(unknown), " ".join(sorted(parts))))
    refs = [r for r in sorted(parts) if r not in skip]
    if not refs:
        sys.exit("diagnose: nothing left to blame after --exclude")
    return refs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target", nargs="?", default="rat",
                    help="which target's netlist the board claims to be")
    ap.add_argument("--pedal", default=None,
                    help="serial, label or product substring naming one pedal")
    ap.add_argument("--exclude", default="",
                    help="parts that are not on the board and so cannot be "
                         "mis-fitted, comma separated")
    ap.add_argument("--freqs", default=",".join("%g" % f for f in FREQS),
                    help="where to measure, comma separated Hz")
    ap.add_argument("--levels", default=",".join("%g" % v for v in LEVELS),
                    help="the rungs, comma separated dBFS")
    ap.add_argument("--seconds", type=float, default=SECONDS,
                    help="per tone; these three set what the run costs")
    ap.add_argument("--top", type=int, default=5)
    args = ap.parse_args()
    freqs = numbers(args.freqs, "freqs")
    levels = numbers(args.levels, "levels")

    t = targets.target(args.target)
    src = ngspice.netlist(t["netlist"])
    parts = netfault.components(src)
    if not parts:
        sys.exit("diagnose: %s has no plain-valued R/C/L to blame" % t["netlist"])
    refs = blameable(parts, args.exclude)
    skip = set(parts) - set(refs)

    d, why = pedal.sole(args.pedal)
    if not d:
        print("diagnose: SKIPPED - %s" % why)
        return 0
    print("diagnose: %s, %s" % (d["label"], pedal.use_map(d)))
    print("          %s against %s, %d parts, %d rungs, %d points"
          % (args.target, t["netlist"], len(refs), len(levels), len(freqs)))
    if skip:
        print("          not on the board: %s" % ", ".join(sorted(skip)))

    loop.refuse_if_stale(d["port"])
    loop.configure(d["port"], "hardware", t)

    print("\n  put the pedal under test in bypass, then Enter: ", end="")
    input()
    loop_db = loop_response(d["card"], freqs)
    print("  engage it again, then Enter: ", end="")
    input()

    print("\n  measuring the board")
    measured = measured_ladder(d["card"], loop_db, freqs, levels, args.seconds)

    print("  building the dictionary (%d simulations)"
          % ((len(refs) * len(netfault.FACTORS) + 1) * len(levels) * len(freqs)))
    sim = spice_leg(t, t["knobs"], args.seconds)
    cands = netfault.candidates(src, freqs, sim, refs, netfault.FACTORS,
                                levels=levels)

    # Both sides are gains, so aligning would throw away the level a
    # fault most often moves.
    noise = netfault.FLOOR_DB
    usable = netfault.resolvable(cands, noise, align=False)
    dropped = len(cands) - len(usable)
    if dropped:
        print("  %d of %d candidates are below this bench's resolution"
              % (dropped, len(cands) - 1))

    v = netfault.explain(usable, measured, noise_db=noise, align=False)

    print("\n  %-10s %-8s %s" % ("part", "factor", "residual dB"))
    for resid, ref, factor in v["ranked"][:args.top]:
        print("  %-10s %-8s %.4f"
              % (ref or "(nominal)", "-" if ref is None else netfault.describe(factor),
                 resid))

    print()
    if v["verdict"] == "unexplained":
        print("  NOTHING HERE EXPLAINS THIS (%.4f dB residual)." % v["residual"])
        print("  The netlist does not describe this board.  That is the thing")
        print("  to fix before any part above is believed.")
    elif v["verdict"] == "nominal":
        print("  the board matches %s (%.4f dB)" % (t["netlist"], v["residual"]))
    elif v["verdict"] == "ambiguous":
        nxt = v["ranked"][1]
        print("  AMBIGUOUS: %s is only %.4f dB better than %s."
              % (v["ref"] or "nominal", v["margin"], nxt[1] or "nominal"))
    else:
        print("  %s at %s (%.3g -> %.3g)"
              % (v["ref"], netfault.describe(v["factor"]),
                 parts[v["ref"]]["value"], parts[v["ref"]]["value"] * v["factor"]))
        print("  %.4f dB residual, %.4f dB clear of the next answer"
              % (v["residual"], v["margin"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
