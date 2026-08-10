#
# Putting a sine through one effect and asking whether what comes out is
# smooth.
#
# The C half (bench/bench.c) is the pedal's own audio core built for the
# host, so what this drives is the firmware and not a model of it.  This
# half owns every signal and every number.
#
# WHY A SINE, AND WHY THIS ONE
#
# A periodic input through a static nonlinearity comes out periodic with
# the same period, so all of its energy lands on harmonics of the
# fundamental - and how fast those harmonics fall off *is* the
# smoothness of the transfer curve:
#
#   analytic (a real tanh, a biquad)   geometric decay: straight on a
#                                      log-linear plot
#   slope discontinuity (a corner)     ~1/n^2
#   value discontinuity (a step)       ~1/n
#
# So the headline number is the exponent of a power-law fit through the
# harmonic magnitudes.  Near 2 means a corner; a poor power-law fit that
# is straight on log-linear means there isn't one.
#
# THE FREQUENCY IS NOT ARBITRARY
#
# 48000/440 is 1200/11, so 1200 samples hold exactly eleven cycles and a
# 12000-sample window holds exactly 110.  Every harmonic then lands
# exactly on a bin with a rectangular window and there is no leakage at
# all, which is what makes the harmonic/non-harmonic split below an
# exact subtraction rather than an estimate.
#
# More than that: 48000/440 is *not* an integer, and that is the
# property that lets aliasing be seen.  When f0 divides the sample rate
# evenly - 80Hz, 1600Hz, anything at 600 or 30 samples a cycle - every
# aliased harmonic folds back onto another multiple of f0, so it hides
# inside the harmonic series and cannot be told from honest distortion.
# At 440Hz the fold lands off the grid for all but one harmonic in
# eleven, so non-harmonic energy means aliasing and nothing else.
#
# The two companion frequencies are chosen the same way: bin-aligned on
# a 12000-sample window (a multiple of 4Hz) and not a divisor of 48000.
#
import os
import subprocess

import numpy as np

FS = 48000.0

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench", "bench")

# 12000 samples = 250ms.  Every frequency below is an exact number of
# cycles in it.
WINDOW = 12000

# 84Hz is near a low E, 440 is A4, 1320 is the 3rd harmonic of A4 and
# about where a 24th fret lands.  See the note above on why none of them
# divides 48000.
LOW_HZ = 84.0
MID_HZ = 440.0
HIGH_HZ = 1320.0


class BenchError(Exception):
    pass


def run(args, x, warmup=WINDOW * 2):
    """Push a mono signal through the pedal's audio core.

    'x' is in the pedal's internal float scale, where 1.0 is one volt RMS
    of sine and every dB an effect is marked in is measured.  Returns
    (left, right, info) with 'warmup' frames already dropped off the
    front - see settle() for how much is enough.
    """
    if not os.path.exists(BENCH):
        raise BenchError("%s is not built; run 'make bench' in Validation/" % BENCH)

    x = np.asarray(x, dtype=np.float32)
    head = np.zeros(warmup, dtype=np.float32) if warmup else x[:0]

    #
    # The warmup is a repeat of the stimulus rather than silence.  An
    # envelope follower, a compressor and a gate all settle to the
    # signal they are given, so priming them with silence and then
    # measuring a tone measures the attack.
    #
    if warmup:
        reps = -(-warmup // len(x))
        head = np.tile(x, reps)[:warmup]

    full = np.concatenate([head, x])
    buf = np.zeros((len(full), 2), dtype=np.float32)
    buf[:, 0] = full

    p = subprocess.run([BENCH] + list(args), input=buf.tobytes(),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise BenchError(p.stderr.decode().strip())

    y = np.frombuffer(p.stdout, dtype=np.float32).reshape(-1, 2)
    y = y[warmup:]

    #
    # The bench reports its conditions on stderr as alternating name and
    # value - frames, clipped, dropped, fade and the three meters.
    # 'clipped' and 'dropped' are the two that invalidate a measurement
    # rather than merely describing it.
    #
    info = {}
    words = p.stderr.decode().split()
    for i in range(0, len(words) - 1, 2):
        try:
            info[words[i]] = float(words[i + 1])
        except ValueError:
            pass

    return y[:, 0].astype(np.float64), y[:, 1].astype(np.float64), info


def settle(seconds=0.5):
    """Frames of warmup for an effect with no unusual memory.

    Half a second covers the three things that move on their own after
    an init: the 4800-frame fade an effect gets when it is routed
    (EFF_ENABLE_STEPS), the 1/512 slews on trim, volume and the wet/dry
    mix, and an envelope follower's release.  The echo wants far more -
    its delay glides from zero with a time constant of 8333 frames - and
    says so where it is measured.
    """
    return int(seconds * FS)


def tone(hz, dbfs, samples=WINDOW):
    """A sine at an exact number of cycles in the window."""
    cycles = hz * samples / FS
    if abs(cycles - round(cycles)) > 1e-9:
        raise ValueError("%g Hz is %.4f cycles in %d samples, not a whole number"
                         % (hz, cycles, samples))
    amp = 10.0 ** (dbfs / 20.0)
    return (amp * np.sin(2 * np.pi * hz * np.arange(samples) / FS)).astype(np.float32)


def bin_of(hz, n=WINDOW):
    b = hz * n / FS
    assert abs(b - round(b)) < 1e-9
    return int(round(b))


def spectrum(y, n=WINDOW):
    """Magnitudes, rectangular window, no leakage by construction."""
    y = y[-n:]
    return np.abs(np.fft.rfft(y)) * (2.0 / n)


def harmonics(y, f0, n=WINDOW):
    """Magnitude of each harmonic of f0 that fits under Nyquist."""
    mag = spectrum(y, n)
    b0 = bin_of(f0, n)
    idx = np.arange(b0, len(mag), b0)
    return mag[idx]


def alias_db(y, f0, n=WINDOW):
    """Energy off the harmonic grid, in dB below the fundamental.

    Everything a periodic input can honestly produce lands on a multiple
    of f0.  What is left over came back from over Nyquist, which is what
    a corner sounds like rather than what it looks like.
    """
    mag = spectrum(y, n)
    b0 = bin_of(f0, n)
    grid = np.zeros(len(mag), dtype=bool)
    grid[::b0] = True
    grid[0] = True                      # DC is not aliasing
    off = mag[~grid]
    fund = mag[b0]
    if fund <= 0:
        return float("nan")
    return 20 * np.log10(np.sqrt(np.sum(off ** 2)) / fund + 1e-30)


def thd_db(y, f0, n=WINDOW):
    """Harmonic distortion, in dB below the fundamental."""
    h = harmonics(y, f0, n)
    if h[0] <= 0:
        return float("nan")
    return 20 * np.log10(np.sqrt(np.sum(h[1:] ** 2)) / h[0] + 1e-30)


def gain_db(y, x):
    """Best-fit gain, so a phase shift is not read as a level change."""
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)[-len(x):]
    g = (x @ y) / (x @ x)
    return 20 * np.log10(abs(g) + 1e-30), g


def rolloff(y, f0, n=WINDOW, floor_db=-120.0, min_points=4):
    """The exponent p in |H_n| ~ n**-p, over the harmonics that are real.

    Fitted in log-log, which is where a power law is a straight line.
    Returned with the fit's own quality, because the exponent only means
    a corner when the power law actually describes the data - an
    analytic nonlinearity decays geometrically and will fit a power law
    badly, and that bad fit is the answer rather than a failure.

    'r2_pow' near 1 and p near 2: a slope discontinuity.
    'r2_pow' near 1 and p near 1: a step.
    'r2_exp' much better than 'r2_pow': smooth.
    """
    h = harmonics(y, f0, n)
    if len(h) < 2 or h[0] <= 0:
        return dict(p=float("nan"), r2_pow=float("nan"), r2_exp=float("nan"),
                    used=0, thd_db=float("nan"))

    rel = h / h[0]
    keep = np.arange(1, len(rel))
    keep = keep[20 * np.log10(rel[keep] + 1e-30) > floor_db]
    if len(keep) < min_points:
        return dict(p=float("nan"), r2_pow=float("nan"), r2_exp=float("nan"),
                    used=len(keep), thd_db=thd_db(y, f0, n))

    n_h = keep + 1.0
    lg = np.log(rel[keep])

    def fit(u, v):
        a, b = np.polyfit(u, v, 1)
        resid = v - (a * u + b)
        ss = np.sum((v - v.mean()) ** 2)
        return a, 1.0 - np.sum(resid ** 2) / ss if ss > 0 else float("nan")

    slope_pow, r2_pow = fit(np.log(n_h), lg)     # power law
    _, r2_exp = fit(n_h, lg)                     # geometric

    return dict(p=-slope_pow, r2_pow=r2_pow, r2_exp=r2_exp,
                used=len(keep), thd_db=thd_db(y, f0, n))


def repeat_period(f0):
    """The shortest whole number of samples that is a whole number of cycles.

    440Hz is 109.0909 samples a cycle, so there is no such thing as "one
    cycle" to fold on - but 1200 samples are exactly eleven of them, and
    that is what the signal actually repeats on.
    """
    g = np.gcd(int(FS), int(round(f0)))
    return int(FS) // g, int(round(f0)) // g


def cycles(y, f0, n=WINDOW):
    """The steady state folded onto its repeat, and then onto one cycle.

    Two folds, and the second one is free.  The repeat holds 'k' cycles
    at 'period' samples, and period/k is not an integer - which is
    exactly why sample i of the repeat sits at phase (i*k mod period).
    Since k and period are coprime by construction, those phases are the
    period equally spaced values, each hit exactly once.  So reordering
    the repeat by phase gives *one* cycle sampled k times as densely as
    the sample rate would allow: 1200 points across a cycle of 440Hz
    rather than 109.

    That is ordinary equivalent-time sampling and it costs nothing, and
    it is what makes a corner locatable rather than merely detectable.

    Returns (cycle, spread), where spread is the rms difference between
    individual repeats and their mean.  A static nonlinearity gives
    identical repeats and a spread at the noise floor; anything
    modulated does not, and that is information rather than failure.
    """
    period, k = repeat_period(f0)
    y = y[-n:]
    whole = (len(y) // period) * period
    if whole < period:
        raise ValueError("need at least %d samples to fold %g Hz" % (period, f0))

    blocks = y[:whole].reshape(-1, period)
    mean = blocks.mean(axis=0)
    spread = np.sqrt(((blocks - mean) ** 2).mean())

    phase = (np.arange(period) * k) % period
    cycle = np.empty(period)
    cycle[phase] = mean
    return cycle, spread


def corner_position(cycle):
    """Where on the waveform the sharpest bend is, as a fraction of a cycle.

    The second difference of a sampled sine is a scaled sine, so a smooth
    output has a smooth second difference and a corner puts a spike in
    it.  Returned as (position, sharpness) where sharpness is the peak of
    the second difference over its rms - about 1.41 for a pure sine,
    since that is the crest factor of a sine, and larger for a kink.

    The cycle is periodic, so the differences wrap rather than ending.
    """
    d2 = np.roll(cycle, -1) - 2.0 * cycle + np.roll(cycle, 1)
    rms = np.sqrt((d2 ** 2).mean())
    if rms <= 0:
        return float("nan"), float("nan")
    i = int(np.argmax(np.abs(d2)))
    return i / len(cycle), float(np.max(np.abs(d2)) / rms)


def measure(args, f0=MID_HZ, dbfs=-18.0, warmup=None, n=WINDOW):
    """One tone through one configuration, and everything worth saying."""
    x = tone(f0, dbfs, n)
    if warmup is None:
        warmup = settle()
    left, right, info = run(args, x, warmup=warmup)

    out = dict(info)
    out["f0"] = f0
    out["dbfs"] = dbfs
    out.update(rolloff(left, f0, n))
    out["alias_db"] = alias_db(left, f0, n)
    out["gain_db"], _ = gain_db(left, x)

    mean, spread = cycles(left, f0, n)
    out["cycle_spread_db"] = 20 * np.log10(spread / (np.abs(mean).max() + 1e-30) + 1e-30)
    out["corner_at"], out["corner_sharpness"] = corner_position(mean)
    out["_cycle"] = mean
    out["_left"] = left
    return out


def level_sweep(args, f0=MID_HZ, levels=None, **kw):
    """Where an effect stops being smooth, as the input gets louder."""
    if levels is None:
        levels = np.arange(-60, 1, 6.0)
    return [measure(args, f0=f0, dbfs=float(d), **kw) for d in levels]


def describe(m):
    return ("%7.1f dBFS  gain %+7.3f dB  thd %7.1f dB  alias %7.1f dB  "
            "p %5.2f (pow %.3f exp %.3f)  cyc %6.1f dB" %
            (m["dbfs"], m["gain_db"], m["thd_db"], m["alias_db"],
             m["p"], m["r2_pow"], m["r2_exp"], m["cycle_spread_db"]))
