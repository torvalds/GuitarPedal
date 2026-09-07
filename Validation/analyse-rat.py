#!/usr/bin/env python3
#
# Measure [RAT], and print the numbers that go in its page.
#
# Documentation/effects/rat.md is written by hand around this output.
# The prose is the valuable half and a generator cannot write it; this
# exists so that re-measuring is a command rather than an afternoon, and
# so that check-analysis.py can say when the code has moved underneath
# the page.
#
# WHAT IS MEASURED HERE AND WHAT IS NOT
#
# Everything below runs on the host bench - the pedal's own audio core,
# compiled for this machine - so it is deterministic and re-runnable by
# anybody with a compiler.  It is therefore the *model* measuring
# itself.  The page's comparisons against the Helios and against ngspice
# are not here and cannot be: one needs the pedal on a bench and the
# other needs a spice run, and neither belongs in a check that has to
# pass on a laptop.  Those numbers live in the page, with the date of the
# bench session that produced them on them.
#
# THE TONE IS LONG ON PURPOSE
#
# 220 cycles, not 20.  The clamp's operating point is slow: C7's charge
# settles where the two half cycles pass equal charge, and it gets there
# through the diodes' own incremental resistance rather than through R5,
# which is far higher.  Measured on a converged solve at Distortion
# full, the duty cycle reads 47.55% over 20 cycles, 47.05 over 40, 46.74
# over 80 and 46.49 over 220, against 46.46 at 880 - so a 40-cycle tone
# is half a percent high, which is larger than the gap between this
# model and the pedal it models.  A window chosen for speed is a
# measurement of the window.
#
# Run 'make bench' first.  A stale bench measures a pedal you no longer
# have, convincingly.
#
import sys

import numpy as np

sys.path.insert(0, ".")
import bench as B
import loop
import targets as T

FS = 48000.0
RAT = "Rat Sketch"
MODES = ("Silicon", "Stacked", "LED")

#
# A semitone grid from 20 Hz to 20480, and the octave centres are every
# twelfth point of it.  So the curve that gets drawn and the numbers
# that get checked are the same measurement rather than two, and the
# check is a subset of the picture instead of a second opinion about it.
#
# 121 tones is about eleven seconds of bench, which is affordable for
# something that runs when a figure is redrawn.  The eleven-point
# version this replaces was a mermaid limitation - one x label per data
# point - and drew a smooth response as a row of straight segments.
#
SEMITONES = [20.0 * 2 ** (k / 12.0) for k in range(121)]
OCTAVES = SEMITONES[::12]
LABELS = ["20", "40", "80", "160", "320", "640", "1.3k", "2.6k", "5.1k",
          "10k", "20k"]

#
# THE SMALL-SIGNAL LEVEL, WHICH IS LOWER THAN IT LOOKS LIKE IT NEEDS
#
# -100 dBFS, which is far below where a small-signal measurement would
# normally sit.  At full Distortion the network asks for 57 dB, so a
# -66 dBFS stimulus comes out at -9 and the clamp has already started on
# it - and the measurement then reads 2.5 dB of gain that is not there:
#
#       640 Hz, gain in dB against stimulus level
#         in dBFS      -60     -66     -80    -100    -120
#         Distortion 0.0    -5.109  -5.109  -5.109  -5.110  -5.112
#         Distortion 1.0    49.848  54.189  56.645  56.647  56.645
#
# Flat from -80 down, and float32 starts to show at -140 (-5.134).
# -100 is the middle of that. rat-helios.cir's header says the same
# thing about the pedal and capture-rat.py's --level warns about it;
# both were right and neither was heeded until the curve was drawn
# finely enough to look wrong.
#
SMALL_SIGNAL = -100.0


def series(name, values):
    """One line check-analysis.py can read back.

    Printed by hand rather than by repr(): numpy 2 renders a float64 as
    'np.float64(1.0)' inside a list, which is valid python and is not
    the '[1.0, ...]' the checker's regex looks for.  It would have
    parsed as no series at all, and a page with nothing checkable in it
    passes silently - which is the failure mode this whole arrangement
    exists to avoid.
    """
    print("%s [%s]" % (name, ", ".join(repr(float(v)) for v in values)))


def knobs(**kw):
    k = dict(T.target("rat")["knobs"])
    k.update(kw)
    return k


def run(k, x):
    y, _info = B.through(B.BENCH, T.pot_args(T.target("rat"), k),
                         np.asarray(x, np.float32))
    return np.asarray(y, float)


def gain_at(k, hz, dbfs=SMALL_SIGNAL):
    """Small-signal gain, on a whole number of cycles at each frequency."""
    out = []
    for f in hz:
        n = max(int(round(FS / f)) * 60, 9600)
        out.append(round(loop.tone_db(run(k, loop.tone(float(f), dbfs, n)),
                                      float(f)) - dbfs, 2))
    return out


def tone_shape(k, f0=220.0, dbfs=-12.0, cycles=220):
    """Duty cycle and the first two harmonics off a settled tone."""
    n = int(round(FS / f0)) * cycles
    y = run(k, loop.tone(f0, dbfs, n))
    b = y[int(0.15 * len(y)):int(0.9 * len(y))]
    b = b - b.mean()
    Y = np.abs(np.fft.rfft(b * np.hanning(len(b))))
    fr = np.fft.rfftfreq(len(b), 1.0 / FS)

    def h(m):
        i = int(np.argmin(np.abs(fr - m * f0)))
        return Y[max(i - 2, 0):i + 3].max()

    h1 = h(1)
    return (100.0 * np.mean(b > 0), 20 * np.log10(h(2) / h1),
            20 * np.log10(h(3) / h1))


#
# The measurements, as data.  main() prints them for check-analysis.py
# and draw-rat.py imports them to draw, so the page's pictures and the
# page's numbers cannot come from two different runs of two different
# scripts and disagree.
#
DIST_SETTINGS = ((0.0, "Distortion 0.0"), (0.45, "Distortion 0.45"),
                 (1.0, "Distortion 1.0"))
FILTER_SETTINGS = ((0.0, "Filter 0.0"), (0.5, "Filter 0.5"),
                   (1.0, "Filter 1.0"))
CLAMP_RUNGS = list(range(-20, 1, 2))
TRAVEL = [round(0.1 * i, 1) for i in range(11)]




def distortion_response(hz=None):
    return [(label, gain_at(knobs(Distortion=p), hz or SEMITONES))
            for p, label in DIST_SETTINGS]


def filter_response(hz=None):
    return [(label, gain_at(knobs(Distortion=0.45, Filter=f), hz or SEMITONES))
            for f, label in FILTER_SETTINGS]


def clamp_curves():
    """Compression against each mode's own linear region, as 346 quotes it.

    Not the raw output level.  The clamp's whole argument is how far the
    curve departs from a straight line and that it never stops
    departing; plotted raw, all three are a diagonal with the
    interesting part squeezed into the last few decibels, and the LED
    trace lies exactly on top of the linear reference - which is the
    finding, and is invisible.  Subtracting the slope shows it.
    """
    out = []
    for m, name in enumerate(MODES):
        k = knobs(Distortion=0.0, Mode=m)
        row = []
        for d in CLAMP_RUNGS:
            y = run(k, loop.tone(220.0, float(d), 9600))
            row.append(20 * np.log10(np.abs(y[len(y) // 4:]).max()) - d)
        ref = row[0]
        out.append(("Mode %s" % name, [round(v - ref, 2) for v in row]))
    return out


#
# What the Mode switch is actually for, which the transfer curve at
# Distortion minimum does not show.  Down there the input can only
# reach 1.414 V, the LED clamps at 1.81, and the LED position is
# therefore a straight line - true, and the least interesting thing
# about it.  Put some gain in front and the three separate by 7.6 dB of
# output ceiling, which is the whole reason the switch exists.
#
CEILING_RUNGS = list(range(-72, 1, 2))


def mode_ceilings(dist):
    out = []
    for m, name in enumerate(MODES):
        k = knobs(Distortion=dist, Mode=m)
        row = []
        for d in CEILING_RUNGS:
            y = run(k, loop.tone(220.0, float(d), 24000))
            b = y[len(y) // 4:]
            row.append(round(20 * np.log10(np.sqrt(np.mean(b ** 2))
                                           * np.sqrt(2)), 2))
        out.append(("Mode %s" % name, row))
    return out


def travel():
    duty, h2, h3 = [], [], []
    for p in TRAVEL:
        d, a, b = tone_shape(knobs(Distortion=p))
        duty.append(round(d, 2))
        h2.append(round(a, 1))
        h3.append(round(b, 1))
    return duty, h2, h3


def main():
    print("# measured by analyse-rat.py - paste into "
          "Documentation/effects/rat.md")

    #
    # The pre-emphasis, and the corner that eats it.  Two RC legs lift
    # the treble inside the feedback loop; the op-amp's gain-bandwidth
    # takes it back off again, and where it does so depends on how much
    # gain the network is asking for - which is the Distortion knob.
    #
    print("\n## small-signal response, Filter noon, Sweep min, Mode Silicon")
    print("\nx-axis %s" % LABELS)
    for label, g in distortion_response(OCTAVES):
        series(label, g)

    #
    # The clamp, at Distortion minimum where the op-amp is a follower and
    # the input drives the diodes through R5 alone.  This is the one
    # place the transfer curve is visible from outside, and it is the
    # whole argument of the effect: none of the three goes flat.
    #
    print("\n## the clamp at Distortion minimum, compression against in dBFS")
    print("\nx-axis %s" % [str(d) for d in CLAMP_RUNGS])
    for label, row in clamp_curves():
        series(label, row)

    #
    # Asymmetry across the Distortion travel, which is a hump and not a
    # trend - it needs enough drive for the op-amp to rail unevenly and
    # not so much that the clamp flattens the difference away, and those
    # two fight.  ISSUES 343 is where that was first seen on the pedal.
    #
    print("\n## asymmetry across the Distortion travel, 220 Hz at -12 dBFS")
    print("\nx-axis %s" % [str(p) for p in TRAVEL])
    duty, h2, h3 = travel()
    series("duty above zero %", duty)
    series("H2", h2)
    series("H3", h3)

    #
    # Every sixth rung, which is a subset of what gets drawn rather than
    # a second measurement of it - same rule as the octave centres out
    # of the semitone grid above.  A check has to notice the pipeline
    # moving; it does not have to be the picture.
    #
    print("\n## output ceiling against input, at Distortion noon and full")
    print("\nx-axis %s" % [str(d) for d in CEILING_RUNGS[::6]])
    for dist, label in ((0.45, "noon"), (1.0, "full")):
        for name, row in mode_ceilings(dist):
            series("%s at %s" % (name, label), row[::6])

    #
    # The Filter knob, which is the pedal's one tone control and runs
    # backwards: turning it up makes it darker.  One pole, and the whole
    # of what it does.  Taken at Distortion noon, where there is
    # pre-emphasis for it to take back off again - at Distortion minimum
    # there is nothing above the pole for it to remove.
    #
    print("\n## the Filter control, small signal, Distortion noon")
    print("\nx-axis %s" % LABELS)
    for label, g in filter_response(OCTAVES):
        series(label, g)

    return 0


if __name__ == "__main__":
    sys.exit(main())
