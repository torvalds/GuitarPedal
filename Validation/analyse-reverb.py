#!/usr/bin/env python3
#
# Measure [REVERB], and print the numbers that go in its page.
#
# WHAT A REVERB IS MEASURED BY
#
# How long it rings, and whether the top goes before the bottom.  Both
# come off the decay curve, and the standard way to get that is
# Schroeder backward integration: integrate the squared tail from the
# end towards the start, and what comes out is the ensemble-average
# decay of the room rather than one noisy realisation of it.
#
# T60 is then read as three times the time to fall from -5 dB to -25 dB.
# Reading the whole 60 dB directly would run into the point where the
# tail stops decaying and the arithmetic floor takes over, and the -5
# start skips the direct sound, which is not part of the decay.
#
# THE STIMULUS
#
# A gated noise burst rather than an impulse.  Both are correct and the
# burst has thirty decibels more energy in it, which matters because the
# thing being fitted is the bottom of a decay.
#
# There is a lead-in of silence before it and that is not padding: an
# effect fades in over EFF_ENABLE_STEPS (4800 frames) when it is routed,
# and the slews on trim and volume take another 512-frame time constant.
# Measuring a tail through a gain that is still moving measures the gain.
#
# WHAT IS NOT MEASURED HERE
#
# What it costs the cpu.  That is on the board, not on the bench - see
# Validation/measure-load.py - and the page quotes it as such.
#
import math
import sys

import numpy as np

sys.path.insert(0, ".")
import bench as B
import pots as P

REVERB = "Reverb"
FS = int(B.FS)
OCTAVES = [125, 250, 500, 1000, 2000, 4000]


def routed(room_pot=77, damp_pot=45):
    return ["--pot", "Signal Chain:Gate=0", "--route", REVERB,
            "--mix", "Reverb=120",
            "--pot", "%s:Room=%d" % (REVERB, room_pot),
            "--pot", "%s:Damp=%d" % (REVERB, damp_pot)]


def burst(lead_s=0.6, burst_ms=20.0, tail_s=8.0, dbfs=-6.0, seed=3):
    """Silence, a short noise burst, then room to decay in."""
    rng = np.random.default_rng(seed)
    n_b = int(burst_ms / 1000 * FS)
    x = np.zeros(int(lead_s * FS) + n_b + int(tail_s * FS), dtype=np.float32)
    i = int(lead_s * FS)
    x[i:i + n_b] = (10 ** (dbfs / 20) *
                    rng.standard_normal(n_b)).astype(np.float32)
    return x, i + n_b


def band(y, lo, hi):
    """Brick-wall band, done in the frequency domain.

    A filter with a decay of its own would be measured along with the
    room's, which is the one thing this must not do.
    """
    n = 1
    while n < len(y):
        n *= 2
    Y = np.fft.rfft(y, n)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    Y[(f < lo) | (f >= hi)] = 0
    return np.fft.irfft(Y, n)[:len(y)]


def t60(y, lo=-5.0, hi=-25.0):
    """Schroeder backward integration, fitted over lo..hi dB.

    Returns seconds, or nan when the tail never gets that far down -
    which happens, and is a real answer about a band with nothing in it
    rather than a failure to measure.
    """
    e = np.cumsum(y[::-1] ** 2)[::-1]
    if e[0] <= 0:
        return float("nan")
    db = 10 * np.log10(e / e[0] + 1e-30)
    a = np.argmax(db <= lo)
    b = np.argmax(db <= hi)
    if b <= a:
        return float("nan")
    t = np.arange(a, b) / FS
    slope = np.polyfit(t, db[a:b], 1)[0]
    if slope >= 0:
        return float("nan")
    return float(-60.0 / slope)


def tail_of(args, x, cut):
    left, _, info = B.run(args, x, warmup=0)
    return np.asarray(left)[cut:], info


def levels(y, window):
    n = len(y) // window
    return 20 * np.log10(np.sqrt((y[:n * window].astype(np.float64) ** 2)
                                 .reshape(n, window).mean(axis=1)) + 1e-18)


def main():
    print("# measured by analyse-reverb.py - paste into "
          "Documentation/effects/reverb.md\n")

    x, cut = burst()
    print("## stimulus\n")
    print("%.0f ms of noise at -6 dBFS after %.1f s of silence, %.0f s to decay"
          % (20.0, 0.6, 8.0))

    #
    # Room, which is the feedback gain shared by all eight combs.
    #
    print("\n## Room: T60 in seconds, broadband, Damp at default\n")
    room_pots = [0, 24, 48, 72, 96, 120]
    print("x-axis " + str([round(P.value(REVERB, "Room", p), 2)
                           for p in room_pots]))
    row = []
    for p in room_pots:
        y, info = tail_of(routed(room_pot=p), x, cut)
        if info.get("clipped"):
            print("WARNING: clipped at Room=%d" % p)
        row.append(round(t60(y), 2))
    print("T60 s  " + str(row))

    #
    # Damp, which is a one-pole lowpass inside each comb's feedback, so
    # it does not shorten the tail evenly - that is the whole point of
    # it and it only shows per band.
    #
    print("\n## Damp: T60 by octave\n")
    print("x-axis " + str(OCTAVES))
    for dp in (0, 60, 120):
        y, _ = tail_of(routed(damp_pot=dp), x, cut)
        row = [round(t60(band(y, f, f * 2)), 2) for f in OCTAVES]
        print("Damp %.2f  %s" % (P.value(REVERB, "Damp", dp), row))

    #
    # And the same for Room, per octave, to show the two controls are
    # not two names for one thing.
    #
    print("\n## Room: T60 by octave, Damp at default\n")
    print("x-axis " + str(OCTAVES))
    for rp in (0, 72, 120):
        y, _ = tail_of(routed(room_pot=rp), x, cut)
        row = [round(t60(band(y, f, f * 2)), 2) for f in OCTAVES]
        print("Room %.2f  %s" % (P.value(REVERB, "Room", rp), row))

    #
    # What the comb modulation is for.  A fixed comb bank rings on a
    # fixed set of frequencies; the modulation walks them, so a steady
    # tone sitting on one of them does not sit there.  The wet level
    # wandering *is* the effect working.
    #
    #
    # Long enough to mean something.  The slowest LFO is 0.21 Hz, so a
    # 4.8 s period, and an eight-second capture caught barely one and a
    # half of them - which measures whichever part of the cycle it
    # happened to land on rather than the range.  Ninety seconds is
    # about eighteen cycles of the slowest and far more of the rest.
    #
    # The chart stays a 5.5 s excerpt at half-second resolution, and it
    # has to: eleven points is what mermaid can label, and stretching
    # eleven points over ninety seconds would put the window above every
    # rate being looked at and average the wobble away into a flat line.
    # So the excerpt shows the shape and the statistics below cover the
    # whole run.
    #
    print("\n## The modulation: wet level of a steady 440 Hz tone\n")
    n = int(90.0 * FS)
    t = np.arange(n) / FS
    tone = (10 ** (-18.0 / 20) * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    y, _ = tail_of(routed(), tone, int(5.0 * FS))
    win = int(0.5 * FS)
    full = levels(y, win)
    print("x-axis " + str([round(0.5 * i, 1) for i in range(11)]))
    print("dBFS   " + str([round(float(v), 2) for v in full[:11]]))
    print("first 5.5 s: %.2f dB peak to peak"
          % (full[:11].max() - full[:11].min()))
    print("over the whole %.0f s: %.2f dB peak to peak, %.2f dB std, "
          "p5..p95 %.2f dB"
          % (len(y) / FS, full.max() - full.min(), full.std(),
             np.percentile(full, 95) - np.percentile(full, 5)))

    #
    # The comb read is interpolated, so the modulation should add
    # nothing above what went in.  Band-limit and look up high; see the
    # commit that changed it for why differencing two builds does not
    # work.
    #
    print("\n## What the modulated read manufactures\n")
    rng = np.random.default_rng(7)
    src = rng.standard_normal(int(6.0 * FS)) * 0.1
    taps = 2047
    k = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * 1000.0 / FS * k) * np.hamming(taps)
    lp = np.convolve(src, h / h.sum(), mode="same").astype(np.float32)
    y, _ = tail_of(routed(), lp, int(0.6 * FS))

    def above(sig, f_lo, nfft=1 << 15):
        segs = len(sig) // nfft
        acc = np.zeros(nfft // 2 + 1)
        w = np.hanning(nfft)
        for i in range(segs):
            acc += np.abs(np.fft.rfft(sig[i * nfft:(i + 1) * nfft] * w)) ** 2
        f = np.fft.rfftfreq(nfft, 1.0 / FS)
        return 10 * math.log10(float(np.sum(acc[f >= f_lo]) / np.sum(acc))
                               + 1e-30)

    print("input above 4 kHz   %7.1f dB of its total" % above(lp, 4000.0))
    print("output above 4 kHz  %7.1f dB of its total" % above(y, 4000.0))


main()
