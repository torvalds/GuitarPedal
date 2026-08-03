#!/usr/bin/env python3
#
# Pedals patched into each other in a ring, one generating and the rest
# measured.
#
# Everything in test-audio.py is measured against a *declared* stimulus:
# the bench generator cannot be set from here, so --ptp and --freq say
# what the dial reads and every result is quoted against that claim.  It
# cannot be switched off either, so its noise is counted as the pedal's -
# and that turned out to matter enormously, because a bench generator is
# 22.7 dB noisier than a pedal output driving the same converter.
#
# With a pedal at each end the stimulus is set over MIDI, can be turned
# off, and is produced by neither the host nor the board being measured.
# A break in a capture is then the device under test and nothing else.
#
#   ./test-loop.py
#
# Wants at least two pedals wired output to input.  It works out which
# way round they are rather than being told, so there is nothing to keep
# in step with the patch cables and no argument to get backwards.
#
# Skipped rather than failed with fewer than two, in the same spirit as
# the rest of the suite.
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


# The test tone's pots, in SysEx numbering where 0 is the mix.
TONE_MIX, TONE_LEVEL, TONE_FREQ, TONE_SHAPE = 0, 1, 2, 3
SHAPE_SINE, SHAPE_NOISE = 0, 3
LEVEL_OFF = 0

# Freq is EXPONENTIAL(13.75 14080) over 120 steps: ten octaves, twelve
# steps each, so a step is a semitone and these are octaves.
#
# From the bottom of the pot rather than from 55 Hz, which is a guitar's
# low E and not the bottom of what gets plugged in: a four-string bass
# reaches 41.2 Hz and a five-string 30.9 Hz, which is exactly where the
# roll-off starts and where the measurement used to stop.  The step has
# to stay a multiple of twelve so that 440 Hz - the point everything is
# reported against - is one of the points.
SWEEP_LOW_POT, SWEEP_HIGH_POT = 0, 108
FREQ_440_POT = 60

#
# The tone topology detection uses, deliberately not 440 Hz.
#
# A bench signal generator wired to one board's right input is on all the
# time and cannot be switched off from here, so "is there energy on this
# input" is not a question that distinguishes it from a patch cable.  220
# Hz - one octave down, pot 48 - can be told apart from it, and every
# channel is then checked for the frequency rather than for a level.
#
TOPOLOGY_FREQ_POT, TOPOLOGY_FREQ_HZ = 48, 220.0

# usb_output enum
LR_WET, LR_DRY, LR_WETDRY = 1, 2, 3

#
# Channel steering, and the defaults every helper below re-asserts.
#
# Not paranoia: an effect left on CH_OUT_MERGE does not mute when its
# level is taken to zero, because a merge adds the channel it kept -
# 'dry * src + wet * here + merge * kept' is still 'kept' when 'here' is
# silence.  So a stale steering setting quietly turns mute() into a
# pass-through, and then the topology says one pedal feeds two others.
#
POT_CH_IN, POT_CH_OUT, POT_MERGE = 11, 12, 13
STRAIGHT = ((0x03, None, POT_CH_IN, 0),
            (0x03, None, POT_CH_OUT, 0),
            (0x03, None, POT_MERGE, 120))


def straight(eff):
    return tuple((c, eff, pot, val) for c, _, pot, val in STRAIGHT)

# Room for a trip around a ring of pedals.
MAX_LAG = audio.RATE // 10

SETTINGS = None
TONE = None


def level_pot(dbfs):
    return max(0, min(120, int(round((dbfs + 90.0) / 0.75))))


def level_dbfs(pot):
    return -90.0 + pot * 0.75


def tone_id():
    import scene
    for i, e in enumerate(scene.effects_from_map()):
        if e["name"] == "Test Tone":
            return i
    return None


#
# Which USB output mode each pedal is already in.
#
# Setting it is a sysex and a settle, and the mode changes far less often
# than it is asked for - a sweep asks for the same one at every point.
# Remembering it takes about forty of those out of a run.
#
_usb_mode = {}


def usb_mode(d, mode):
    if _usb_mode.get(d["serial"]) == mode:
        return
    pedal.set_pot(d["port"], SETTINGS, pedal.SETTINGS_USB_OUT, mode)
    _usb_mode[d["serial"]] = mode
    time.sleep(0.4)


#
# Each of these is one invocation of aplaymidi rather than one per
# parameter, which is worth about two seconds a message - see pedal.py
# for why sending them back to back is safe.
#
def generate(d, dbfs, shape=SHAPE_SINE, freq=FREQ_440_POT):
    """Make this pedal's output the tone, and nothing else.

    At full mix the tone replaces the input rather than adding to it, so
    this also cuts the ring at this pedal - which is what stops the whole
    thing being an oscillator.
    """
    pedal.send_many(d["port"],
                    (0x08, TONE),
                    (0x03, TONE, TONE_MIX, 120),
                    (0x03, TONE, TONE_SHAPE, shape),
                    (0x03, TONE, TONE_FREQ, freq),
                    (0x03, TONE, TONE_LEVEL, level_pot(dbfs)),
                    *straight(TONE))


def mute(d):
    """Full mix, no level: digital silence out, and the input ignored."""
    pedal.send_many(d["port"],
                    (0x08, TONE),
                    (0x03, TONE, TONE_MIX, 120),
                    (0x03, TONE, TONE_LEVEL, LEVEL_OFF),
                    *straight(TONE))


def passthrough(d):
    pedal.send_many(d["port"],
                    (0x08,),                             # nothing routed
                    (0x03, 0, pedal.CHAIN_GATE, 0),      # fully down is off
                    (0x03, 0, pedal.CHAIN_TRIM, 60),     # 0 dB
                    (0x03, 0, pedal.CHAIN_VOLUME, 80))   # 0 dB


def raw_in(d, seconds):
    """Both raw ADC channels, in the internal float scale.

    LR_Dry rather than Wet/Dry, because what is wanted here is what
    arrived at the jacks and not what the chain made of it - the chain
    copies left over right before anything else runs (issue 56), so
    anything asked after that point cannot see a second channel.
    """
    usb_mode(d, LR_DRY)
    x = audio.capture(seconds, d["card"])
    return x[:, 0] * audio.SAMPLE_TO_FLOAT, x[:, 1] * audio.SAMPLE_TO_FLOAT


def main():
    global SETTINGS, TONE

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--level", type=float, default=-30.0,
                    help="generator level, dBFS")
    args = ap.parse_args()

    found = pedal.discover()
    if len(found) < 2:
        print(f"test-loop: SKIPPED - found {len(found)} pedal(s), need 2")
        return 0

    TONE = tone_id()
    n = pedal.effect_count()
    if TONE is None or not n:
        print("test-loop: SKIPPED - no Test Tone effect in the built map")
        return 0
    SETTINGS = n - 1

    print("test-loop: " + ", ".join(
        "%s (card %s, midi %s)" % (d["label"], d["card"], d["port"])
        for d in found))

    ring = topology(found, args)
    if not ring:
        return 1

    print()
    for src, dst in ring:
        print("  %s -> %s" % (src["label"], dst["label"]))
        one_edge(src, dst, args)

    print()
    latency(ring, found, args)

    print()
    for d in found:
        mute(d)
    if FAILED:
        print(f"test-loop: {len(FAILED)} failed: {', '.join(FAILED)}")
        return 1
    print("test-loop: all checks pass")
    return 0


def topology(found, args):
    """Work out what is plugged into what, by trying it.

    Every pedal is muted first - full mix and no level, so each one puts
    out digital silence and ignores its input.  That stops anything
    propagating past the board it reaches, so when one pedal is then made
    to generate, the only pedal that can hear it is the one its output is
    physically wired to.

    Which makes this a wiring check as much as a setup step: it reports
    the patch cables rather than believing anything about them.
    """
    print()
    print("  topology")
    for d in found:
        mute(d)
    time.sleep(1.0)

    nxt = {}
    for src in found:
        generate(src, args.level, freq=TOPOLOGY_FREQ_POT)
        time.sleep(0.8)
        heard = []
        for dst in found:
            if dst is src:
                continue
            for ch, x in zip(("L", "R"), raw_in(dst, 1.0)):
                lvl = audio.dbfs(audio.rms(x))
                if lvl < -60.0:
                    continue
                f0 = audio.dominant(x)
                if abs(f0 - TOPOLOGY_FREQ_HZ) > 0.05 * TOPOLOGY_FREQ_HZ:
                    continue
                heard.append((lvl, dst, ch))
        mute(src)

        if len({d["serial"] for _, d, _ in heard}) == 1:
            lvl, dst, chans = heard[0][0], heard[0][1], \
                "".join(sorted(c for _, _, c in heard))
            nxt[src["serial"]] = dst
            note("%s feeds" % src["label"],
                 "%s %s at %.1f dBFS" % (dst["label"], chans, lvl))
        elif not heard:
            note("%s feeds" % src["label"], "nothing - output not connected?")
        else:
            note("%s feeds" % src["label"], "several: " +
                 ", ".join("%s %s %.1f dBFS" % (d["label"], c, l)
                           for l, d, c in heard))

    edges = [(s, nxt[s["serial"]]) for s in found if s["serial"] in nxt]
    check("every pedal feeds exactly one other", len(edges) == len(found),
          "%d of %d links found" % (len(edges), len(found)))
    if len(edges) != len(found):
        print("  (an incomplete ring - measuring the links that exist)")
    return edges


def one_edge(src, dst, args):
    #
    # Cut the ring here and prove it is cut before measuring anything.
    #
    # Pedals patched into each other oscillate: noise goes round, gains a
    # little each time, and they all sit and clip.  A generator at full
    # mix cuts that at a known place - but only if it really is at full
    # mix, so the precondition is measured rather than assumed.
    #
    mute(src)
    passthrough(dst)
    time.sleep(0.6)

    quiet_L, quiet_R = raw_in(dst, args.seconds)
    open_loop = audio.dbfs(audio.rms(quiet_L))
    if open_loop > -40.0:
        check("loop is cut", False,
              "%.1f dBFS into %s with %s silent - still ringing, not measuring"
              % (open_loop, dst["label"], src["label"]))
        return
    check("loop is cut", True,
          "%.1f dBFS into %s with %s silent" % (open_loop, dst["label"],
                                                src["label"]))

    #
    # The noise floor with the input driven by a real source.
    #
    # An open or high-impedance input is far noisier than anything that
    # will ever be plugged into a pedal, and a bench generator that
    # cannot be switched off contributes whatever it contributes - 22.7
    # dB of it, measured.  Here the input is terminated by another
    # pedal's output stage, which is what a quiet source looks like.
    #
    nf = audio.noise_floor(quiet_L)
    note("terminated noise floor",
         "%.1f dBFS at %s, %.0f uV rms"
         % (nf["noise_dbfs"], dst["label"], audio.rms(quiet_L) * 1e6))
    check("gate default has headroom", nf["noise_dbfs"] - (-70.0) < -6.0,
          "-70 dBFS default is %+.1f dB above the floor"
          % (-70.0 - nf["noise_dbfs"]))

    # What the cable does, across a spread - a gain right at one level
    # only is not a gain.
    gains = []
    for want in (args.level, args.level - 15.0, args.level - 30.0):
        generate(src, want)
        sent = level_dbfs(level_pot(want))
        L, _ = raw_in(dst, args.seconds)
        gains.append(audio.dbfs(audio.rms(L) * np.sqrt(2)) - sent)

    spread = max(gains) - min(gains)
    note("link gain", "%+.2f dB, spread %.2f dB over 30 dB"
         % (float(np.mean(gains)), spread))
    check("link is linear", spread < 1.0,
          "%.2f dB of spread across a 30 dB range" % spread)

    # The response of the analog path, in octaves.  Reported against the
    # 440 Hz point: the absolute level is the link gain, measured above,
    # and what is wanted here is the shape.
    generate(src, args.level)
    resp = []
    for pot in range(SWEEP_LOW_POT, SWEEP_HIGH_POT + 1, 12):
        pedal.set_pot(src["port"], TONE, TONE_FREQ, pot)
        L, _ = raw_in(dst, args.seconds)
        resp.append((13.75 * 2 ** (pot / 12.0), audio.dbfs(audio.rms(L))))

    ref = dict((round(f), db) for f, db in resp).get(440)
    note("frequency response",
         "  ".join("%.0fHz %+.1f" % (f, db - ref) for f, db in resp))
    mid = [db - ref for f, db in resp if 100.0 <= f <= 4000.0]
    check("response is flat in the middle", max(mid) - min(mid) < 3.0,
          "%.1f dB across 110 Hz to 3520 Hz" % (max(mid) - min(mid)))

    pedal.set_pot(src["port"], TONE, TONE_FREQ, FREQ_440_POT)
    mute(src)


def latency(ring, found, args):
    """How long the whole ring takes.

    The generating pedal is at full mix, so its output ignores its input
    and the ring is open at that end - which means its own capture holds
    both halves of the journey.  In Wet/Dry the left channel is what it
    sent and the right is what came back to it, both sampled by the same
    converter on the same clock, so the lag between them needs no
    alignment to find.  Two captures could not do this: they start at
    unrelated instants and that difference dwarfs the delay.

    Noise rather than a tone, because a sine correlates with itself every
    cycle and the answer would be a cycle count.
    """
    if len(ring) != len(found):
        note("ring latency", "skipped - the ring is not closed")
        return

    src = ring[0][0]
    for d in found:
        if d is not src:
            passthrough(d)
    generate(src, args.level, shape=SHAPE_NOISE)
    usb_mode(src, LR_WETDRY)
    time.sleep(0.8)

    d = audio.capture(args.seconds, src["card"])
    sent, back = d[:, 0], d[:, 1] * audio.SAMPLE_TO_FLOAT
    lag = audio.delay_samples(sent, back, MAX_LAG)
    ms = lag * 1000.0 / audio.RATE
    note("ring latency", "%.2f ms round the whole ring of %d, %.0f samples"
         % (ms, len(found), lag))
    note("per pedal", "%.2f ms average" % (ms / len(found)))
    check("ring latency is a real delay", 0.05 < ms < 90.0, "%.2f ms" % ms)

    #
    # The one the rig was built for.  A state dump is fifteen kilobytes
    # in one pass of the main loop, and the audio endpoint is fed at the
    # end of that same loop, so the pedal's own replies are the heaviest
    # thing that competes with its audio.  Measuring it needed a stimulus
    # that could not itself be disturbed by the host, which is what the
    # far pedal is: not captured from, not asked anything, and unaware
    # the test is running.
    #
    dst = ring[0][1]
    generate(src, args.level)
    usb_mode(dst, LR_DRY)
    time.sleep(0.6)

    def hammer():
        for _ in range(12):
            pedal.send(dst["port"], 0x05)

    cap = audio.capture(args.seconds, dst["card"], during=hammer)
    seen = cap[:, 0] * audio.SAMPLE_TO_FLOAT
    breaks = audio.discontinuities(seen, audio.dominant(seen))
    per_s = len(breaks) / (len(seen) / audio.RATE)
    check("audio survives its own sysex", not breaks,
          "%d breaks in %.1fs (%.1f/s) while %s answered 12 state dumps"
          % (len(breaks), len(seen) / audio.RATE, per_s, dst["label"]))


if __name__ == "__main__":
    sys.exit(main())
