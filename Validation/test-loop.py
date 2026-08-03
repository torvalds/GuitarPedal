#!/usr/bin/env python3
#
# Two pedals patched into each other, one generating and one measured.
#
# Everything in test-audio.py is measured against a *declared* stimulus:
# the bench generator cannot be set from here, so --ptp and --freq say
# what the dial reads and every result is quoted against that claim.  It
# also cannot be switched off, so whatever noise it contributes is
# counted as the pedal's - which is most of why the noise floor there
# moved several dB between runs for reasons that turned out to be the
# cabling rather than the firmware.
#
# With a pedal at each end the stimulus is set over MIDI, can be turned
# off, and is produced by neither the host nor the pedal being measured.
# That last part is the one that matters for the USB audio work: a tone
# arriving over a patch cable is unaffected by anything the host does to
# the device under test, so when a capture breaks up there is no longer
# any question of whether the generator wobbled.
#
#   ./test-loop.py
#
# Wants both boards on the USB, output to input in both directions.  It
# discovers them by codec, so which is which is not a command-line
# argument and cannot be got the wrong way round; it runs every check in
# both directions instead, because the two boards are not the same and
# the interesting numbers are the ones that differ.
#
# Skipped rather than failed with fewer than two pedals, in the same
# spirit as the rest of the suite.
#
import argparse
import sys
import time

import numpy as np

import audio
import pedal

FAILED = []


def check(name, ok, detail):
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}: {detail}")
    if not ok:
        FAILED.append(name)


def note(name, detail):
    print(f"  --    {name}: {detail}")


#
# The test tone's pots, in SysEx numbering where 0 is the mix.
#
TONE_MIX, TONE_LEVEL, TONE_FREQ, TONE_SHAPE = 0, 1, 2, 3
SHAPE_SINE, SHAPE_NOISE = 0, 3

# Level is LINEAR(-90 0) over 120 steps, so a step is 0.75 dB.
LEVEL_OFF = 0


def level_pot(dbfs):
    return max(0, min(120, int(round((dbfs + 90.0) / 0.75))))


def level_dbfs(pot):
    return -90.0 + pot * 0.75


def tone_id():
    """Which effect the test tone is, by name, out of the built map."""
    import scene
    effects = scene.effects_from_map()
    for i, e in enumerate(effects):
        if e["name"] == "Test Tone":
            return i
    return None


def silence(p, tone):
    """Stop a pedal generating, without unrouting it."""
    pedal.set_pot(p, tone, TONE_LEVEL, LEVEL_OFF)


def generate(p, tone, dbfs, shape=SHAPE_SINE):
    """Make this pedal's output the tone, and nothing else."""
    pedal.set_routing(p, tone)
    pedal.set_pot(p, tone, TONE_MIX, 120)
    pedal.set_pot(p, tone, TONE_SHAPE, shape)
    pedal.set_pot(p, tone, TONE_LEVEL, level_pot(dbfs))


def passthrough(p):
    """Nothing routed, gate open, unity trim and volume."""
    pedal.set_routing(p)
    pedal.set_pot(p, 0, pedal.CHAIN_GATE, 0)     # fully down is off
    pedal.set_pot(p, 0, pedal.CHAIN_TRIM, 60)    # 0 dB
    pedal.set_pot(p, 0, pedal.CHAIN_VOLUME, 80)  # 0 dB


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--level", type=float, default=-30.0,
                    help="generator level, dBFS")
    args = ap.parse_args()

    found = pedal.discover()
    if len(found) < 2:
        print(f"test-loop: SKIPPED - found {len(found)} pedal(s), need 2")
        return 0

    tone = tone_id()
    if tone is None:
        print("test-loop: SKIPPED - no Test Tone effect in the built map")
        return 0

    names = ", ".join(f"{d['codec']} (card {d['card']}, midi {d['port']})"
                      for d in found)
    print(f"test-loop: {names}")

    n = pedal.effect_count()
    for d in found:
        pedal.wet_dry(d["port"], n - 1)

    for src, dst in ((found[0], found[1]), (found[1], found[0])):
        print(f"\n  {src['codec']} -> {dst['codec']}")
        one_direction(src, dst, tone, args)

    print()
    if FAILED:
        print(f"test-loop: {len(FAILED)} failed: {', '.join(FAILED)}")
        return 1
    print("test-loop: all checks pass")
    return 0


def one_direction(src, dst, tone, args):
    #
    # Break the loop first, and prove it is broken before measuring
    # anything.
    #
    # Two pedals patched into each other are an oscillator: noise goes
    # round, gains a little each time, and both of them sit and clip.  A
    # generator at full mix replaces its input rather than adding to it,
    # so routing it on one of the two cuts the loop at a known place -
    # but only if it really is at full mix, and a scene load or a stray
    # pot can leave it otherwise.  So the precondition is checked rather
    # than assumed: with the source silent and the sink passing through,
    # the sink must see nothing.  Anything else means the loop is still
    # closed and every number after it would be measuring the ringing.
    #
    generate(src["port"], tone, args.level)
    passthrough(dst["port"])
    silence(src["port"], tone)
    time.sleep(0.5)

    quiet = audio.capture(args.seconds, dst["card"])
    quiet_in = quiet[:, 1] * audio.SAMPLE_TO_FLOAT
    open_loop = audio.dbfs(audio.rms(quiet_in))
    if open_loop > -40.0:
        check("loop is broken", False,
              f"{open_loop:.1f} dBFS at {dst['codec']}'s input with "
              f"{src['codec']} silent - still oscillating, not measuring")
        return
    check("loop is broken", True,
          f"{open_loop:.1f} dBFS at {dst['codec']}'s input with "
          f"{src['codec']} silent")

    #
    # The noise floor, with the input driven by a real source.
    #
    # This is the measurement a bench generator cannot take.  An open or
    # high-impedance input is a far noisier thing than anything that will
    # ever be plugged into the pedal, and a generator that cannot be
    # switched off contributes whatever it contributes.  Here the input
    # is terminated by another pedal's output stage, which is what a
    # quiet source actually looks like.
    #
    nf = audio.noise_floor(quiet_in)
    note("terminated noise floor",
         f"{nf['noise_dbfs']:.1f} dBFS at {dst['codec']}'s input, "
         f"{audio.rms(quiet_in) * 1e6:.0f} uV rms")

    gate_margin = nf["noise_dbfs"] - (-70.0)
    check("gate default has headroom", gate_margin < -6.0,
          f"-70 dBFS default is {-gate_margin:+.1f} dB above the floor")

    #
    # What the cable does.  One number, and it has never been one.
    #
    # test-audio.py's every result is denominated in a --ptp the operator
    # typed in.  Here the level is set over MIDI and the pedal's own
    # scale is the reference at both ends, so the link is measurable
    # rather than declarable.  Checked across a spread, because a gain
    # that is only right at one level is not a gain.
    #
    gains = []
    for want in (args.level, args.level - 15.0, args.level - 30.0):
        pedal.set_pot(src["port"], tone, TONE_LEVEL, level_pot(want))
        sent = level_dbfs(level_pot(want))
        d = audio.capture(args.seconds, dst["card"])
        seen_in = d[:, 1] * audio.SAMPLE_TO_FLOAT
        got = audio.dbfs(audio.rms(seen_in) * np.sqrt(2))
        gains.append(got - sent)

    spread = max(gains) - min(gains)
    note("link gain",
         f"{np.mean(gains):+.2f} dB {src['codec']} out to {dst['codec']} in, "
         f"spread {spread:.2f} dB over 30 dB")
    check("link is linear", spread < 1.0,
          f"{spread:.2f} dB of spread across a 30 dB range")

    #
    # The frequency, loosely.
    #
    # Deliberately not reported as a clock offset, though that is what a
    # tone generated in one pedal's sample clock and measured in
    # another's would tell you if it could be measured well enough.  It
    # cannot, yet: dominant() interpolates across an unwindowed peak and
    # comes back with errors of several bins that change sign from one
    # frequency to the next - see the issue list.  1% is safe, ppm is
    # not, so this checks that the right tone arrived and says nothing
    # about how right it was.
    #
    pedal.set_pot(src["port"], tone, TONE_LEVEL, level_pot(args.level))
    d = audio.capture(args.seconds, dst["card"])
    seen_in = d[:, 1] * audio.SAMPLE_TO_FLOAT
    f0 = audio.dominant(seen_in)
    check("the tone arrives", abs(f0 / 440.0 - 1.0) < 0.01,
          f"440 Hz generated reads {f0:.2f} Hz at {dst['codec']}, "
          f"{(f0 / 440.0 - 1.0) * 100:+.2f}%")

    #
    # The one the rig was built for.
    #
    # A state dump is about fifteen kilobytes and goes out in a single
    # pass of the main loop, and the audio endpoint is fed at the end of
    # that same loop - so the pedal's own replies are the heaviest thing
    # that ever competes with its audio.  Measuring that needed a
    # stimulus that could not itself be disturbed by the host, which is
    # what the far pedal now is: it is not being asked anything, it is
    # not being captured from, and it does not know the test is running.
    #
    # So a break here is the device under test, and nothing else.
    #
    def hammer():
        for _ in range(12):
            pedal.send(dst["port"], 0x05)   # state dump request

    d = audio.capture(args.seconds, dst["card"], during=hammer)
    seen_in = d[:, 1] * audio.SAMPLE_TO_FLOAT
    breaks = audio.discontinuities(seen_in, f0)
    per_s = len(breaks) / (len(seen_in) / audio.RATE)
    check("audio survives its own sysex", not breaks,
          f"{len(breaks)} breaks in {len(seen_in) / audio.RATE:.1f}s "
          f"({per_s:.1f}/s) while {dst['codec']} answered 12 state dumps")

    silence(src["port"], tone)


if __name__ == "__main__":
    sys.exit(main())
