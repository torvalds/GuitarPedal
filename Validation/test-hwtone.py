#!/usr/bin/env python3
#
# What the codec's own tone stack actually does to a signal.
#
# [INTONE] and [OUTTONE] run in the TAC5112's biquads rather than in the
# audio core, so nothing on the host can check them by arithmetic: the
# coefficients are written over i2c in a 1.31 fixed-point format, scaled
# to fit it, and handed back the difference by the channel's digital
# volume.  Every one of those steps can be wrong in a way that still
# sounds like a tone control.
#
# THE MEASUREMENT
#
# One pedal generates a swept tone into the pedal under test.  The
# record sections sit in front of the ADC, so what the pedal under test
# reads off its own jack has already been through the stack - which is
# why this reads LR_Dry and not the chain's output.
#
# Swept twice, flat and boosted, and the answer is the difference.  The
# cable, both converters, the analog path and the generator's own
# response are in both sweeps and cancel; what is left is the filter.
# That is the whole reason this can claim a hundredth of a decibel on a
# bench with no calibrated anything on it.
#
# WHAT IT IS COMPARED AGAINST
#
# bench/coeff, which is the pedal's own _biquad_*() constructors
# compiled for the host - so a mistake shared by both would not show
# here, and a mistake in the writing, the scaling or the makeup would.
# See the header of bench/coeff.c on why a second implementation would
# be the wrong reference.
#
# TWO LIMITS, BECAUSE THEY CATCH DIFFERENT THINGS
#
#   shape   the whole curve, loosely.  A section written to the wrong
#           page, a sign flipped, a coefficient that overflowed 1.31 -
#           all of those are decibels out, not hundredths.
#   level   the curve away from where it does anything, tightly.  The
#           numerators are divided down to fit and the channel volume
#           puts it back in whole half-decibel steps; if those two do
#           not cancel exactly the whole channel sits at the wrong
#           level, by a different amount for every setting, and the
#           skirts of the curve are where that shows up alone.
#
# The level limit is on the *mean* of the skirt errors, not the worst of
# them.  What it is looking for is an offset shared by every point, and
# averaging is what tells that apart from the scatter: the scatter here
# reaches 0.09 dB on a bad run and averages to about 0.02, while an
# offset survives the averaging intact.  Taking the worst point instead
# leaves no room between the noise and the 0.12 dB the defect this
# catches was actually worth.
#
import argparse
import subprocess
import sys

import numpy as np

import audio
import effectmap
import pedal
import pots as P

RATE = 48000.0

#
# Three bands at once, on purpose.  Each section is divided down only as
# far as it has to be and a later one with room to spare is scaled back
# up by what the earlier ones gave away, so the product over the three
# is what sets the channel's level - a setting that moves one band would
# never exercise it.
#
SETTING = (
    ("loshelf", "Bass",   "Bass Freq",     200.0,  6.0),
    ("peaking", "Mid",    "Mid Freq",      800.0, 12.0),
    ("hishelf", "Treble", "Treble Freq",  3000.0, -6.0),
)
Q = 0.707

SWEEP = range(0, 109, 6)
SHAPE_LIMIT = 0.5       # dB, anywhere
LEVEL_LIMIT = 0.06      # dB, averaged out where the filter does nothing
SKIRT_HZ = (60.0, 6000.0)

failures = []


def check(what, ok, detail=""):
    print("  %-4s %s%s" % ("ok" if ok else "FAIL", what,
                           ": " + detail if detail else ""))
    if not ok:
        failures.append(what)


def note(what, detail):
    print("  --   %s: %s" % (what, detail))


def predicted(freqs):
    """The same three sections, built by the pedal's own constructors."""
    asked = "".join("%s %g %g %g\n" % (kind, f, Q, db)
                    for kind, _, _, f, db in SETTING)
    out = subprocess.run(["bench/coeff"], input=asked,
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("test-hwtone: bench/coeff failed - 'make bench' first")

    z = np.exp(-2j * np.pi * np.asarray(freqs) / RATE)
    h = np.ones_like(z)
    for line in out.stdout.strip().splitlines():
        b0, b1, b2, a1, a2 = (float(x) for x in line.split()[-5:])
        h = h * (b0 + b1*z + b2*z**2) / (1 + a1*z + a2*z**2)
    return 20 * np.log10(np.abs(h))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", help="which pedal carries the stack")
    ap.add_argument("--source", help="which pedal drives it")
    ap.add_argument("--seconds", type=float, default=1.0)
    ap.add_argument("--level", type=float, default=-30.0,
                    help="generator level, dBFS")
    args = ap.parse_args()

    #
    # test-loop.py owns the question of what is plugged into what, and
    # asks it by trying rather than by being told.  Borrowed rather than
    # answered again here: two scripts with two opinions about the patch
    # cables is how a measurement ends up describing the wrong board.
    #
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "test_loop", __file__.replace("test-hwtone.py", "test-loop.py"))
    tl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tl)

    found = pedal.discover()
    if len(found) < 1:
        print("test-hwtone: SKIPPED - no pedal on the USB")
        return 0
    for d in found:
        try:
            d["ids"] = tl.Ids(pedal.schema(d["port"]))
        except Exception as e:
            print("test-hwtone: SKIPPED - %s: %s" % (d["label"], e))
            return 0

    dst, why = pedal.sole(args.target, among=found)
    if dst is None:
        print("test-hwtone: SKIPPED - %s" % why)
        return 0

    #
    # The board answers for itself rather than the build answering for
    # it: 'HW: CODEC_DSP' is a build fact and this is a probe result.
    #
    ident = pedal.identity(dst["port"]) or {}
    if not (ident.get("have") or {}).get("codec_dsp", True):
        print("test-hwtone: SKIPPED - %s has a strapped codec, so there "
              "are no biquads to write" % dst["label"])
        return 0

    #
    # Whoever feeds it, which is itself on a board wired output to input
    # and the other board on a cross-connected pair.
    #
    src = None
    for d in found:
        if args.source and not pedal.matches(args.source, [d]):
            continue
        for e in found:
            tl.mute(e)
        tl.generate(d, args.level, freq=d["ids"].topology_freq)
        L, _ = tl.raw_in(dst, 0.5)
        tl.mute(d)
        if audio.dbfs(audio.rms(L)) > -60.0:
            src = d
            break
    if src is None:
        print("test-hwtone: SKIPPED - nothing is driving %s, so there is "
              "no patch cable into its input" % dst["label"])
        return 0

    print("test-hwtone: %s carries the stack, %s drives it"
          % (dst["label"], src["label"] if src is not dst else "itself"))

    INTONE = effectmap.effect("INTONE")
    pot = lambda label: effectmap.pot("INTONE", label)

    #
    # The routing carries the generator too when it is this same board,
    # because a routing message says the whole chain and would otherwise
    # switch the tone off at the first sweep.
    #
    route = (0x08, INTONE) if src is not dst else \
        (0x08, src["ids"].tone, INTONE)

    def stack(boosted):
        msgs = [route]
        for _, gain_label, freq_label, freq, db in SETTING:
            msgs.append((0x03, INTONE, pot(gain_label),
                         P.to_pot("INTONE", gain_label, db if boosted else 0.0)))
            msgs.append((0x03, INTONE, pot(freq_label),
                         P.to_pot("INTONE", freq_label, freq)))
        msgs.append((0x03, INTONE, pot("Mid Q"), P.to_pot("INTONE", "Mid Q", Q)))
        pedal.send_many(dst["port"], *msgs)

    def sweep():
        out = []
        for p in SWEEP:
            pedal.set_pot(src["port"], src["ids"].tone, src["ids"].freq, p)
            L, _ = tl.raw_in(dst, args.seconds)
            out.append((P.value("Test Tone", "Freq", p),
                        audio.dbfs(audio.rms(L))))
        return out

    tl.passthrough(dst)
    pedal.set_chain_enabled(dst["port"], True)
    tl.generate(src, args.level)

    stack(False)
    flat = sweep()
    stack(True)
    boosted = sweep()
    tl.mute(src)

    freqs = np.array([f for f, _ in flat])
    got = np.array([b - a for (_, a), (_, b) in zip(flat, boosted)])
    want = predicted(freqs)
    err = got - want

    print()
    print("     freq   measured  predicted     error")
    for f, g, w, e in zip(freqs, got, want, err):
        print("  %7.0f  %+9.2f  %+9.2f  %+8.2f" % (f, g, w, e))
    print()

    note("what was asked for",
         ", ".join("%s %+g dB at %g Hz" % (g, db, f)
                   for _, g, _, f, db in SETTING))
    check("the curve is the one asked for", np.max(np.abs(err)) < SHAPE_LIMIT,
          "worst %.2f dB against a %.1f dB limit"
          % (np.max(np.abs(err)), SHAPE_LIMIT))

    #
    # Out on the skirts the sections are doing almost nothing, so an
    # error there is the channel's level rather than its shape - which
    # is what the numerator scaling and the volume makeup have to cancel
    # to.  They did not, once: the volume rounded to the nearest half
    # decibel while the scaling did not, and the whole curve sat 0.12 dB
    # high.
    #
    skirt = (freqs < SKIRT_HZ[0]) | (freqs > SKIRT_HZ[1])
    offset = float(np.mean(err[skirt]))
    note("scatter on the skirts", "worst %.2f dB of %d points"
         % (np.max(np.abs(err[skirt])), int(np.count_nonzero(skirt))))
    check("and it is at the level asked for", abs(offset) < LEVEL_LIMIT,
          "%+.3f dB averaged below %g Hz and above %g Hz, against a "
          "%.2f dB limit" % (offset, SKIRT_HZ[0], SKIRT_HZ[1], LEVEL_LIMIT))

    print()
    print("test-hwtone: " + ("all checks pass" if not failures
                             else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
