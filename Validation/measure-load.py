#!/usr/bin/env python3
#
# What an effect costs the audio core, measured on the pedal itself.
#
# HOW THE PEDAL KNOWS
#
# single_sample() times the spin waiting for the RX DMA to produce a
# sample, one sample in sixteen.  Per sample period the core either
# spins there or works, and the period is fixed by the DMA, so the spin
# is the idle fraction exactly and 1 - idle is the load.  Nothing is
# estimated and nothing is a model; it is the audio loop timing itself.
#
# Two things make that worth reading finely.  The firmware times the
# spin in cpu cycles, so a tick is 0.03% of the sample period rather
# than the 4.8% a 1MHz timer gives; and telemetry carries the result at
# fourteen bits - an MSB where the old seven-bit byte was, an LSB after
# it - so 0.006% a step rather than 0.787%.  Neither is much use without
# the other.
#
# Measured across four boots, which is what that buys:
#
#   empty chain      9.577 .. 9.614 %
#   reverb routed   31.624 .. 31.691 %     std 0.014 %
#
# So (routed - empty) from a single reading is meaningful, and -b N is
# for confidence rather than for averaging anything away.  The empty
# reading is printed beside it because a baseline that has moved is the
# first sign that something outside this has changed.
#
# Numbers taken before either change are not comparable at this
# precision: with a microsecond timer the idle reading sat in one of two
# states 1.2% apart and held one for a whole boot, which is the
# instrument and not the pedal.
#
# THE BASELINE IS A SETTING, NOT JUST A BUILD
#
# process_output() returns immediately when "USB L/R Out" is None and
# otherwise stores a frame into a ring with a release barrier every
# sample, on the audio core, whether or not a host is listening.
# Measured, that is +0.306% of the sample period for the default Dry,
# and +0.348% for Wet/Dry.
#
# Which is small and was still large enough to be mistaken for
# something else: two runs an hour apart differed by 0.37% and got as
# far as an issue blaming the measurement, when what had changed was
# that this setting had been moved in the web app in between.  A
# difference of two effects cancels it, an absolute load does not.
#
# So a run pins it to None and lets the closing program change put the
# scene back.  The number this prints is therefore what the *chain*
# costs, with the USB tap excluded on purpose.
#
# WHY AN IDLE PEDAL IS A VALID PLACE TO MEASURE
#
# Nothing need be plugged in.  The reverb runs eight combs and four
# allpasses every sample whatever the input is, so its cost has no data
# dependency worth the name.  An effect with a branch on the signal -
# the gate, a compressor above its threshold - would not be safe to
# measure this way and is not what this is for.
#
# WHY IT IS SAFE TO RUN
#
# Routing is set live over SysEx and nothing is ever saved: the flash is
# untouched, and the run ends with a program change that reloads the
# scene.  Unplugging would do the same.
#
# Called as:  ./measure-load.py [-b N] [effect-id ...]
#             -b N   re-measure across N reboots (default 1)
#             ids    default 11, the Reverb
#
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, ".")
import pedal

SETTLE = 1.0            # effect fades are 100 ms, the load meter 21 ms
DEFAULT = [11]          # Reverb

#
# The settings pseudo-effect is last in effects[], and its first pot is
# "USB L/R Out".  Pinned to None for the duration of a run - see the note
# on the baseline below - and put back by the program change at the end,
# the same way the routing is.
#
SETTINGS = 18
USB_OUT_POT = 1
USB_OUT_NONE = 0
STEP_PCT = 100.0 / 16383   # what one telemetry step is worth, 14-bit
COARSE = 128               # ...and how many of them the old 7-bit step was


def effect_names(map_h="../build/effect_map.h"):
    try:
        text = open(map_h).read()
    except OSError:
        return {}
    return {i: n for i, n in enumerate(re.findall(r'\.name = "([^"]*)"', text))}


def _frames(text):
    """Every telemetry body in a blob of hex, as a load value."""
    out = []
    for m in re.finditer(r"F0 7D 0B((?: [0-9A-F]{2})+?) F7", text.upper()):
        body = [int(v, 16) for v in m.group(1).split()]
        #
        # 14 bits where the firmware sends them, and the old 7-bit byte
        # scaled up where it does not, so an older build stays
        # comparable with a newer one.
        #
        if len(body) >= 7:
            out.append((body[5] << 7) | body[6])
        elif len(body) >= 6:
            out.append(body[5] * COARSE)
    return out


def _via_amidi(dev, n, dump_s):
    """One invocation per reading: sends and dumps in about half a second."""
    out = []
    for _ in range(n):
        r = subprocess.run(["amidi", "-p", dev, "-S", "F0 7D 0B F7",
                            "-d", "-t", str(dump_s)],
                           capture_output=True, text=True)
        if r.returncode:
            return None
        out += _frames(" ".join(r.stdout.split()))
    return out


def _via_seq(p, n, gap=0.25):
    """The sequencer, for when something else holds the raw device.

    Slower - holding aseqdump open is itself what makes the raw device
    busy, so pedal.send() drops to aplaymidi at two seconds a call - and
    the extra traffic is not nothing when the thing being measured is
    the audio core's spare time.  Correct, though, and available when
    the fast path is not.
    """
    dump = subprocess.Popen(["aseqdump", "-p", p], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        time.sleep(0.5)
        for _ in range(n):
            pedal.send(p, 0x0B)
            time.sleep(gap)
        time.sleep(1.0)
    finally:
        dump.terminate()
        text = dump.stdout.read()
        dump.wait()

    hexed = " ".join(re.findall(r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)",
                                text))
    return _frames(" ".join(hexed.split()))


_slow_warned = []


def sample_loads(p, n, dump_s=0.4):
    """n telemetry frames, by whichever transport is available.

    amidi is preferred and costs about half a second a reading.  Whether
    it can be had depends on what else has the MIDI port open, and the
    ordinary case is the WebMIDI app in a browser tab - which also polls
    telemetry five times a second while it is there.  So being refused
    is a condition rather than an error, exactly as pedal.py's _play()
    already assumes, and the fallback is not a rare path.
    """
    dev = pedal.rawmidi(p)
    if dev:
        got = _via_amidi(dev, n, dump_s)
        if got is not None:
            return got
    if not _slow_warned:
        print("  (raw MIDI device busy - using the sequencer, four times "
              "slower and noisier)")
        _slow_warned.append(True)
    return _via_seq(p, n)


def fmt(vals):
    """A reading as a percentage range, however many bits it arrived in."""
    if not vals:
        return "-"
    lo, hi = min(vals) * STEP_PCT, max(vals) * STEP_PCT
    return "%.3f" % lo if lo == hi else "%.3f..%.3f" % (lo, hi)


def reboot(p):
    pedal.enter_bootsel(p)
    time.sleep(3)
    subprocess.run(["picotool", "reboot"], capture_output=True)
    time.sleep(7)


def one_pass(p, ids, n=6):
    pedal.set_pot(p, SETTINGS, USB_OUT_POT, USB_OUT_NONE)
    pedal.set_routing(p)
    time.sleep(SETTLE)
    empty = sample_loads(p, n)
    pedal.set_routing(p, *ids)
    time.sleep(SETTLE)
    routed = sample_loads(p, n)
    pedal.set_routing(p)
    return empty, routed


def main():
    args = sys.argv[1:]
    boots = 1
    if args and args[0] == "-b":
        boots = int(args[1])
        args = args[2:]
    ids = [int(a) for a in args] or DEFAULT

    names = effect_names()
    label = " + ".join(names.get(i, "effect %d" % i) for i in ids)
    p = pedal.port()
    print("pedal on %s, %s, %d boot(s)" % (p, label, boots))
    print("USB L/R Out pinned to None; the scene goes back at the end\n")
    print("  boot   empty (noisy)     routed          routed load")

    routed_all = []
    for k in range(boots):
        if k:
            reboot(p)
            p = pedal.port()
        empty, routed = one_pass(p, ids)
        if not routed:
            print("  %4d   no reply" % (k + 1))
            continue
        routed_all += routed
        print("  %4d   %-16s  %-16s  %6.3f %%"
              % (k + 1, fmt(empty), fmt(routed),
                 statistics.mean(routed) * STEP_PCT))

    if routed_all:
        m = statistics.mean(routed_all)
        print("\n  %s routed: %.3f %% of the sample period" % (label, m * STEP_PCT))
        if len(set(routed_all)) > 1:
            print("  spread %.3f %% across every reading, %.3f %% std"
                  % ((max(routed_all) - min(routed_all)) * STEP_PCT,
                     statistics.pstdev(routed_all) * STEP_PCT))

    pedal.program_change(p, 0)
    print("  scene 0 reloaded")


main()
