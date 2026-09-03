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

#
# The two notes that decide whether this is a bass problem: the bottom
# string of a five-string and of a four-string.
#
LOW_B, LOW_E = 30.87, 41.20

#
# A regression guard rather than a target.  The boards measured so far
# sit between 35 and 44 Hz, and the next generation is expected around
# 14 to 27 Hz - so this catches a board that got worse without pretending
# to assert a number nobody has agreed to yet.  Tighten it when there is
# something to tighten it against.
#
CORNER_LIMIT_HZ = 50.0

# Everything measured, for whatever wants to draw it later.
RESULTS = {"links": []}


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
    ap.add_argument("--json", metavar="PATH",
                    help="write every measurement here, for plotting")
    #
    # The sysex load, as knobs rather than a constant.
    #
    # An attempt at the sysex/audio contention once fixed the light case
    # and made the heavy one worse, and that was only caught because both
    # happened to get run.  A load that cannot be varied from the command
    # line is a load that gets measured at one point and argued about
    # everywhere else.
    #
    ap.add_argument("--load", choices=sorted(LOADS), default="dump",
                    help="which reply to ask for.  'schema' is the big-message "
                         "case: 15700 bytes in one sysex message, with no "
                         "message boundary in it to resume from.  'dump' is "
                         "140 bytes in 8 and is the control")
    ap.add_argument("--dumps", type=int, default=12,
                    help="replies to ask for during the capture")
    ap.add_argument("--dump-interval", type=float, default=0.0,
                    help="extra seconds between requests; pedal.send() "
                         "already settles 60ms of its own")
    ap.add_argument("--note", metavar="TEXT", action="append", default=[],
                    help="what the bench looked like for this run - cables, "
                         "what else was plugged in, anything that moved since "
                         "last time.  Repeatable, stored in the json - "
                         "nothing else records any of it.")
    args = ap.parse_args()

    found = pedal.discover()
    if len(found) < 2:
        print(f"test-loop: SKIPPED - found {len(found)} pedal(s), need 2")
        return 0

    TONE = tone_id()
    SETTINGS = pedal.settings_effect()
    if TONE is None or SETTINGS is None:
        print("test-loop: SKIPPED - no Test Tone effect in the built map")
        return 0

    print("test-loop: " + ", ".join(
        "%s (card %s, midi %s)" % (d["label"], d["card"], d["port"])
        for d in found))

    ring = topology(found, args)
    if not ring:
        return 1

    print()
    for src, dst in ring:
        print("  %s -> %s" % (src["label"], dst["label"]))
        one_edge(src, dst, found, args)

    print()
    latency(ring, found, args)

    if args.json:
        import json
        RESULTS["pedals"] = [{k: d[k] for k in ("label", "serial", "product")}
                             for d in found]
        RESULTS["ring"] = [[a["label"], b["label"]] for a, b in ring]
        RESULTS["recorded"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        RESULTS["operator_notes"] = args.note

        #
        # Written into every file rather than left in this source, on the
        # grounds that the file is what somebody will be looking at in
        # six months and the source is not.  The first of these wants
        # replacing with a bench block the run fills in for itself, so
        # that a measurement carries its conditions instead of a warning
        # that it does not.
        #
        RESULTS["caveats"] = [
            "These are bench measurements, not board specifications. "
            "Nothing here records the conditions that produced them: which "
            "cable was on which link and whether it was TS, TRS or a Y "
            "pair, what else was plugged into the boards, which USB ports "
            "and hubs they were on, or what the room was radiating.",

            "noise_floor_dbfs and noise_uv_rms are the most "
            "environment-sensitive numbers in this file. They measure the "
            "board, the cable, its connectors, the ground path between the "
            "two boards and the supply feeding both, and they cannot "
            "separate them. A 12 dB move has already been observed from a "
            "cable change alone.",

            "A difference between two runs is a hypothesis. Promote it by "
            "moving the one thing that changed and seeing whether the "
            "number follows - re-running on an unchanged bench only "
            "confirms the bench did not move.",

            "corner_hz tracks the receiving board and has so far been "
            "stable against cabling, so it is the safer number to compare "
            "across runs. It is still a fit: read fit_residual_db before "
            "quoting it.",
        ]
        with open(args.json, "w") as f:
            json.dump(RESULTS, f, indent=1)
        print("\n  measurements written to %s" % args.json)
        print("  read the 'caveats' in it before comparing runs")

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


def highpass_db(f, fc):
    """A single pole, in dB."""
    return 20.0 * np.log10(f / np.sqrt(f * f + fc * fc))


def fit_corner(points, ref_db):
    """The single-pole corner that best explains a sweep.

    Fitted over the bottom of the range only.  Above a few hundred hertz
    every candidate curve is flat to a hundredth of a decibel, so those
    points carry no information about the corner and would only dilute
    the residual that says whether one pole is the right model at all.
    """
    f = np.array([p[0] for p in points])
    db = np.array([p[1] - ref_db for p in points])
    m = f <= 1000.0
    grid = np.arange(1.0, 300.0, 0.05)
    err = [np.sum((highpass_db(f[m], fc) - db[m]) ** 2) for fc in grid]
    fc = float(grid[int(np.argmin(err))])
    resid = float(np.max(np.abs(highpass_db(f[m], fc) - db[m])))
    return fc, resid


def one_edge(src, dst, found, args):
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
    # ---------------------------------------------------------------
    # READ THIS BEFORE BELIEVING A CHANGE IN THIS NUMBER.
    #
    # This is the most environment-sensitive figure the suite produces,
    # and the environment is not recorded anywhere.  It measures the
    # board, the cable, the two connectors at each end of the cable, the
    # ground path between the boards, the USB supply feeding both, and
    # whatever the bench is radiating - and it cannot separate them.
    #
    # It has already moved 12 dB on a cable change: on 2026-08-04 the
    # link between the two TAC5242s read 9 uV against 35-36 uV on the
    # other two links, after that one link was rewired from a pair of
    # TRS-to-TS Y cables to a single TRS patch cable.  Removing a second
    # ground path between two boards is a real mechanism for that, and
    # so is "that board's input is just quieter", and this measurement
    # cannot tell them apart.
    #
    # So a difference here is a hypothesis, not a result.  The way to
    # promote it is to move the one thing that changed and see whether
    # the number follows it - not to run this again and get the same
    # answer, which only says the bench did not move either.
    # ---------------------------------------------------------------
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

    #
    # The low end as one number, so a board can be compared with a board.
    #
    # It is a single pole because the measurement says so - one coupling
    # capacitor into the codec's own input impedance dominates, and
    # everything else in the path is three decades away (issue 91).  The
    # residual is reported alongside because it is what would say if that
    # stopped being true.
    #
    fc, resid = fit_corner(resp, ref)
    note("low corner", "%.1f Hz, one pole, worst residual %.2f dB" % (fc, resid))
    note("what a bass sees",
         "low B %.2f dB, low E %.2f dB"
         % (highpass_db(LOW_B, fc), highpass_db(LOW_E, fc)))
    check("low corner has not regressed", fc < CORNER_LIMIT_HZ,
          "%.1f Hz against a %.0f Hz limit" % (fc, CORNER_LIMIT_HZ))

    RESULTS["links"].append({
        "src": src["label"], "dst": dst["label"],
        "response": [[f, db - ref] for f, db in resp],
        "corner_hz": fc, "fit_residual_db": resid,
        "low_b_db": highpass_db(LOW_B, fc), "low_e_db": highpass_db(LOW_E, fc),
        "link_gain_db": float(np.mean(gains)),
        "gain_spread_db": spread,
        "noise_floor_dbfs": nf["noise_dbfs"],
        "noise_uv_rms": audio.rms(quiet_L) * 1e6,
    })

    pedal.set_pot(src["port"], TONE, TONE_FREQ, FREQ_440_POT)
    stereo(src, dst, found, args)
    mute(src)


def stereo(src, dst, found, args):
    """Which channels the cable carries, and whether it crossed them.

    Steering (92) is what makes this askable.  The chain copies left over
    right before anything else runs, so both of a pedal's output channels
    normally carry the same thing - and a crossed pair is then
    indistinguishable from a straight one, because there is nothing to
    tell apart.  Steering the generator to one side at a time puts
    something different on each, which is exactly the question.

    Everything but the source is silenced first, so the channel that is
    not being driven is carrying nothing rather than carrying the rest of
    the ring.

    Including the destination - which *is* the rest of the ring when there
    are only two.  Leaving it in passthrough feeds the source's output back
    into the source's own input, and the chain's left-over-right copy (56)
    then puts it on the channel that is supposed to be silent, arriving
    louder than the driven one because it has been round the link and
    picked up the gain.  Two correctly wired boards failed this in both
    directions at -2.9 dB; muting the far end gives 81 dB.  It costs the
    measurement nothing, because the reading is LR_Dry - what arrives at
    the jacks, whatever the board does with it afterwards.
    """
    for d in found:
        if d is not src:
            mute(d)

    #
    # At the topology frequency and measured only there, for the reason
    # that frequency exists: a bench generator wired to one board's right
    # input is on all the time, and a plain level reading calls that a
    # carried channel and then calls the link crossed because it happens
    # to be a decibel louder than the tone.  Which it duly did.
    #
    seen = {}
    for side, out in (("L", 1), ("R", 2)):     # CH_OUT_LEFT, CH_OUT_RIGHT
        generate(src, args.level, freq=TOPOLOGY_FREQ_POT)
        pedal.send_many(src["port"], (0x03, TONE, POT_CH_OUT, out))
        time.sleep(0.3)
        L, R = raw_in(dst, args.seconds)
        seen[side] = (audio.tone_level(L, TOPOLOGY_FREQ_HZ),
                      audio.tone_level(R, TOPOLOGY_FREQ_HZ))
    mute(src)

    #
    # Steering has to have done something, or none of this means
    # anything.  Firmware without it ignores the write, the tone goes to
    # both channels both times, and the two readings come back identical
    # - at which point "drove R, saw it on L" is true and says nothing
    # about the cable.  Skipped rather than answered, for the same reason
    # check-hw skips an input that is not what was declared.
    #
    same = (abs(seen["L"][0] - seen["R"][0]) < 1.0 and
            abs(seen["L"][1] - seen["R"][1]) < 1.0)
    if same:
        note("stereo", "skipped - %s ignores channel steering, so both "
                       "sides carry the same thing" % src["label"])
        return

    #
    # A channel is "carried" if driving it puts something there well above
    # what the link is doing when it is quiet.
    #
    carried = [s for s, (l, r) in (("L", seen["L"]), ("R", seen["R"]))
               if max(l, r) > -60.0]
    note("channels carried", ", ".join(carried) if carried else "neither")

    #
    # Crossed is a different question from missing, and only answerable
    # for a channel that arrives at all: driving L has to show up on L.
    # A mono cable carries one channel and is not crossed, so it passes.
    #
    crossed = []
    for side, (l, r) in seen.items():
        here, there = (l, r) if side == "L" else (r, l)
        if max(l, r) > -60.0 and there > here:
            crossed.append(side)
    check("channels are not crossed", not crossed,
          "; ".join("drove %s, saw L %.1f R %.1f" % (s, seen[s][0], seen[s][1])
                    for s in ("L", "R")))

    if len(carried) == 2:
        sep = min(seen["L"][0] - seen["L"][1], seen["R"][1] - seen["R"][0])
        note("channel separation", "%.1f dB, the worse of the two" % sep)


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
    RESULTS["latency"] = {"ms": ms, "samples": float(lag), "pedals": len(found)}
    note("per pedal", "%.2f ms average" % (ms / len(found)))
    check("ring latency is a real delay", 0.05 < ms < 90.0, "%.2f ms" % ms)

    #
    # The one the rig was built for.  A reply is turned into four-byte
    # MIDI packets in a single pass of the main loop, and the audio
    # endpoint is fed at the end of that same loop, so the pedal's own
    # replies are the heaviest thing that competes with its audio.
    # Measuring it needed a stimulus that could not itself be disturbed by
    # the host, which is what the far pedal is: not captured from, not
    # asked anything, and unaware the test is running.
    #
    sysex_load(src, ring[0][1], args)


#
# The end of a state dump, on the wire.
#
# The routing message is sent last and exactly once per dump - the
# firmware says so and the web app already relies on it - so counting
# these counts dumps that were actually answered.  Verified by capturing
# one dump and counting: one 0x08 in it, wherever the rest of the dump
# happened to be that week.
#
# Bytes *of sysex*, which is not the same as bytes seen.  The pedal
# streams its status CCs the whole time, so a raw count off a capture
# window includes however much of that stream the window was open for.
#
DUMP_END = bytes([0xF0, 0x7D, 0x08])

#
# The two loads, and why the big one is not a synthetic message.
#
# The schema *is* the stress case and always was: 15700 bytes in one
# single SysEx message, so there is not a message boundary anywhere in it
# to resume from.  Nothing had to be built to test a big reply, because
# the pedal already sends the biggest one it will ever send.
#
# The state dump is the light load and has become much lighter - 140
# bytes in 8 messages, from 1184 in 162, once it stopped sending pots for
# effects that are not routed and started packing an effect's pots into
# one message.  It no longer breaks any audio at all, which is what makes
# it useful as the control: a run where *both* loads look clean is a run
# where something is wrong with the bench rather than right with the
# pedal.
#
LOADS = {
    # name:     (request opcode, the reply that says one arrived)
    "dump":     (0x05, bytes([0xF0, 0x7D, 0x08])),
    "schema":   (0x01, bytes([0xF0, 0x7D, 0x02])),
}


def sysex_load(src, dst, args):
    """Audio breaks while the pedal is answering its own sysex.

    Reported per *answered* dump rather than per second, and that is the
    whole point of the shape of this function.

    The load is delivered by asking for state dumps, and the pedal is
    free to not answer: today it blocks until it has, but the fix this
    test exists to score gives it a queue, and a queue that is full drops
    what will not fit.  A test that counted the requests it *sent* would
    then see the load quietly evaporate and call it a pass - the fix
    would appear to work by breaking the instrument.  So the denominator
    is measured on the wire, and a run that got fewer dumps than it asked
    for says so instead of scoring it.

    The listener runs inside the capture rather than around it: it has to
    be up before the first request goes out, and audio.capture() calls
    'during' in a thread once arecord is actually recording, which is the
    only moment that is true of.
    """
    generate(src, args.level)
    usb_mode(dst, LR_DRY)
    time.sleep(0.6)

    opcode, marker = LOADS[args.load]

    def request_dumps():
        #
        # Paced rather than fired in a burst.  pedal.send() already
        # settles for 60ms of its own, so this is mostly explicit about
        # something that was happening by accident - but it is the pacing
        # that makes "requested" and "answered" comparable numbers, so it
        # should not be an accident.
        #
        for i in range(args.dumps):
            pedal.send(dst["port"], opcode)
            if args.dump_interval > 0 and i + 1 < args.dumps:
                time.sleep(args.dump_interval)

    def one_pass(listening):
        seen_midi = []
        if listening:
            def load():
                seen_midi.extend(pedal.midi_listen(dst["port"], seconds=0.4,
                                                   during=request_dumps))
        else:
            load = request_dumps
        cap = audio.capture(args.seconds, dst["card"], during=load)
        x = cap[:, 0] * audio.SAMPLE_TO_FLOAT
        return (len(audio.discontinuities(x, audio.dominant(x))),
                bytes(seen_midi).count(marker),
                len(x) / audio.RATE)

    #
    # Unread first, then drained, then read.
    #
    # In that order because the unread pass leaves a mess: with nothing
    # reading MIDI IN the replies pile up inside the pedal, and the moment
    # a listener opens the device the backlog floods out and lands in
    # whatever is being measured next.  That is not hypothetical - it
    # showed up as 16 dumps answered against 12 requested.  So the pass
    # that creates the backlog goes first and the backlog is drained on
    # purpose before anything is counted.
    #
    unread, _, secs = one_pass(listening=False)
    pedal.midi_listen(dst["port"], seconds=1.5)
    breaks, answered, secs = one_pass(listening=True)

    RESULTS["sysex_breaks"] = {
        "count": breaks, "per_second": breaks / secs, "seconds": secs,
        "pedal": dst["label"],
        "load": args.load,
        "dumps_requested": args.dumps,
        "dumps_answered": answered,
        "breaks_per_dump": (breaks / answered) if answered else None,
        "breaks_unread": unread,
    }

    note("%s replies" % args.load,
         "%d requested, %d answered" % (args.dumps, answered))

    #
    # A missing denominator is reported before anything is concluded from
    # it.  Not a failure on its own - a pedal that legitimately refuses a
    # request it has no room for is behaving, and the harness firing
    # faster than the pedal can answer is the harness's fault - but the
    # breaks number below is being divided by this, so a shortfall has to
    # be visible next to it rather than inferred later.
    #
    if answered < args.dumps:
        note("dump shortfall",
             "%d of %d unanswered - the load is smaller than it looks, and "
             "a low break count here is not yet good news"
             % (args.dumps - answered, args.dumps))

    if not answered:
        check("audio survives its own sysex", False,
              "no dumps answered in %.1fs - nothing was measured, so this "
              "is a broken instrument rather than a passing pedal" % secs)
    else:
        check("audio survives its own sysex", not breaks,
              "%d breaks in %.1fs (%.2f per dump) while %s answered %d of "
              "%d state dumps, with the host reading the replies"
              % (breaks, secs, breaks / answered, dst["label"], answered,
                 args.dumps))

    #
    # And the same load with nothing reading the replies.
    #
    # Measured separately because it is a different defect wearing the
    # same number, and it is much the larger of the two: usb_midi_write()
    # spins on a full transmit fifo for MIDI_TX_TIMEOUT_MS per packet
    # calling tud_task(), and never usb_audio_task().  A host that is
    # reading keeps that fifo empty and the spin almost never happens; a
    # host that is not turns every packet into a timeout.  Measured on one
    # bench the same afternoon, 12 dumps either way: 12-14 breaks unread
    # against 1 read.
    #
    # It is not a contrived case.  Anything that pokes the pedal over MIDI
    # and exits - a script, this harness before it grew a listener - is a
    # host that has stopped reading.
    #
    check("audio survives sysex nobody is reading", not unread,
          "%d breaks in %.1fs while nothing drained the reply endpoint "
          "(%d read, same load)" % (unread, secs, breaks))


if __name__ == "__main__":
    sys.exit(main())
