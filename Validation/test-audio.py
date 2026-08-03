#!/usr/bin/env python3
#
# What the pedal actually does to a signal, measured over USB audio.
#
# Everything else here checks the firmware against itself.  This checks
# it against an oscilloscope's worth of reality: a signal generator on
# the analog input, the pedal's own USB audio as the measurement
# channel, and arithmetic in between.
#
# The generator cannot be set from here, so its setting is declared on
# the command line and every number that depends on it says what it
# assumed.  A test that quietly assumes 90mV while the dial reads 100mV
# is worse than no test at all.
#
#   ./test-audio.py --ptp 0.100 --freq 440
#
# Skipped rather than failed when there is no pedal, in the same spirit
# as the node check in the Makefile: 'make check' should be worth
# running on a machine with nothing plugged into it.
#
import argparse
import sys
import time

import numpy as np

import audio
import pedal

FAILED = []
NOTED = []


def check(name, ok, detail):
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}: {detail}")
    if not ok:
        FAILED.append(name)


def note(name, detail):
    print(f"  --    {name}: {detail}")
    NOTED.append((name, detail))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ptp", type=float, default=0.100,
                    help="generator amplitude, volts peak to peak")
    ap.add_argument("--freq", type=float, default=440.0,
                    help="generator frequency, Hz")
    ap.add_argument("--seconds", type=float, default=3.0)
    args = ap.parse_args()

    card = audio.find_card()
    p = pedal.port()
    if card is None or p is None:
        print("test-audio: SKIPPED - no pedal on the USB")
        return 0

    print(f"test-audio: card {card}, midi {p}, "
          f"generator {args.ptp * 1000:.0f}mV PtP at {args.freq:.0f}Hz")

    #
    # Wet on the left, the untouched input on the right.  Both are the
    # same instant, so anything below can subtract one from the other
    # without aligning them first.
    #
    n = pedal.effect_count()
    if n:
        pedal.wet_dry(p, n - 1)

    d = audio.trim(audio.capture(args.seconds, card))
    wet, dry = d[:, 0], d[:, 1]

    if audio.peak(dry) < 1e-6:
        print("test-audio: SKIPPED - nothing on the analog input")
        return 0

    #
    # ...and that it is the signal that was *declared*, before anything
    # is measured against the declaration.
    #
    # Everything below is denominated in --ptp and --freq, so an input
    # that is not what those say does not produce a wrong result, it
    # produces a confident wrong result: the frequency check reports
    # -50%, the harmonic measure calls the real tone distortion, and the
    # noise floor counts it as noise.  Three failures that all say
    # "pedal" and all mean "bench".
    #
    # Which stopped being hypothetical the moment a second pedal was
    # wired to this one's input, since a pedal is a perfectly good signal
    # source and simply is not the one on the command line.
    #
    # Skipped rather than failed, because nothing here has been shown to
    # be wrong - the run was not set up to ask.
    #
    f0 = audio.dominant(dry)
    if abs(f0 - args.freq) > 0.05 * args.freq:
        print(f"test-audio: SKIPPED - the input is {f0:.1f} Hz, not the "
              f"{args.freq:.0f} Hz declared.")
        print("            Is the generator on the other channel, or is "
              "another pedal driving this one?")
        return 0

    want = args.ptp / 2 / np.sqrt(2)
    seen = audio.rms(dry * audio.SAMPLE_TO_FLOAT)
    if abs(audio.dbfs(seen / want)) > 6.0:
        print(f"test-audio: SKIPPED - the input is {audio.dbfs(seen / want):+.1f} "
              f"dB from the {args.ptp * 1000:.0f}mV PtP declared.")
        return 0

    # ---- the scale everything else is denominated in ----------------
    #
    # audio/process.h arranges for a 1Vrms sine to peak at 1.0, and every
    # effect's dB marking is denominated in that.  Set by hand off a
    # scope and never checked since, so this reports rather than judges.
    #
    # The left channel is the internal float already; the right is the
    # raw sample and needs converting before the two can be compared.
    vrms = args.ptp / 2 / np.sqrt(2)
    internal = audio.peak(wet)
    off = audio.dbfs(internal / vrms)
    check("scale", abs(off) < 1.0,
          f"internal peak {internal:.5f}, expected {vrms:.5f} for "
          f"{vrms * 1000:.1f}mVrms -> {off:+.2f} dB from the 1Vrms claim")

    # ---- the signal itself ------------------------------------------
    #
    # Measured on the longest stretch with no break in it.  Over a whole
    # capture the resyncs below drag the answer down by a couple of Hz,
    # which would be measuring the USB stream rather than the pedal.
    #
    breaks = audio.discontinuities(dry, args.freq)
    clean = longest_clean(dry, breaks)
    f = audio.dominant(clean) if len(clean) > 8192 else audio.dominant(dry)
    check("frequency", abs(f - args.freq) / args.freq < 0.01,
          f"{f:.2f} Hz in the longest unbroken stretch "
          f"({len(clean) / audio.RATE * 1000:.0f} ms), "
          f"{(f - args.freq) / args.freq * 100:+.2f}%")

    note("distortion", f"{audio.thd(clean, args.freq):.1f} dB below the "
                       f"fundamental, harmonics 2-5")

    # ---- how quiet it is when nothing is asked of it -----------------
    #
    # The tone cannot be switched off from here, so it is left out of
    # the spectrum instead.  Which makes this an upper bound on the
    # pedal's noise rather than a measurement of it: whatever the
    # generator contributes is in here too.
    #
    nf = audio.noise_floor(dry * audio.SAMPLE_TO_FLOAT)
    note("noise floor", f"{nf['noise_dbfs']:.1f} dBFS "
                        f"({nf['noise_vrms'] * 1e6:.0f} uV rms in), "
                        f"{nf['density_dbfs']:.1f} dBFS/sqrt(Hz), "
                        f"snr {nf['snr_db']:.1f} dB")

    #
    # The gate's default threshold is -70dBFS, and a gate set below the
    # noise never closes.  Reported, and deliberately not asserted on.
    #
    # What this measures is whatever is driving the input, and the pedal
    # only underneath it.  What that is depends on the bench: a signal
    # generator, another pedal, or an open jack, and the spread between
    # those is far wider than the thing being asked about.  Measured, by
    # putting a pedal output on one input channel of a board and a bench
    # generator on the other and reading both in the same capture:
    #
    #     LEFT  driven by a pedal, silent    -91.8 dBFS
    #     RIGHT driven by the generator      -69.1 dBFS
    #
    # Twenty-two decibels, between two sources on one converter.  So a
    # pass/fail here is a pass/fail on the cabling, which is what it was:
    # it used to flip between runs while nothing changed but which side
    # of -70 the afternoon landed on.
    #
    # The real check lives in test-loop.py, which knows what is driving
    # the input because it is another pedal it just told to be silent.
    # There the floor is the pedal's own, reads about -91 dBFS, and gets
    # asserted against six decibels of margin rather than against a sign.
    #
    margin = -70.0 - nf["noise_dbfs"]
    note("gate headroom here",
         f"default threshold -70 dBFS is {margin:+.1f} dB "
         f"{'above' if margin > 0 else 'below'} this floor, which is "
         f"the bench's and not the pedal's - see check-loop")

    # ---- and whether the pedal agrees about its own level ------------
    tel = pedal.telemetry(p)
    if tel:
        mine = audio.dbfs(audio.peak(wet))
        check("pedal's own meter", abs(tel["in_dbfs"] - mine) <= 2.0,
              f"reports {tel['in_dbfs']} dBFS in, measured {mine:.1f} dBFS "
              f"(gate {tel['gate']}, load {tel['load']}/127)")
    else:
        note("pedal's own meter", "no telemetry reply")

    # ---- what the USB stream costs, before anything is asked of it ---
    #
    # This is the floor.  A save has to be told apart from it, and if the
    # floor is not zero that is itself worth knowing: it means the USB
    # audio path drops samples continuously, not just when busy.
    #
    b = audio.bursts(breaks)
    per_s = len(breaks) / (len(dry) / audio.RATE)
    check("usb stream idle", not breaks,
          f"{len(breaks)} breaks in {len(dry) / audio.RATE:.1f}s "
          f"({per_s:.1f}/s), largest {max((e['size'] for e in breaks), default=0):.0f}x "
          f"the fastest a clean tone can move")

    # ---- transparency ------------------------------------------------
    #
    # With nothing routed the pedal is trim, gate and volume, all at
    # unity, so the two channels should be the same signal.
    #
    pedal.set_routing(p)
    d = audio.trim(audio.capture(args.seconds, card))
    wet, dry = d[:, 0], d[:, 1]

    # Against the raw input scaled into the same units, not against the
    # raw input - see SAMPLE_TO_FLOAT.
    ref = dry * audio.SAMPLE_TO_FLOAT
    resid = audio.null_db(wet, ref)
    check("null, nothing routed", resid < -80.0,
          f"residue {resid:.1f} dB below the input")

    # ...and that the constant itself is what process.h says it is.
    k = float(np.dot(wet, dry) / np.dot(dry, dry))
    check("input/output scaling", abs(k / audio.SAMPLE_TO_FLOAT - 1) < 0.001,
          f"measured {k:.6f}, process.h says {audio.SAMPLE_TO_FLOAT:.6f} "
          f"({(k / audio.SAMPLE_TO_FLOAT - 1) * 100:+.3f}%)")

    # ---- and what a save costs it ------------------------------------
    #
    # The number this whole rig was worth building for.  A save erases a
    # 4kB sector with interrupts off, which stops core 0 for tens of
    # milliseconds, and core 0 is what drains usb_output.  Standalone
    # nobody saves while playing; plugged into a computer and used as an
    # interface, this is exactly what happens.
    #
    # Measured against the idle floor above rather than against zero,
    # because the floor is not zero.
    #
    nominal = int(round(args.seconds))

    def overhead(act, runs=3):
        got = []
        for _ in range(runs):
            audio.capture(nominal, card, during=act)
            got.append((audio.capture.elapsed - nominal) * 1000)
        return sum(got) / len(got), min(got), max(got)

    idle_ms, ilo, ihi = overhead(None)
    save_ms, slo, shi = overhead(
        lambda: (time.sleep(1.0), pedal.save_scene(p, 0)))

    note("save cost", f"{save_ms - idle_ms:+.0f} ms of stream time "
                      f"(idle {idle_ms:.0f} ms spread {ihi - ilo:.0f}, "
                      f"saving {save_ms:.0f} ms spread {shi - slo:.0f})")

    print()
    if FAILED:
        print(f"test-audio: {len(FAILED)} failed: {', '.join(FAILED)}")
    else:
        print("test-audio: ok")
    return 1 if FAILED else 0


def longest_clean(x, breaks):
    """The longest stretch of x with no break in it."""
    marks = [0] + sorted(int(e["at_ms"] * audio.RATE / 1000)
                         for e in breaks) + [len(x)]
    best = (0, 0)
    for a, b in zip(marks, marks[1:]):
        if b - a > best[1] - best[0]:
            best = (a, b)
    a, b = best
    pad = min(64, (b - a) // 8)
    return x[a + pad:b - pad]


if __name__ == "__main__":
    sys.exit(main())
