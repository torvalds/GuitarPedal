#!/usr/bin/env python3
#
# The Pitch effect's output level on a sustained note.
#
# The two taps read the same delay line 2048*|step| samples apart, so
# what they carry is correlated, and the sin/cos crossfade - which
# keeps the two tap *powers* adding to one - then modulates the power
# of the sum at the crossfade rate instead.  On a sustained note that
# is a tremolo at SAMPLES_PER_SEC/DISCONT_STEPS, with a depth of up to
# the full correlation between the taps: notes whose period divides
# the tap spacing swung from near-silence to twice their power and
# back, twice a window, forever.
#
# What this asserts is the thing the bug is: how far the output
# envelope of a sustained note still moves while it plays.  The
# frequencies are whole cycles in the analysis window, like everywhere
# else in here, and they are picked for the mechanism: 164Hz is
# exactly four tap spacings away at the default octave, so the taps
# are fully correlated and the old crossfade dug a hole to silence
# twice a window; 84Hz lands near the opposite correlation phase and
# did nearly as badly from the other sign.  128Hz at the octave-0 pot
# is the degenerate bottom of it, where both taps read the same sample
# and the weights cancel the signal outright.
#
# Bounds sit well above what the correction leaves behind (a couple of
# tenths of a dB to ~2.5dB across the pot ranges) and far below what
# the uncorrected crossfade does to the same notes (7-20dB), so the
# test has room on both sides and no exact-float dependence anywhere.
#
# Skips rather than fails without numpy, the same bargain
# test-effects.py makes.
#
import sys

try:
    import numpy as np
except ImportError:
    print("test-pitch: SKIPPED - no numpy")
    sys.exit(0)

import bench as B

FAILED = []


def check(name, got, hi, unit="dB"):
    ok = got <= hi
    print("  %-46s %9.2f %-4s  want < %-10g %s"
          % (name, got, unit, hi, "ok" if ok else "FAIL"))
    if not ok:
        FAILED.append(name)


WARMUP = int(B.FS) * 2   # fill the 16384-sample line, fade in, settle moments
N = int(B.FS) * 2        # ~23 crossfade power cycles to measure across


def envelope_pp_db(y, win=512, hop=64):
    n = (len(y) - win) // hop
    idx = np.arange(n)[:, None] * hop + np.arange(win)[None, :]
    e = 20 * np.log10(np.sqrt((y[idx] ** 2).mean(axis=1) + 1e-30))

    # keep the measurement off the effect's own entry ramp
    trim = int(0.05 * B.FS / hop)
    e = e[trim:-trim]
    return e.max() - e.min()


def sustained(hz, oct_raw=90, fb_raw=60, x=None):
    if x is None:
        x = B.tone(hz, -12.0, N)
    args = ["--pot", "Signal Chain:Gate=0",
            "--route", "Pitch",
            "--mix", "Pitch=120",
            "--pot", "Pitch:Octave=%d" % oct_raw,
            "--pot", "Pitch:Feedback=%d" % fb_raw]
    left, right, info = B.run(args, x, warmup=WARMUP)
    return left, info


print("pitch crossfade level - sustained notes, effect wet")

# The default setting (octave +1, feedback 0.5), at the two notes the
# tap spacing treats worst from either side of the correlation phase.
for hz in (164.0, 84.0):
    left, info = sustained(hz)
    check("envelope swing (%gHz, default pots)" % hz,
          envelope_pp_db(left), 2.5)
    check("no clipping (%gHz)" % hz, info["clipped"], 0.5, "")

# Octave 0 is no shift at all: both taps read the same sample, and the
# old weights cancelled it outright twice a window.  What should come
# out is the note, at the level it went in with.
left, info = sustained(128.0, oct_raw=60, fb_raw=0)
check("envelope swing (128Hz, octave 0)", envelope_pp_db(left), 4.0)
check("no clipping (octave 0)", info["clipped"], 0.5, "")

# A correction that can reach +24dB must stay out of the way when
# there is nothing to correct.
left, info = sustained(0, x=np.zeros(N, dtype=np.float32))
check("silence stays silent",
      20 * np.log10(np.abs(left).max() + 1e-30), -100.0)

print()
if FAILED:
    print("test-pitch: FAILED - %s" % ", ".join(FAILED))
    sys.exit(1)
print("test-pitch: ok")
