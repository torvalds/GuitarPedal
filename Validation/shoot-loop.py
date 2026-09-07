#!/usr/bin/env python3
#
# Does it sound like the pedal it is a model of?
#
# The measurement question and the listening question are different, and
# this is only the second one.  compare-spice.py and loop.py say whether
# the numbers agree; this plays the same material through the real pedal
# and through the model and puts them side by side, because a model can
# be a decibel out everywhere and indistinguishable, or right on every
# metric here and obviously wrong on a chord.
#
# BOTH LEGS COME OFF THE SAME RIG, WHICH IS THE POINT
#
# The hardware leg routes nothing, so the DAC gets the stimulus and the
# pedal under test answers it.  The model leg routes the effect, so the
# same stimulus is answered by the same board's arithmetic.  Converters,
# cables, clock and stimulus are common to both and cancel; what is left
# is the circuit against its model.
#
# TWO THINGS THAT WILL FOOL A LISTENER, AND WHAT IS DONE ABOUT THEM
#
# The level.  Louder wins every blind test ever run, so the takes are
# matched - but the gain it took is *printed*, because if the model
# needs two decibels to sit level then that is one of the findings and
# not a nuisance to normalise away.
#
# The noise.  The hardware leg carries the analog floor and whatever hum
# the room is putting into two cables; the model leg is arithmetic and
# is silent.  A listener picks the hardware every time on hiss alone,
# having heard nothing about the tone, and that is a false result with a
# confident feel to it.  So the model leg gets the measured noise of the
# other one added to it - captured from the same rig with the stimulus
# silent, so it is that room on that evening and not a generator.
#
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio
import bench as B
import loop
import pedal
import shootout
import targets as T


def noise_of(card, seconds=2.0):
    """What the loop puts out with nothing going into it."""
    cap = audio.capture(int(seconds) + 1, card)
    return cap[:, 1]


def take(p, card, leg, t, knobs, x, tries=3):
    """One leg of the comparison, as (sent, returned)."""
    loop.configure(p, leg, t, knobs)
    last = None
    for _ in range(tries):
        try:
            return loop.play_capture(card, x)
        except loop.LoopError as e:
            last = e
    raise loop.LoopError("%s leg: %s" % (leg, last))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", nargs="?", default="rat")
    ap.add_argument("--input", default="Inputs/Dry-Guitar.wav")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--offset", type=float, default=12.0)
    ap.add_argument("--peak", type=float, default=0.35,
                    help="what the stimulus is normalised to, in the "
                         "pedal's scale - a guitar, not a full-scale sine")
    ap.add_argument("--out", default="rat-shootout")
    ap.add_argument("--knob", action="append", default=[], metavar="NAME=V",
                    help="move one pot off the target's default, to match "
                         "however the pedal on the bench is set")
    ap.add_argument("--blind", action="store_true")
    ap.add_argument("--against", default=None, metavar="REF",
                    help="add a third leg: the same effect built from an "
                         "older commit, so what changed can be heard "
                         "rather than only measured")
    ap.add_argument("--no-dither", action="store_true",
                    help="leave the model leg silent between notes")
    args = ap.parse_args()

    t = T.target(args.target)
    knobs = dict(t["knobs"])
    for kv in args.knob:
        name, _, val = kv.partition("=")
        if name not in knobs:
            sys.exit("%s has no pot %r; it has %s"
                     % (args.target, name, ", ".join(knobs)))
        knobs[name] = int(val) if isinstance(knobs[name], int) else float(val)
    p = pedal.port()
    card = audio.find_card("Pedal")
    print("firmware:", loop.refuse_if_stale(p))

    x, _ = audio.decode(args.input, seconds=args.seconds, offset=args.offset)
    x = x / max(np.abs(x).max(), 1e-9) * args.peak

    print("measuring the loop's own noise")
    floor = noise_of(card)
    print("   %.1f dBFS rms" % (20*np.log10(max(float(np.sqrt(np.mean(floor**2))), 1e-12))))

    takes = []
    print("knobs: " + ", ".join("%s %s" % kv for kv in knobs.items()))
    print("hardware leg")
    _sent, hw = take(p, card, "hardware", t, None, x)
    takes.append(("hardware", "the pedal itself", hw))

    #
    # The model legs are run on the host bench rather than on the pedal.
    # It is the same arithmetic - the bench compiles the pedal's own
    # audio core - and it is the only way to have two *versions* of the
    # effect in one comparison without reflashing between takes.  What
    # it costs is that the model legs have not been through the
    # converters and the hardware leg has; the loop measures -0.28 dB
    # and flat to 0.4 degrees, so that is not what anyone will hear.
    #
    pa = T.pot_args(t, knobs)
    print("model leg, this tree")
    now, _info = B.through(B.BENCH, pa, x.astype(np.float32))
    legs = [("model", "%s, as it is now" % t["short"], np.asarray(now, float))]

    if args.against:
        print("model leg, %s" % args.against)
        old_bin, sha = B.reference_bench(args.against)
        was, _info = B.through(old_bin, pa, x.astype(np.float32))
        legs.append(("before", "%s, at %s" % (t["short"], sha[:8]),
                     np.asarray(was, float)))

    for key, label, y in legs:
        if not args.no_dither:
            #
            # The same room, tiled to length rather than generated: what
            # is wanted is the thing the other leg actually carries.
            #
            y = y + np.resize(floor, len(y))
        takes.append((key, label, y))

    print()
    #
    # Matched to the hardware's level, because that is the thing the
    # other two are claiming to be - and 'ref' is an rms, not a take.
    #
    shootout.publish(takes, args.out,
                     title="%s against the pedal it models" % t["short"],
                     ref=float(np.sqrt(np.mean(hw ** 2))),
                     blind=args.blind, match="rms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
