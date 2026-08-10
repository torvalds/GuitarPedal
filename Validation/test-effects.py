#!/usr/bin/env python3
#
# Whether the effect bench is working, which is not the same question as
# whether the effects are.
#
# What an effect does to a signal is a conversation - see
# Documentation/effect-analysis - and almost none of it has a right
# answer that can be asserted.  What does have a right answer is the
# instrument: a transparent chain has to come out transparent, and a
# waveshaper that is deliberately sharp has to read as sharp.  Those two
# are the whole of this file.
#
# A bench that fails either makes every number taken with it worthless,
# which is why this is in 'check' and the rest of it is not.
#
# Skips rather than fails without numpy, the same bargain the node check
# in the Makefile makes: this is the only thing in Validation/ that needs
# more than a C compiler and a python, and 'make check' should still be
# worth running without it.
#
import sys

try:
    import numpy as np
except ImportError:
    print("test-effects: SKIPPED - no numpy")
    sys.exit(0)

import bench as B

FAILED = []


def check(name, got, lo=None, hi=None, unit=""):
    ok = True
    if lo is not None and got < lo:
        ok = False
    if hi is not None and got > hi:
        ok = False
    want = "%s..%s" % ("" if lo is None else "%g" % lo,
                       "" if hi is None else "%g" % hi)
    print("  %-44s %9.3f %-4s  want %-14s %s"
          % (name, got, unit, want, "ok" if ok else "FAIL"))
    if not ok:
        FAILED.append(name)


#
# The negative control.  [CHAIN] with the gate off and both gains at 0dB
# is a wire, and every number here is the bench measuring itself.
#
# The gain is allowed to be a whisker under unity rather than exactly
# unity: the pedal's slewed gains converge to within eps(1.0)/2 * 512 of
# their target and stop there, which is -0.000265 dB.  That is the
# firmware's arithmetic and not an error in the measurement, so the
# bound admits it and nothing more.
#
print("negative control - [CHAIN] at unity, gate off")
for dbfs in (-40.0, -18.0, -6.0):
    m = B.measure(["--pot", "Signal Chain:Gate=0"], dbfs=dbfs)
    tag = "%.0f dBFS" % dbfs
    check("transparent gain (%s)" % tag, m["gain_db"], -0.001, 0.001, "dB")
    check("transparent thd (%s)" % tag, m["thd_db"], None, -120.0, "dB")
    check("transparent alias (%s)" % tag, m["alias_db"], None, -120.0, "dB")
    #
    # sqrt(2) is the crest factor of a sine, so that is what the second
    # difference of an undistorted one comes out at.  Anything with a
    # bend in it reads higher.
    #
    check("no corner (%s)" % tag, m["corner_sharpness"], 1.35, 1.50)
    check("no clipping (%s)" % tag, m["clipped"], None, 0.0)
    check("no dropped samples (%s)" % tag, m["dropped"], None, 0.0)

#
# The positive control.  boost.h's fold() reflects everything past the
# level about the level, so the transfer curve reverses slope there.
# That is a slope discontinuity on purpose, and an instrument that
# cannot see it cannot see any of the accidental ones either.
#
print()
print("positive control - [BOOST] folding on purpose")
folding = ["--pot", "Signal Chain:Gate=0", "--route", "Boost",
           "--mix", "Boost=120",
           "--pot", "Boost:Boost=90", "--pot", "Boost:Level=40",
           "--pot", "Boost:Basscut=120", "--pot", "Boost:Highcut=120"]

clean = B.measure(folding, dbfs=-60.0)
check("below the fold, no corner", clean["corner_sharpness"], 1.35, 3.0)

for dbfs in (-24.0, -12.0):
    m = B.measure(folding, dbfs=dbfs)
    tag = "%.0f dBFS" % dbfs
    check("folding reads as a corner (%s)" % tag, m["corner_sharpness"], 8.0, None)
    check("folding aliases audibly (%s)" % tag, m["alias_db"], -30.0, None, "dB")

print()
if FAILED:
    print("test-effects: FAILED - %s" % ", ".join(FAILED))
    sys.exit(1)
print("test-effects: ok")
