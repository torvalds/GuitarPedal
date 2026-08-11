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
# Telemetry sends it through fraction_to_byte(), so it arrives as 7 bits:
# 0.787% of the sample period per step.
#
# WHICH NUMBER TO BELIEVE, WHICH IS THE WHOLE POINT OF THIS FILE
#
# The obvious statistic is (routed - empty), and it is the bad one.
# Measured across five reboots of identical firmware:
#
#   boot   empty         reverb   difference
#      1   13            38       25.00
#      2   12            38       26.00
#      3   11, 12        38       26.75
#      4   11            38       27.00
#      5   11, 12, 13    38       26.00
#
# The routed reading never moved.  The *empty* one wandered over three
# steps and dithered within a single boot.  So the noise is all in the
# baseline, and subtracting it does not cancel - it imports it, and the
# spread it imports is the same size as the difference a change to one
# effect makes.
#
# Why the idle case is the noisy one is not established here.  A
# plausible half is that get_usb_audio_input() takes a different path
# depending on whether the host happens to be streaming, and whether
# pipewire has the pedal open varies across re-enumeration - but that is
# a guess and is not what this reports.
#
# So the number this reports is the **routed absolute load**, and
# comparing two firmwares means comparing that.  The empty reading is
# printed for context, with its spread, and never subtracted.
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
STEP_PCT = 100.0 / 127  # what one telemetry step is worth


def effect_names(map_h="../Software/build/effect_map.h"):
    try:
        text = open(map_h).read()
    except OSError:
        return {}
    return {i: n for i, n in enumerate(re.findall(r'\.name = "([^"]*)"', text))}


def sample_loads(p, n, gap=0.25):
    """n telemetry frames, one aseqdump for the lot.

    telemetry() in pedal.py starts and stops a dump per call, which is
    both slow and lossy - the reply can land after the dump is killed.
    One dump held open across every request loses nothing.
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

    blob = bytes.fromhex("".join(
        re.findall(r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)", text)
    ).replace(" ", ""))

    out, i = [], 0
    while True:
        i = blob.find(bytes([0xF0, 0x7D, 0x0B]), i)
        if i < 0:
            break
        end = blob.find(0xF7, i)
        body = blob[i + 3:end]
        if len(body) >= 6:
            out.append(body[5])
        i = end + 1 if end > 0 else i + 3
    return out


def reboot(p):
    pedal.enter_bootsel(p)
    time.sleep(3)
    subprocess.run(["picotool", "reboot"], capture_output=True)
    time.sleep(7)


def one_pass(p, ids, n=6):
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
    print("pedal on %s, %s, %d boot(s)\n" % (p, label, boots))
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
        print("  %4d   %-16s  %-14s  %6.3f %%"
              % (k + 1, sorted(set(empty)) or "-", sorted(set(routed)),
                 statistics.mean(routed) * STEP_PCT))

    if routed_all:
        m = statistics.mean(routed_all)
        print("\n  %s routed: %.2f/127 = %.3f %% of the sample period"
              % (label, m, m * STEP_PCT))
        if len(set(routed_all)) > 1:
            print("  spread %d..%d steps across every reading"
                  % (min(routed_all), max(routed_all)))

    pedal.program_change(p, 0)
    print("  scene 0 reloaded")


main()
