#!/usr/bin/env python3
#
# Does the host bench agree with a real pedal?
#
# bench/ builds the pedal's own audio core for the workstation, and
# everything measured with it is quoted as though it were the pedal.
# That claim is worth exactly one thing: a run where both are asked the
# same question and the answers are put side by side.  This is that run.
#
# It needs no signal generator, no cable and no second board.  The
# stimulus is [TESTTONE], which is in the pedal precisely so that a test
# does not have to be told what the bench is set to, and the capture is
# the pedal's own USB audio.  So the whole path is digital end to end and
# a disagreement is a real disagreement rather than an analog front end.
#
# WHAT IT ASKS
#
# [BOOST] with fold() engaged, because it is the sharpest thing in the
# tree: the transfer curve reflects about the level and reverses slope,
# which throws odd harmonics a long way up and folds a pile of them back
# over Nyquist.  A waveshaper that severe is where two implementations
# of the same arithmetic would diverge if they were going to.  It is
# also static - no LFO, no delay - so the output is periodic and every
# harmonic lands on a bin.
#
# The clean setting is run first as the control.  It is not a null test:
# the 200 Hz basscut takes 0.18 dB out of a 440 Hz tone, and both sides
# have to agree about that too.
#
# WHAT COUNTS AS AGREEMENT
#
# Only harmonics that are actually there.  A wavefolder is symmetric, so
# it makes odd harmonics and no even ones, and the even bins sit at
# -135 dBFS being numerical noise - comparing those would be comparing
# two different roundings of zero.  So anything under HARMONIC_FLOOR is
# skipped, and what is left has to match closely, because these are two
# builds of one source and there is no reason for them to differ at all.
#
import subprocess
import argparse
import sys
import time

try:
    import numpy as np
except ImportError:
    print("test-bench: SKIPPED - no numpy")
    sys.exit(0)

import audio
import bench as B
import pedal

# Effect ids, which are indexes into the firmware's effects[] - the same
# order 'bench --list' prints, since it is printing that array.
CHAIN, BOOST, TESTTONE, SETTINGS = 0, 5, 16, 18

# Pot numbers as the SysEx sees them: 0 is the mix, 1-10 are the effect's.
CHAIN_GATE, CHAIN_TRIM, CHAIN_VOLUME = 1, 4, 5
TT_LEVEL, TT_FREQ, TT_SHAPE = 1, 2, 3
BOOST_BOOST, BOOST_LEVEL, BOOST_BASSCUT, BOOST_HIGHCUT = 1, 2, 3, 4
SETTINGS_USB_OUT, USB_OUT_WET = 1, 1

N = B.WINDOW                    # 12000 samples: 110 whole cycles of 440 Hz
BIN = B.bin_of(B.MID_HZ, N)     # ...so the fundamental is bin 110

# Below this a "harmonic" is two roundings of zero, not a signal.
HARMONIC_FLOOR = -120.0
# Two builds of one source.  This is generous, not tight.
TOLERANCE_DB = 0.5

FAILED = []


def configure(p, boost, level):
    """Put the pedal in a known state, from wherever it happened to be."""
    pedal.set_routing(p, TESTTONE, BOOST)
    time.sleep(0.2)
    for eff, pot, val in (
            (CHAIN, CHAIN_GATE, 0),         # gate off: it has nothing to do here
            (CHAIN, CHAIN_TRIM, 60),        # 0 dB
            (CHAIN, CHAIN_VOLUME, 80),      # 0 dB - it scales the tone, see testtone.h
            (TESTTONE, 0, 120),             # full mix: replace the input rather than add
            (TESTTONE, TT_FREQ, 60),        # pot 60 is 440 Hz exactly
            (TESTTONE, TT_SHAPE, 0),        # sine
            (TESTTONE, TT_LEVEL, 96),       # -18 dBFS
            (BOOST, 0, 120),
            (BOOST, BOOST_BOOST, boost),
            (BOOST, BOOST_LEVEL, level),
            (BOOST, BOOST_BASSCUT, 120),
            (BOOST, BOOST_HIGHCUT, 120),
            (SETTINGS, SETTINGS_USB_OUT, USB_OUT_WET)):
        pedal.set_pot(p, eff, pot, val)
        time.sleep(0.02)
    #
    # Long enough for the enable fade (EFF_ENABLE_STEPS, 100ms), the
    # 1/512 slews on trim, volume and the two mixes, and the boost's own
    # 200 Hz biquad.
    #
    time.sleep(1.0)


def on_bench(boost, level):
    args = ["--pot", "Signal Chain:Gate=0",
            "--route", "Test Tone", "--route", "Boost",
            "--mix", "Test Tone=120",
            "--pot", "Test Tone:Freq=60", "--pot", "Test Tone:Shape=0",
            "--pot", "Test Tone:Level=96",
            "--mix", "Boost=120",
            "--pot", "Boost:Boost=%d" % boost, "--pot", "Boost:Level=%d" % level,
            "--pot", "Boost:Basscut=120", "--pot", "Boost:Highcut=120"]
    #
    # Silence in: the test tone ignores its input, which is the whole
    # point of it.
    #
    left, _, _ = B.run(args, np.zeros(N, dtype=np.float32), warmup=B.settle(1.0))
    return left


def spectrum(y):
    return np.abs(np.fft.rfft(y[-N:])) * 2.0 / N


def compare(name, pedal_y, bench_y, harmonics=9):
    print("  %s" % name)
    mp, mb = spectrum(pedal_y), spectrum(bench_y)

    for n in range(1, harmonics + 1):
        a, b = mp[BIN * n], mb[BIN * n]
        da, db = 20 * np.log10(a + 1e-30), 20 * np.log10(b + 1e-30)
        if max(da, db) < HARMONIC_FLOOR:
            continue
        diff = da - db
        ok = abs(diff) <= TOLERANCE_DB
        print("    harmonic %-2d  pedal %8.2f  bench %8.2f  diff %+6.2f dB  %s"
              % (n, da, db, diff, "ok" if ok else "FAIL"))
        if not ok:
            FAILED.append("%s harmonic %d" % (name, n))

    #
    # Everything off the harmonic grid, which for a folding waveshaper is
    # the harmonics that went over Nyquist and came back - the thing a
    # corner actually sounds like.  Compared with a looser bound because
    # this one really does contain the converter's noise floor as well.
    #
    grid = np.zeros(len(mp), dtype=bool)
    grid[::BIN] = True
    grid[0] = True
    ea = 20 * np.log10(np.sqrt((mp[~grid] ** 2).sum()) + 1e-30)
    eb = 20 * np.log10(np.sqrt((mb[~grid] ** 2).sum()) + 1e-30)
    #
    # Only meaningful when there is aliasing to compare.  In the clean
    # case both sides are sitting on their own noise, and the pedal's is
    # a real converter's - so it is reported and not judged.
    #
    judge = max(ea, eb) > -80.0
    ok = abs(ea - eb) <= 1.0 or not judge
    print("    non-harmonic  pedal %8.2f  bench %8.2f  diff %+6.2f dB  %s"
          % (ea, eb, ea - eb, "ok" if ok else "FAIL" if judge else "(noise, not judged)"))
    if not ok:
        FAILED.append("%s aliasing" % name)
    print()


def main():
    #
    # Which board, when there is more than one.
    #
    # Refusing to guess is the default and stays the default: two boards
    # of one revision look identical to everything except their serial,
    # and answering either of them is how a test ends up reporting on
    # the board nobody was asking about.  --target names one, through
    # pedal.find(), which refuses ambiguity in its own right.
    #
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", default=None,
                    help="serial, label or product substring naming one pedal")
    args = ap.parse_args()

    found = pedal.discover()
    if not found:
        print("%s: SKIPPED - no pedal on the USB" % "test-bench")
        return 0
    if args.target:
        d = pedal.find(args.target, among=found)
        if not d:
            print("%s: SKIPPED - '%s' does not name exactly one of the %d "
                  "pedals here: %s"
                  % ("test-bench", args.target, len(found),
                     ", ".join(x["label"] for x in found)))
            return 0
    elif len(found) > 1:
        print("%s: SKIPPED - %d pedals and no --target; this wants exactly one"
              % ("test-bench", len(found)))
        return 0
    else:
        d = found[0]
    print("test-bench: %s, card %d, port %s"
          % (d["label"], d["card"], d["port"]))
    print("            [TESTTONE] 440 Hz -18 dBFS into [BOOST], captured over USB")
    print()

    try:
        subprocess.run(["arecord", "--version"], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        print("test-bench: SKIPPED - no arecord")
        return 0

    for name, boost, level in (
            ("clean - Boost 0 dB, Level 0 dB (the 200 Hz basscut is -0.18 dB here)", 0, 120),
            ("folding - Boost +30 dB, Level -26.7 dB", 90, 40)):
        configure(d["port"], boost, level)
        compare(name, audio.capture(2, d["card"])[:, 0], on_bench(boost, level))

    #
    # Leave it quiet.  The tone is at full mix and would otherwise still
    # be playing when the next person picks the pedal up.
    #
    pedal.set_routing(d["port"])
    pedal.set_pot(d["port"], SETTINGS, SETTINGS_USB_OUT, 2)     # back to Dry

    if FAILED:
        print("test-bench: FAILED - %s" % ", ".join(FAILED))
        return 1
    print("test-bench: ok - the bench and the pedal agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
