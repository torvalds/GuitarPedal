#!/usr/bin/env python3
#
# [RAT]'s clipping node, solved on the host at build time.
#
# The node is one equation - the drive divides between R5 and a pair of
# diodes, and the diodes' share is a logarithm of their own voltage:
#
#	drive = v + I(v)*(Rs + R5),   I(v) = IREF*exp((v - v1)/vt) - Is
#
# It has no closed form, so solving it at all means an upper bound and
# an iteration - several transcendentals a sample, for a curve that
# depends on nothing but the Mode switch and cannot move while a note is
# playing.  Paying that per sample would be paying it a few million
# times for an answer that changes when somebody turns a switch.
#
# So it is solved here, at every point of a table, with as many
# bisections as it takes and no budget to keep to.  What the audio core
# does is an index and a lerp.
#
# WHY THE CONSTANTS COME OUT OF rat.h AND NOT OUT OF THE NETLIST
#
# The usual rule is that a generated header reads its component values
# out of the netlist ngspice checks, so the thing being verified and the
# thing being run cannot drift apart.  That is the right rule and this is
# the exception to it, for a reason worth stating rather than working
# around silently: rat-helios.cir's diodes are a generic 1N914 with N=1.752 and
# no series resistance at all, and these were fitted to the pedal on the
# bench - N=1.859, Is=2.03nA, Rs=31 ohm - which is a better answer than
# the deck has.  The two are *meant* to differ, and the difference is
# itself a finding.  So rat.h is the authority here and this file is
# downstream of it, the way effect_map.h is downstream of the POT lines.
#
# WHY A UNIFORM TABLE IS ENOUGH, AND HOW BIG IT HAS TO BE
#
# The curve looks like it has a corner in it and does not.  Below the
# knee the node follows the drive exactly; above it the node grows as a
# logarithm; and the join between them is spread over several thermal
# voltages.  Its slope falls from 1.000 to 0.014 across the range, all
# of it smoothly, so linear interpolation does well and keeps doing well
# as the table shrinks - 12 dB per halving, with no sign of the knee
# being missed even where a step is ten times a thermal voltage.
#
# So the size is not set by the null on real playing, which was already
# inaudible at the smallest size tried, nor by the harmonics, which do
# not move at all between eight entries and 256.  It is set by the
# transfer curve itself, measured at Distortion minimum where the input
# drives the diodes through R5 alone:
#
#       entries      32      64     128     256
#       worst     4.055   0.743   0.223   0.064 mV
#
# 128 is kept, and the line it has to be under is not a preference.
# The constants above were fitted to the pedal with a residual of
# 0.62 mV rms (ISSUES 346).  A table coarser than that makes the
# interpolation the dominant error in a curve that was measured to
# better; a table finer than it is modelling noise.  64 is just the
# wrong side of that line and 128 is comfortably the right side.
#
# For the record, since it is the obvious question: a rational
# approximant does not win here.  Fitting P(m)/Q(m) over the same range
# needs degree 6 over 6 - thirteen coefficients - to reach 0.33 mV,
# which is about what 128 entries give.  That saves 1.4 kB and costs a
# divide plus a dozen multiply-adds where this costs a load pair and a
# lerp, on an effect where cycles were the thing being bought.  Fitting
# the deficit m - v instead of the node does not help.  And the fits are
# badly conditioned - degree 5 came out a thousand times worse than
# degrees 4 and 6 - which is not a thing to put in an audio path.
#
import argparse
import math
import re
import sys
from pathlib import Path

# What the drive can be.  The op-amp cannot leave +3.045/-2.545, and the
# charge on C7 has been measured swinging over a volt, so four is past
# anything reachable rather than a guess about typical.
DMAX = 4.0

WANT = ("RAT_D_N", "RAT_D_IS", "RAT_D_RS", "RAT_LED_N", "RAT_LED_IS",
        "RAT_LED_RS", "RAT_VT", "RAT_IREF", "RAT_R5")


def constants(path):
    """The diode law's numbers, read out of the effect that documents them."""
    text = Path(path).read_text()
    out = {}
    for name in WANT:
        m = re.search(r"^#define\s+%s\s+([-+0-9.eE]+)f?\s*(?:/\*|$)"
                      % name, text, re.M)
        if not m:
            sys.exit("rat_clamp: %s does not define %s" % (path, name))
        out[name] = float(m.group(1))
    return out


def node(drive, vt, v1, isat, rtot):
    """Solve the node by bisection.  Slow, exact, and run at build time."""
    if drive <= 0.0:
        return 0.0
    lo, hi = 0.0, drive
    for _ in range(200):
        v = 0.5 * (lo + hi)
        try:
            i = math.exp((v - v1) / vt) * 1e-3 - isat
        except OverflowError:
            i = float("inf")
        if v + i * rtot < drive:
            lo = v
        else:
            hi = v
    return 0.5 * (lo + hi)


def table(f, name, n, series, N, Is, Rs, c):
    vt = series * N * c["RAT_VT"]
    v1 = vt * math.log(c["RAT_IREF"] / Is)
    rtot = Rs + c["RAT_R5"]
    f.write("\n/* %s: vt %.6f V, v1 %.6f V, Rs+R5 %.1f ohm */\n"
            % (name, vt, v1, rtot))
    f.write("const float __not_in_flash(\"audio\") %s[] = {" % name)
    for i in range(n + 1):
        v = node(DMAX * i / n, vt, v1, Is, rtot)
        f.write("%s%+.8ef," % ("\n\t" if i % 4 == 0 else " ", v))
    f.write("\n};\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("header", help="Effects/rat.h, which owns the constants")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("-n", "--entries", type=int, default=128)
    args = ap.parse_args()

    c = constants(args.header)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w") as f:
        f.write("/* Generated by scripts/rat_clamp.py - do not edit. */\n")
        f.write("#define RAT_CLAMP_N\t%d\n" % args.entries)
        f.write("#define RAT_CLAMP_DMAX\t%.1ff\n" % DMAX)
        f.write("#define RAT_CLAMP_STEP\t%.8ef\n" % (args.entries / DMAX))
        table(f, "rat_clamp_si", args.entries, 1, c["RAT_D_N"],
              c["RAT_D_IS"], c["RAT_D_RS"], c)
        table(f, "rat_clamp_stack", args.entries, 2, c["RAT_D_N"],
              c["RAT_D_IS"], c["RAT_D_RS"], c)
        table(f, "rat_clamp_led", args.entries, 1, c["RAT_LED_N"],
              c["RAT_LED_IS"], c["RAT_LED_RS"], c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
