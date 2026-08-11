#!/usr/bin/env python3
#
# Where does a biquad actually land?
#
# Every cookbook constructor in Audio/biquad.h takes a frequency
# and builds a filter that is supposed to be characteristic at it.  This
# asks each of them, right across the audio range, whether it did.
#
# The coefficients come out of bench/coeff, which is the pedal's own
# biquad.h compiled for the host.  Nothing here builds a filter.  The
# only arithmetic on this side is evaluating a transfer function that
# already exists, in double, which is a different job from constructing
# one and cannot agree with a construction bug by accident.
#
# The locator is per filter type and is deliberately the *definition* of
# the frequency rather than a formula rearranged from the coefficients:
# a notch is where the response is smallest, a low-pass is where it has
# fallen 3 dB from the passband, a shelf is where it reaches half its
# decibels.  Ask the response, not the algebra.
#
import subprocess
import sys
import math
import cmath
import os

FS = 48000.0
HERE = os.path.dirname(os.path.abspath(__file__))
COEFF = os.path.join(HERE, "bench", "coeff")

#
# 20 Hz to 20 kHz is what the pots offer - see the EXPONENTIAL(20.0
# 20480.0) lines in the effect headers - and the whole point of 141 is
# that a defect at the bottom is invisible if you only look where the
# complaint was.  So: every semitone, all the way down.
#
FREQS = [20.0 * 2 ** (n / 12.0) for n in range(0, 121)]

#
# Q of 0.5 to 50.  The pedal only uses about 1 today, but 139 wants 20 or
# more, and a placement error shows up in proportion to how narrow the
# filter is - a sweep that only tried Q 1 would have found nothing.
#
QS = [0.5, 0.707, 1.0, 5.0, 20.0, 50.0]

GAINS = [-20.0, -6.0, 6.0, 20.0]


def response(c, f):
    """|H(f)| in double, from float32 coefficients."""
    b0, b1, b2, a1, a2 = c
    z = cmath.exp(-2j * math.pi * f / FS)
    return abs((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z))


def phase(c, f):
    b0, b1, b2, a1, a2 = c
    z = cmath.exp(-2j * math.pi * f / FS)
    return cmath.phase((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z))


def db(x):
    return 20 * math.log10(max(abs(x), 1e-30))


#
# Nothing is searched for above Nyquist.  The response is periodic, so a
# search that runs past it finds the image and reports a corner at 62
# kHz with a straight face - which is what the first version of this did.
#
NYQUIST = FS / 2


def band(f0, span=8.0):
    return max(0.5, f0 / span), min(NYQUIST - 0.5, f0 * span)


def solve(fn, lo, hi, iters=200):
    """Bisect fn from lo to hi.  fn is negative below the answer."""
    if lo >= hi or fn(lo) > 0 or fn(hi) < 0:
        return None
    for _ in range(iters):
        mid = math.sqrt(lo * hi)		# geometric: frequency is log
        if fn(mid) < 0:
            lo = mid
        else:
            hi = mid
    return math.sqrt(lo * hi)


def extremum(c, f0, want_min):
    """Ternary search for the deepest or tallest point, an octave either side."""
    lo, hi = band(f0, 2.0)
    if lo >= hi:
        return None
    for _ in range(200):
        a, b = lo * (hi / lo) ** (1 / 3), lo * (hi / lo) ** (2 / 3)
        better = response(c, a) < response(c, b) if want_min \
            else response(c, a) > response(c, b)
        if better:
            hi = b
        else:
            lo = a
    return math.sqrt(lo * hi)


#
# One locator per filter type.  Each returns the frequency the filter
# actually turned out to be characteristic at, found from the response.
#
# The two-pole low-pass does NOT have its -3 dB point at w0 - it has it
# there only at Q = 1/sqrt(2), and |H(w0)| is exactly Q.  Using -3 dB
# would have measured the Q instead of the frequency, and reported a
# 20 kHz filter as landing at 67 kHz.  What is true at w0 for every Q is
# the phase: -90 degrees for a low-pass, +90 for a high-pass, -180 for
# the allpass.  The bilinear transform maps w0 exactly, so these hold in
# the digital filter and not merely in the analog prototype.
#
def loc_lpf(c, f0, q, g):
    # Phase runs 0 down to -pi; take it on that branch and find -pi/2.
    def p(f):
        v = phase(c, f)
        return v if v <= 0 else v - 2 * math.pi
    return solve(lambda f: -p(f) - math.pi / 2, *band(f0, 4.0))


def loc_hpf(c, f0, q, g):
    # Phase runs +pi down to 0; take it on [0, 2pi) and find +pi/2.
    def p(f):
        return phase(c, f) % (2 * math.pi)
    return solve(lambda f: -p(f) + math.pi / 2, *band(f0, 4.0))


def loc_notch(c, f0, q, g):
    return extremum(c, f0, True)


def loc_bpf(c, f0, q, g):
    return extremum(c, f0, False)


def loc_allpass(c, f0, q, g):
    # A second-order allpass runs 0 to -2pi; w0 is where it passes -pi.
    def p(f):
        v = phase(c, f)
        return v if v <= 0 else v - 2 * math.pi
    return solve(lambda f: -p(f) - math.pi, *band(f0, 4.0))


def loc_peaking(c, f0, q, g):
    return extremum(c, f0, g < 0)


#
# A shelf reaches half its decibels at w0 by construction.  Which way
# the response is going through that point depends on both which shelf
# it is and the sign of the gain, and getting it wrong makes every case
# unlocatable rather than wrong - which is how the first version of this
# reported "-" for every low shelf it tried.
#
def _shelf(c, f0, g, rising):
    half = g / 2.0
    if rising:
        return solve(lambda f: db(response(c, f)) - half, *band(f0, 8.0))
    return solve(lambda f: half - db(response(c, f)), *band(f0, 8.0))


def loc_loshelf(c, f0, q, g):
    return _shelf(c, f0, g, rising=(g < 0))


def loc_hishelf(c, f0, q, g):
    return _shelf(c, f0, g, rising=(g > 0))


LOCATORS = {
    "lpf": loc_lpf, "hpf": loc_hpf, "notch": loc_notch,
    "bpf": loc_bpf, "bpf_peak": loc_bpf, "allpass": loc_allpass,
    "peaking": loc_peaking, "loshelf": loc_loshelf, "hishelf": loc_hishelf,
}

PLAIN = ["lpf", "hpf", "notch", "bpf", "bpf_peak", "allpass"]
GAINED = ["peaking", "loshelf", "hishelf"]

#
# How close a biquad *can* be placed, and why it is not a flat number.
#
# The constructors carry the frequency as cos(w0), and at the bottom of
# the range that is a float32 just under 1.0 - at 20 Hz, 1-cos(w0) is
# 3.4e-06, so a single ulp is 1.7% of the entire quantity that carries
# the angle.  Recovering w from it halves that: 0.43%.  Nothing built
# this way can do better, whatever the sine table does, and that floor
# falls off fast - it is 0.05% by 60 Hz and invisible above a few
# hundred.
#
# So the bound is that model rather than a percentage read off a run.  A
# flat tolerance loose enough for 20 Hz would be asleep at 1 kHz, where
# the same defect would show up as a hundred times the floor and still
# pass.
#
# HEADROOM is the one empirical number here and it is deliberately not
# fitted tightly: the shelf constructors put cos(w0) through four
# multiply-adds and a reciprocal before it reaches a coefficient, so a
# handful of ulps rather than one.  The floor at the bottom keeps the
# top of the range from being asked for more precision than a float32
# frequency argument can express.
#
HEADROOM = 8.0
FLOOR = 0.05		# percent
ULP = 2.0 ** -24	# of a float32 just below 1.0


def tolerance(f):
    """Percent of f, from what float32 can say about cos(w0)."""
    w = 2 * math.pi * f / FS
    angle_err = (ULP / 2) / math.sin(w)		# radians, one ulp of cos
    return max(FLOOR, HEADROOM * 100.0 * angle_err / w)


def build_cases():
    cases = []
    for t in PLAIN:
        for f in FREQS:
            for q in QS:
                cases.append((t, f, q, 0.0))
    for t in GAINED:
        for f in FREQS:
            for q in QS:
                for g in GAINS:
                    cases.append((t, f, q, g))
    return cases


def main():
    verbose = "-v" in sys.argv
    cases = build_cases()

    stdin = "".join(f"{t} {f!r} {q!r} {g!r}\n" for t, f, q, g in cases)
    r = subprocess.run([COEFF], input=stdin, stdout=subprocess.PIPE,
                       universal_newlines=True)
    if r.returncode:
        sys.exit(f"test-biquad: {COEFF} failed - has 'make bench' been run?")

    lines = r.stdout.strip().split("\n")
    if len(lines) != len(cases):
        sys.exit(f"test-biquad: asked {len(cases)}, got {len(lines)}")

    # worst error as a fraction of what is allowed there, per (type, Q)
    worst = {}
    failures = []
    unlocatable = []

    for line, (t, f0, q, g) in zip(lines, cases):
        p = line.split()
        c = [float(x) for x in p[4:]]
        got = LOCATORS[t](c, f0, q, g)
        if got is None:
            # A shelf of 20 dB at Q 0.5 can be so gentle that half its
            # gain is off the end of the search; that is not a
            # placement error and is not this test's business.
            unlocatable.append((t, f0, q, g))
            continue
        err = 100.0 * (got - f0) / f0
        tol = tolerance(f0)
        key = (t, q)
        if key not in worst or abs(err) / tol > worst[key][0]:
            worst[key] = (abs(err) / tol, err, f0, g)
        if abs(err) > tol:
            failures.append((t, f0, q, g, got, err, tol))

    print(f"test-biquad: {len(cases)} filters, {FREQS[0]:.0f} Hz to "
          f"{FREQS[-1]:.0f} Hz, every semitone\n")
    print(f"worst placement error, as a fraction of what float32 allows "
          f"there\n(1.00 would be exactly at the bound; over 1.00 fails)\n")

    print(f"{'filter':>9} " + "".join(f"{'Q=' + str(q):>12}" for q in QS))
    print("-" * (10 + 12 * len(QS)))
    for t in PLAIN + GAINED:
        row = f"{t:>9} "
        for q in QS:
            e = worst.get((t, q))
            row += f"{e[0]:11.2f} " if e else f"{'-':>12}"
        print(row)

    if unlocatable and verbose:
        print(f"\n{len(unlocatable)} cases had no locatable corner "
              f"(a shelf too gentle to reach half its gain):")
        for t, f0, q, g in unlocatable[:10]:
            print(f"    {t} {f0:.1f} Hz Q={q} {g:+.0f} dB")

    if not failures:
        print("\ntest-biquad: ok")
        return 0

    print(f"\ntest-biquad: {len(failures)} of {len(cases)} outside the "
          f"bound, worst first:")
    failures.sort(key=lambda x: -abs(x[5]) / x[6])
    for t, f0, q, g, got, err, tol in \
            failures[:20 if not verbose else len(failures)]:
        gs = f" {g:+.0f}dB" if t in GAINED else ""
        print(f"    {t:>9} {f0:8.2f} Hz Q={q:<5}{gs:>8}  landed at "
              f"{got:8.2f} Hz  {err:+7.3f}%  (allowed {tol:.3f}%)")
    if not verbose and len(failures) > 20:
        print(f"    ... and {len(failures) - 20} more (-v for all)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
