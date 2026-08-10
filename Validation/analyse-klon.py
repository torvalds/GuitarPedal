#!/usr/bin/env python3
#
# Measure [KLON], and print the numbers that go in its page.
#
# Documentation/effects/klon.md is written by hand around this output -
# the prose is the valuable half and a generator cannot write it - so
# this exists to make re-measuring a command rather than an afternoon.
# The last analysis was done once, by hand, outside the tree, and went
# quietly wrong the moment single_pole_freq() was fixed underneath it.
#
# Everything printed here is deterministic: same bench binary, same
# input, same bytes out.  That is what lets check-analysis.py compare
# the page against a fresh run exactly rather than with a tolerance.
#
# Run 'make bench' first.  A stale bench measures a pedal you no longer
# have, convincingly - see the issue list.
#
import sys
import math

sys.path.insert(0, ".")
import bench as B

KLON = "Klonlike"

#
# Third-octave centres from 20Hz, which is what the mermaid x-axis in the
# page wants: it has no logarithmic mode, so log-spaced labels are how a
# log axis gets drawn.  Trimmed to whole cycles in the analysis window by
# B.tone(), so these are the nominal names and bin_of() picks the bin.
#
def octaves(lo, hi, per_octave=1):
    out, f = [], float(lo)
    while f <= hi * 1.0001:
        out.append(round(f / B.FS * B.WINDOW) * B.FS / B.WINDOW)
        f *= 2 ** (1.0 / per_octave)
    return out


def pots(gain, treble, output):
    """[KLON]'s three pots are all LINEAR(0 1), so 0..120 maps directly."""
    return ["--pot", f"{KLON}:Gain={round(gain * 120)}",
            "--pot", f"{KLON}:Treble={round(treble * 120)}",
            "--pot", f"{KLON}:Output={round(output * 120)}"]


def routed(gain, treble=0.5, output=0.4):
    return (["--pot", "Signal Chain:Gate=0", "--route", KLON]
            + pots(gain, treble, output))


def response(gain, treble, freqs, dbfs=-40.0):
    """Small signal, so the clipper is not what is being measured."""
    out = []
    for f in freqs:
        m = B.measure(routed(gain, treble), f0=f, dbfs=dbfs)
        out.append(round(float(m["gain_db"]), 2))
    return out


def main():
    print("# measured by analyse-klon.py - paste into Documentation/effects/klon.md\n")

    #
    # 1. What the gain knob does.  THD and output against Gain, at a
    #    level a guitar actually produces.
    #
    print("## gain sweep, -18 dBFS in, Treble 0.5, Output 0.4\n")
    print(f"{'Gain':>6} {'out dB':>9} {'THD dB':>9} {'alias dB':>9} {'even %':>8}")
    gains = [i / 10.0 for i in range(11)]
    for g in gains:
        m = B.measure(routed(g), f0=B.MID_HZ, dbfs=-18.0)
        h = B.harmonics(m["_left"], m["f0"])
        even = math.sqrt(sum(v * v for v in h[1::2]))
        odd = math.sqrt(sum(v * v for v in h[0::2]))
        pct = 100.0 * even / (odd + 1e-30)
        print(f"{g:6.1f} {m['gain_db']:9.3f} {m['thd_db']:9.1f} "
              f"{m['alias_db']:9.1f} {pct:8.2f}")

    #
    # 2. The treble control, at both ends, small signal.
    #
    freqs = octaves(20, 20480, per_octave=1)
    print("\n## small-signal response, Gain 0.5, Treble 0.0 and 1.0\n")
    print("x-axis " + str([int(round(f)) for f in freqs]))
    for t in (0.0, 1.0):
        r = response(0.5, t, freqs)
        print(f"Treble {t:.1f} " + str([round(v, 2) for v in r]))

    #
    # 3. The thing 147 is about: the clean path skips the input filters,
    #    so the response depends on the gain knob.  Measured at both ends.
    #
    # Normalised to 640Hz, because the point is the *shape* against the
    # gain knob and the two settings are 30dB apart in level.  Done here
    # rather than by hand into the page: a number worked out in someone's
    # head is exactly the kind that goes stale unnoticed.
    print("\n## small-signal response, Treble 0.5, Gain 0.0 and 1.0\n")
    print("x-axis " + str([int(round(f)) for f in freqs]))
    mid = freqs.index(next(f for f in freqs if abs(f - 640) < 40))
    for g in (0.0, 1.0):
        r = response(g, 0.5, freqs)
        print(f"Gain {g:.1f} " + str(r))
        print(f"Gain {g:.1f} rel 640Hz " +
              str([round(v - r[mid], 2) for v in r]))

    #
    # 4. Where it stops being clean, as the input gets louder.
    #
    print("\n## level sweep at Gain 0.7\n")
    print(f"{'in dBFS':>9} {'out dB':>9} {'THD dB':>9} {'corner':>8}")
    for m in B.level_sweep(routed(0.7), f0=B.MID_HZ):
        print(f"{m['dbfs']:9.1f} {m['gain_db']:9.3f} {m['thd_db']:9.1f} "
              f"{m['corner_sharpness']:8.2f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
