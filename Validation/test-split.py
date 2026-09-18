#!/usr/bin/env python3
#
# Does per-effect channel routing actually do anything?
#
# The chain is stereo inside even on a mono board - the front of it
# duplicates the input - and an effect can be told which half to read
# and where to put its answer.  That is worth exactly nothing unless
# something checks it, and the difference is inaudible on a mono output
# unless you know what to listen for.
#
# So set up two scenes that differ in one field, and measure:
#
#   scene 0     TONE   in=L out=L        the split: L shaped, R kept
#               TONE 2 in=L out=Merge    the join:  L = tone2(L) + R
#
#   scene 1     TONE   in=L out=L        same split
#               TONE 2 in=L out=L        no join
#
# Both tone stacks are flat, and a shelf at 0dB is exactly transparent
# rather than approximately - the numerator and denominator of the
# section come out identical term by term - so the arithmetic is clean:
#
#   scene 0     L = in + in  =  2 x in   ->  +6.02 dB
#   scene 1     L = in                   ->   0.00 dB
#
# The two must differ by 6dB, and if they do not then either the kept
# channel is not being kept or the merge is not merging.
#
# Everything here goes over SysEx: the pots, the routing, the steering
# and the two saves.  So this also answers whether a scene carries
# steering across a save and a Program Change, which is the other half
# of the feature and is not separately testable.
#
import argparse
import sys

import numpy as np

import audio
import effectmap
import pedal
import pots as P


def configure(p, out):
    """Both tones routed and flat, with TONE 2's output where asked.

    Every pot is written rather than only the steering, because a save
    keeps what the pedal has and what it has is whatever the last
    session left behind.
    """
    tone1, tone2 = effectmap.effect("Tone 1"), effectmap.effect("Tone 2")
    msgs = [(0x08, tone1, tone2)]

    for name in ("Signal Chain", "Tone 1", "Tone 2"):
        eff = effectmap.effect(name)
        for label, raw in P.defaults(name).items():
            msgs.append((0x03, eff, effectmap.pot(name, label), raw))

    for name, eff, where in (("Tone 1", tone1, "Left"), ("Tone 2", tone2, out)):
        msgs.append((0x03, eff, effectmap.MIX, 120))
        msgs.append((0x03, eff, effectmap.pot(name, "In"),
                     P.to_pot(name, "In", "Left")))
        msgs.append((0x03, eff, effectmap.pot(name, "Out"),
                     P.to_pot(name, "Out", where)))

    pedal.send_many(p, *msgs)


def measure(p, card, which):
    pedal.program_change(p, which)
    d = audio.trim(audio.capture(3, card))
    wet, dry = d[:, 0], d[:, 1]
    # The right channel is the raw sample; scale it into the same units
    # the left one is already in - see audio.SAMPLE_TO_FLOAT.
    return audio.ratio_db(wet, dry * audio.SAMPLE_TO_FLOAT), wet, dry


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", default=None,
                    help="serial, label or board name naming one pedal")
    args = ap.parse_args()

    d, why = pedal.sole(args.target)
    if not d:
        print("test-split: SKIPPED - %s" % why)
        return 0
    card, p = d["card"], d["port"]
    if card is None or p is None:
        print("test-split: SKIPPED - %s has no card or no MIDI port"
              % d["label"])
        return 0

    if pedal.capabilities(d)["stereo"] is False:
        print("test-split: SKIPPED - %s is mono; there is no second channel "
              "to keep" % d["label"])
        return 0

    pedal.wet_dry(p, effectmap.settings())

    #
    # Scenes 0 and 1, in the order the measurements want them.  Saving
    # is the ordinary Save Scene command, so this leaves the pedal the
    # way pressing save twice would.
    #
    for which, out in ((0, "Merge"), (1, "Left")):
        configure(p, out)
        pedal.save_scene(p, which)

    merge_db, wet, dry = measure(p, card, 0)
    keep_db, _, _ = measure(p, card, 1)

    if audio.peak(dry) < 1e-6:
        print("test-split: SKIPPED - nothing on the analog input")
        return 0

    print(f"  scene 0, TONE 2 out=Merge : {merge_db:+.2f} dB   (want +6.02)")
    print(f"  scene 1, TONE 2 out=L     : {keep_db:+.2f} dB   (want  0.00)")
    print(f"  difference                : {merge_db - keep_db:+.2f} dB")

    bad = []
    if abs(merge_db - 6.02) > 0.5:
        bad.append("merge is not summing the kept channel at unity")
    if abs(keep_db) > 0.5:
        bad.append("out=L is not passing the shaped channel through")
    if abs((merge_db - keep_db) - 6.02) > 0.5:
        bad.append("the two scenes measure the same - routing does nothing")

    print()
    for b in bad:
        print(f"test-split: FAIL - {b}")
    if not bad:
        print("test-split: ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
