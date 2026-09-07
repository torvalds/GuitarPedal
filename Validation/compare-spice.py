#!/usr/bin/env python3
#
# An effect claims to be a circuit.  Ask ngspice.
#
# [RAT] is a deliberate simplification - a transistor becomes a gain, an
# op-amp becomes a pole and two limits - and the interesting question
# about it is not "is it right", which it is not by construction, but
# *which* of the things it gave up can be heard in a measurement and how
# far off it is.  This is the instrument for that question.
#
# It takes a target name rather than knowing about one effect, because
# the comparison is the same comparison whatever is being compared, and
# because two of these scripts would be two opinions about what "the
# level ladder" means.
#
# WHAT IT ASKS, AND WHY THESE
#
#   the level ladder      output against input from -66 to 0 dBFS, and
#                         the lift across the useful part of it.  This is
#                         where a fixed diode drop shows up: a real
#                         clipper's limit is a logarithm of the current
#                         through it, so a real pedal gets louder when it
#                         is played harder even after it has started to
#                         clip, and a number does not.
#   the harmonics         H2 to H5 at a playing level.  H3 is the shape
#                         of the clipping; H2 is its asymmetry, and
#                         asymmetry is the thing a symmetric model
#                         cannot have at all.
#   the small signal      the linear chain, where nothing is clipping,
#                         which is the half of the model that could in
#                         principle be exact.
#   aliasing              energy off the harmonic grid.  ngspice is
#                         point-sampled here exactly as the model is, so
#                         its number is the aliasing an ideal
#                         non-oversampled version of this circuit would
#                         have - which is the right thing to be compared
#                         against, and is not zero.
#
# WHY THE STIMULUS IS 220 Hz AND SIX WINDOWS LONG
#
# 220 Hz is bin-exact on a 12000-sample window and does not divide 48000,
# so an aliased harmonic folds off the harmonic grid instead of hiding
# inside it - bench.py's header has that argument at length.  Six windows
# because ngspice starts its transient from the operating point and then
# has a full-amplitude note switched on at it, and the coupling
# capacitors take most of a second to stop arguing about that; the tail
# is what gets measured.  Getting this wrong does not look like an error,
# it looks like a circuit with a lot of second harmonic.
#
import argparse
import os
import sys

import numpy as np

import bench as B
import ngspice as NG
from targets import TARGETS, pot_args

HERE = os.path.dirname(os.path.abspath(__file__))

FS = B.FS
N = B.WINDOW
REP = 6
F0 = 220.0
VPEAK = 1.41421356

LADDER = (-66, -54, -42, -30, -24, -18, -12, -6, 0)
LIN_HZ = (40, 80, 160, 320, 640, 1250, 2500, 5000)


def harmonics(y):
    Y = np.abs(np.fft.rfft(y))
    b = int(round(F0 * N / FS))
    f1 = max(Y[b], 1e-12)
    return [20 * np.log10(max(Y[b * k], 1e-12) / f1) for k in (2, 3, 4, 5)]


def db(x):
    return 20 * np.log10(max(float(np.sqrt(np.mean(x ** 2))), 1e-12))


def one(t, knobs, rival=False):
    """Run one pot setting, model and oracle, and return the numbers."""
    params = t["spice"](knobs)
    args = pot_args(t, knobs)
    rargs = pot_args(t, knobs, t["rival"]) if rival and t.get("rival") else None

    lad = []
    for dbfs in LADDER:
        x = B.tone(F0, dbfs, N * REP)
        y, _, _ = B.run(args, x, warmup=B.settle())
        s = NG.tran(t["netlist"], t["node"], x * VPEAK, fs=FS,
                    settle=0.3, params=params) / VPEAK
        row = [dbfs, db(s[-N:]), db(y[-N:])]
        if rargs:
            r, _, _ = B.run(rargs, x, warmup=B.settle())
            row.append(db(r[-N:]))
        lad.append(row)

    #
    # Harmonics and aliasing at one level rather than all of them,
    # because a spectrum is only worth printing where somebody plays.
    #
    x = B.tone(F0, -12.0, N * REP)
    y, _, _ = B.run(args, x, warmup=B.settle())
    s = NG.tran(t["netlist"], t["node"], x * VPEAK, fs=FS,
                settle=0.3, params=params) / VPEAK
    hm, hs = harmonics(y[-N:]), harmonics(s[-N:])
    al = (B.alias_db(s[-N:], F0), B.alias_db(y[-N:], F0))

    lin = []
    for hz in LIN_HZ:
        k = round(hz * N / FS)
        f = k * FS / N
        x = B.tone(f, -140.0, N * 3)
        y, _, _ = B.run(args, x, warmup=B.settle())
        g = 20 * np.log10(abs(np.fft.rfft(y[:N])[k])
                          / abs(np.fft.rfft(x[:N])[k]))
        lin.append((f, NG.ac(t["netlist"], t["node"], [f], params=params)[0], g))
    return lad, (hs, hm), al, lin


def report(name, t, rival=False):
    print("=" * 62)
    print("%s   effect %s   oracle %s.cir"
          % (name, t["effect"], t["netlist"]))
    for over in t["settings"]:
        knobs = dict(t["knobs"])
        knobs.update(over)
        label = ", ".join("%s=%s" % (k, v) for k, v in over.items()) or "default"
        print("\n  [%s]" % label)
        lad, (hs, hm), al, lin = one(t, knobs, rival)

        head = "    in dBFS   spice    model    delta"
        if len(lad[0]) > 3:
            head += "    %s" % t["rival"]
        print(head)
        for row in lad:
            line = "    %6d  %7.2f  %7.2f  %+7.2f" % (
                row[0], row[1], row[2], row[2] - row[1])
            if len(row) > 3:
                line += "  %7.2f" % row[3]
            print(line)
        lift = lambda c: [r for r in lad if r[0] == -18][0][c] \
                       - [r for r in lad if r[0] == -60 + 6][0][c]
        print("    lift -54..-18   spice %+.2f   model %+.2f"
              % (lift(1), lift(2)))

        print("    harmonics at -12 dBFS, dB below the fundamental")
        print("              H2      H3      H4      H5")
        print("      spice %7.1f %7.1f %7.1f %7.1f" % tuple(hs))
        print("      model %7.1f %7.1f %7.1f %7.1f" % tuple(hm))
        print("    off the harmonic grid   spice %6.1f   model %6.1f dB"
              % al)

        worst = max(abs(g - r) for _, r, g in lin)
        print("    small signal, model minus spice, worst %.2f dB" % worst)
        print("      " + "  ".join("%6.0f" % f for f, _, _ in lin))
        print("      " + "  ".join("%+6.2f" % (g - r) for _, r, g in lin))


#
# The same comparison on real playing, in third-octave bands.
#
# This is the measurement that found the two worst errors in this
# directory, and it found both of them in one run after every other
# instrument here had said the models were fine.  A sine level ladder, a
# small-signal sweep over twenty pot settings, the harmonic ratios and
# the pulse battery all agreed to about a decibel while two of the models
# were thirteen decibels dark above 4 kHz.
#
# The reason is not subtle once it is said out loud.  Everything else
# here puts *one frequency in at a time*, and the harmonics of one
# frequency are a sparse comb: a model can get every one of them right
# and still be wrong about what it does to the dense high end a chord or
# a pick attack produces.  Real material is the only stimulus that fills
# the band, and a bank of third-octave bands is the crudest possible way
# to look at it, which is why it works - it cannot be gamed by a phase
# error or a fraction of a millisecond of delay, both of which a null
# test reports as catastrophic and nobody can hear.
#
# Four seconds because that is what MAX_TRAN_SAMPLES allows, and the
# committed bass line because a test that needs a file somebody has to go
# and find is a test that stops being run.
#
BANDS = (63, 125, 250, 500, 1000, 2000, 4000, 8000, 12000)


def third_octave(v, n=8192):
    w = np.hanning(n)
    acc = np.zeros(n // 2 + 1)
    for i in range(0, len(v) - n, n // 2):
        acc += np.abs(np.fft.rfft(v[i:i + n] * w)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / FS)
    return np.array([10 * np.log10(acc[(f >= fc / 2 ** (1 / 6.0))
                                       & (f < fc * 2 ** (1 / 6.0))].sum() + 1e-30)
                     for fc in BANDS])


def played(name, t, knobs, seconds=4.0):
    import audio

    x, _ = audio.decode(os.path.join(HERE, "Inputs", "BassForLinus.mp3"),
                        seconds=seconds)
    x = np.asarray(x / max(np.abs(x).max(), 1e-9) * 0.35, dtype=np.float32)

    y, _, _ = B.run(pot_args(t, knobs), x, warmup=B.settle())
    s = NG.tran(t["netlist"], t["node"], x * VPEAK, fs=FS, settle=0.3,
                params=t["spice"](knobs)) / VPEAK
    y, s = y[:len(x)], s[:len(x)]
    #
    # Level-matched, because a gain error is what every other table here
    # already measures and this one is about the balance.
    #
    unit = lambda v: v / np.sqrt(np.mean(v ** 2))
    d = third_octave(unit(y)) - third_octave(unit(s))
    print("    " + "  ".join("%6d" % b for b in BANDS)
          + "     level, model/spice")
    print("    " + "  ".join("%+6.1f" % v for v in d)
          + "     %+6.2f/%+6.2f"
          % (20 * np.log10(np.sqrt(np.mean(y ** 2))),
             20 * np.log10(np.sqrt(np.mean(s ** 2)))))
    return d


#
# Sub-sample alignment, so a difference can be about shape.
#
# max_lag is under one cycle of F0 on purpose: a tone correlates with
# itself once per period, so a wider search finds whichever cycle the
# arithmetic liked.  The shift itself is a phase rotation rather than an
# integer move, because the delay being removed here is a fraction of a
# sample as often as not.
#
def _align(ref, test, max_lag=None):
    #
    # Signed, which is why this is not audio.delay_samples().  That one
    # searches r[:max_lag] and so only ever finds a lag - handed a signal
    # that is *early* it returns 0.0, silently, and the caller subtracts
    # nothing and reports the whole residual.  Checked on a known shift:
    # +3.7 samples comes back as +3.704 and -2.5 comes back as 0.000.
    # Here the model has always been the late one, but a helper that is
    # right only in the direction you happen to be looking is how the
    # next person gets it wrong.
    #
    L = max_lag or int(0.45 * FS / F0)
    n = 1 << int(np.ceil(np.log2(len(ref) + 2 * L)))
    r = np.fft.irfft(np.fft.rfft(test, n) * np.conj(np.fft.rfft(ref, n)), n)
    idx = np.concatenate([np.arange(0, L + 1), np.arange(n - L, n)])
    k = idx[int(np.argmax(r[idx]))]
    a, b, c = r[(k - 1) % n], r[k], r[(k + 1) % n]
    den = a - 2 * b + c
    frac = 0.5 * (a - c) / den if den else 0.0
    return (k - n if k > n // 2 else k) + frac


def _shift(v, k):
    n = len(v)
    f = np.fft.rfft(v)
    w = np.exp(-2j * np.pi * k * np.arange(len(f)) / n)
    return np.fft.irfft(f * w, n)


#
# The same comparison drawn rather than tabulated.
#
# A table says a model is 0.8 dB out in a band and cannot say what it did
# to a waveform, and the two failures this file has caught so far were
# both shape rather than level: a low-pass on the wrong side of a
# limiter, and a solver pole slewing every clipped edge.  Neither had a
# number that looked wrong until somebody listened.  So: the circuit and
# the model on the same axes, at three time scales, plus the difference
# between them - which is the only one of the four that has a scale worth
# reading, because it is the thing that is supposed to be zero.
#
def waves(name, t, knobs, out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    params = t["spice"](knobs)
    args = pot_args(t, knobs)

    #
    # A burst rather than a steady tone, because the attack is where a
    # model and a circuit come apart: everything with a capacitor in it
    # is still arriving for the first few cycles.
    #
    n = int(0.25 * FS)
    k = np.arange(n)
    env = np.clip(k / (0.002 * FS), 0, 1) * np.clip((n - k) / (0.002 * FS), 0, 1)
    x = (10 ** (-12.0 / 20.0) * np.sin(2 * np.pi * F0 * k / FS) * env)
    x = np.concatenate([np.zeros(int(0.05 * FS)), x, np.zeros(int(0.2 * FS))])
    x = np.asarray(x, dtype=np.float32)

    y, _, _ = B.run(args, x, warmup=B.settle())
    s = NG.tran(t["netlist"], t["node"], x * VPEAK, fs=FS, settle=0.3,
                params=params) / VPEAK
    y, s = y[:len(x)], s[:len(x)]

    a = int(0.05 * FS)
    spans = (("the burst", a - int(0.005 * FS), a + int(0.28 * FS)),
             ("the attack", a - int(0.001 * FS), a + int(0.02 * FS)),
             ("two cycles, settled", a + int(0.12 * FS),
              a + int(0.12 * FS) + int(2.0 / F0 * FS)))

    fig, ax = plt.subplots(4, 1, figsize=(11, 11))
    fig.suptitle("%s   %s   %s"
                 % (t["effect"], t["netlist"],
                    ", ".join("%s=%s" % kv for kv in sorted(knobs.items()))))
    for i, (lab, lo, hi) in enumerate(spans):
        tt = (np.arange(lo, hi) - a) * 1000.0 / FS
        ax[i].plot(tt, s[lo:hi], lw=1.1, color="#0072b2", label="ngspice")
        ax[i].plot(tt, y[lo:hi], lw=0.9, color="#d55e00", label="model")
        ax[i].set_title(lab, fontsize=9, loc="left")
        ax[i].set_ylabel("out")
        ax[i].grid(alpha=0.25)
        ax[i].legend(fontsize=8, loc="upper right")
    #
    # The difference, with the delay taken out of it first.
    #
    # A model that is a fifth of a millisecond late is not a model that
    # is wrong by the whole waveform, but that is what a plain
    # subtraction says: two square-ish waves offset by less than a
    # sample's worth of anything audible cancel nowhere and add
    # everywhere at the edges, so the residual is bigger than either
    # signal.  Both are drawn - the raw one behind, in grey, because how
    # much of the residual *was* delay is itself the finding.
    #
    # The delay is real and it is worth naming rather than only
    # removing: it is the group delay of whatever the model filters
    # before its limiter, and the circuit does not have it because its
    # own filter is short-circuited the moment a diode conducts.
    #
    lo, hi = spans[0][1], spans[0][2]
    tt = (np.arange(lo, hi) - a) * 1000.0 / FS
    #
    # The lag is taken over the settled part and the residual is scored
    # there too.  Both for the same reason: the first few cycles of a
    # note are a transient in their own right, every capacitor in the
    # circuit is still arriving, and a peak taken across them is a
    # measurement of the attack wearing the label of the whole note.
    #
    m0, m1 = a + int(0.10 * FS), a + int(0.24 * FS)
    lag = _align(s[m0:m1], y[m0:m1])
    raw = y[lo:hi] - s[lo:hi]
    d = _shift(y[lo:hi] - y[lo:hi].mean(), -lag) + y[lo:hi].mean() - s[lo:hi]
    w = slice(m0 - lo, m1 - lo)
    ref = np.abs(s[m0:m1]).max()
    db = lambda v: 20 * np.log10(max(v, 1e-12) / max(ref, 1e-12))
    ax[3].plot(tt, raw, lw=0.7, color="#bbb", label="raw")
    ax[3].plot(tt, d, lw=0.8, color="#444", label="delay removed")
    ax[3].set_title("model minus ngspice, over the settled part: "
                    "peak %.1f dB raw, %.1f dB aligned (rms %.1f dB), "
                    "%.0f us of delay"
                    % (db(np.abs(raw[w]).max()), db(np.abs(d[w]).max()),
                       db(float(np.sqrt(np.mean(d[w] ** 2)))),
                       lag * 1e6 / FS),
                    fontsize=9, loc="left")
    ax[3].set_xlabel("ms from the start of the note")
    ax[3].legend(fontsize=8, loc="upper right")
    ax[3].grid(alpha=0.25)
    fig.tight_layout()
    path = os.path.join(out_dir, "waves-%s.png" % name)
    fig.savefig(path, dpi=110)
    plt.close(fig)
    print("  %s" % path)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", nargs="*", default=sorted(TARGETS),
                    help="which of %s" % ", ".join(sorted(TARGETS)))
    ap.add_argument("--rival", action="store_true",
                    help="also run the exact model where one exists")
    ap.add_argument("--waves", action="store_true",
                    help="draw the waveforms instead of tabulating them")
    ap.add_argument("--played", action="store_true",
                    help="third-octave bands on real material, which is the "
                         "only thing here that can see a shape error")
    ap.add_argument("--out", default=".", help="where --waves writes its PNGs")
    args = ap.parse_args()

    try:
        B.refuse_if_stale()
    except B.BenchError as e:
        sys.exit("compare-spice: %s" % e)

    for name in args.target:
        if name not in TARGETS:
            sys.exit("no such target: %s" % name)
        t = TARGETS[name]
        if args.waves:
            waves(name, t, dict(t["knobs"]), args.out)
        elif args.played:
            print("=" * 62)
            print("%s   effect %s   oracle %s.cir"
                  % (name, t["effect"], t["netlist"]))
            for over in t["settings"]:
                knobs = dict(t["knobs"])
                knobs.update(over)
                print("\n  [%s]"
                      % (", ".join("%s=%s" % kv for kv in over.items())
                         or "default"))
                played(name, t, knobs)
        else:
            report(name, TARGETS[name], args.rival)


if __name__ == "__main__":
    main()
