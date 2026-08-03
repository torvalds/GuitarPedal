#!/usr/bin/env python3
#
# Does per-effect channel routing actually do anything?
#
# The chain is stereo inside even on a mono board - the front of it
# duplicates the input - and an effect can be told which half to read
# and where to put its answer.  That is worth exactly nothing unless
# something checks it, and nothing could: there is no way to set it over
# MIDI yet, and the difference is inaudible on a mono output unless you
# know what to listen for.
#
# So plant two scenes that differ in one field, and measure.
#
#   scene 0     TONE   in=L out=L        the split: L shaped, R kept
#               TONE 2 in=L out=merge    the join:  L = tone2(L) + R
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
# channel is not being kept or the merge is not merging. Planting takes
# a trip through BOOTSEL, so both scenes go in at once and the test
# switches between them with a Program Change afterwards.
#
import subprocess
import sys
import time

import numpy as np

import audio
import pedal
import scene

PICOTOOL = "picotool"


def plant(scenes, picotool, base_seq):
    """Both scenes, one trip through BOOTSEL."""
    p = pedal.port()
    pedal.enter_bootsel(p)

    for _ in range(50):
        time.sleep(0.2)
        if subprocess.run([picotool, "info"], capture_output=True).returncode == 0:
            break
    else:
        sys.exit("test-split: never reached BOOTSEL")

    import tempfile
    import os
    for slot, (key, payload) in enumerate(scenes, start=40):
        img = scene.slot_image(payload, key, base_seq + slot)
        tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        try:
            tmp.write(img)
            tmp.close()
            subprocess.run([picotool, "load", "-o",
                            hex(scene.slot_address(slot)), tmp.name],
                           check=True, capture_output=True)
        finally:
            os.unlink(tmp.name)

    subprocess.run([picotool, "reboot"], check=True, capture_output=True)
    time.sleep(5)


def measure(p, card, which, freq):
    pedal.program_change(p, which)
    d = audio.trim(audio.capture(3, card))
    wet, dry = d[:, 0], d[:, 1]
    # The right channel is the raw sample; scale it into the same units
    # the left one is already in - see audio.SAMPLE_TO_FLOAT.
    return audio.ratio_db(wet, dry * audio.SAMPLE_TO_FLOAT), wet, dry


def main():
    picotool = PICOTOOL
    if subprocess.run(["which", picotool], capture_output=True).returncode:
        import os
        picotool = os.path.expanduser("~/bin/picotool")
        if not os.path.exists(picotool):
            print("test-split: SKIPPED - no picotool")
            return 0

    card, p = audio.find_card(), pedal.port()
    if card is None or p is None:
        print("test-split: SKIPPED - no pedal on the USB")
        return 0

    effects = scene.effects_from_map()
    t1 = scene.by_name(effects, "Tone 1")
    t2 = scene.by_name(effects, "Tone 2")
    routed = [effects.index(t1), effects.index(t2)]

    t1["channels"] = scene.channels(scene.IN_LEFT, scene.OUT_LEFT)

    t2["channels"] = scene.channels(scene.IN_LEFT, scene.OUT_MERGE)
    merged = scene.build(effects, routed)

    t2["channels"] = scene.channels(scene.IN_LEFT, scene.OUT_LEFT)
    kept = scene.build(effects, routed)

    #
    # Above whatever the pedal has already written, not at some number
    # chosen in advance.  A planted scene only wins if its sequence beats
    # the ones already there, and the pedal has been saving scene 0 every
    # time anything measured what a save costs - so a fixed base quietly
    # stops working after enough of those, and the test then measures two
    # scenes that never loaded.
    #
    ident = pedal.identity(p)
    newest = (ident or {}).get("save", {}).get("newest", 0)
    print(f"test-split: planting two scenes above sequence {newest}, "
          f"one BOOTSEL trip")
    plant([(0, merged), (1, kept)], picotool, newest + 10)

    n = pedal.effect_count()
    pedal.wet_dry(p, n - 1)

    merge_db, wet, dry = measure(p, card, 0, 440.0)
    keep_db, _, _ = measure(p, card, 1, 440.0)

    if audio.peak(dry) < 1e-6:
        print("test-split: SKIPPED - nothing on the analog input")
        return 0

    print(f"  scene 0, TONE 2 out=merge : {merge_db:+.2f} dB   (want +6.02)")
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
