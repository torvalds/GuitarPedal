#!/usr/bin/env python3
#
# Measure [COMPRESSOR], and print the numbers that go in its page.
#
# A compressor cannot be measured with a steady tone, or rather it can
# and the answer is worthless: what it does is change its gain over
# time, in response to a signal that changes over time.  A sine tells
# you where the knee is and nothing about whether the thing is any use.
#
# So there are two halves here.  The static half is a level sweep with
# tones, which gives the threshold and the ratio exactly.  The dynamic
# half plays a real recording through it - Inputs/BassForLinus.mp3, a
# friend of Linus's playing scales on a bass - and asks what the gain
# actually did over seventy-six seconds of playing.
#
# THE DECODE, which is a measurement choice and not a detail.
#
# The mp3 is committed unmodified, so it matches the copy it came from.
# It decodes to +2.86 dBFS peak - mp3 reconstruction overshoots, and the
# master was already close to full scale - so used raw it would clip the
# pedal's input before the compressor saw any of it.
#
# It is scaled here to -6 dBFS peak, which is where people are usually
# told to land an instrument track, and left there.  That choice moves
# every number below: a quieter input crosses the threshold less often
# and the compressor does less.  It is stated in the page for that
# reason.
#
# Needs ffmpeg.  The decode is deterministic for a given ffmpeg, so the
# stats printed under "decode" are the fingerprint of it - if those move
# and nothing else changed, the decoder did.
#
import math
import shutil
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, ".")
import bench as B

COMP = "Compressor"
INPUT = "Inputs/BassForLinus.mp3"
PEAK_DBFS = -6.0
WINDOW_S = 0.5


def decode(path):
    """The mp3 as mono float32 at the pedal's own rate."""
    if not shutil.which("ffmpeg"):
        sys.exit("analyse-compressor: needs ffmpeg to decode " + path)
    with tempfile.NamedTemporaryFile(suffix=".f32") as f:
        subprocess.run(["ffmpeg", "-v", "error", "-i", path,
                        "-ac", "1", "-ar", str(int(B.FS)),
                        "-f", "f32le", "-acodec", "pcm_f32le", "-y", f.name],
                       check=True)
        return np.fromfile(f.name, dtype=np.float32)


def pot_value(name, db, lo, hi):
    """A LINEAR(lo hi) pot, as the 0..120 the firmware stores."""
    return ["--pot", f"{COMP}:{name}={round((db - lo) / (hi - lo) * 120)}"]


def routed(level_db=-20.0, attack_ms=15.0, release_ms=150.0,
           ratio=4.8, boost_db=6.0):
    return (["--pot", "Signal Chain:Gate=0", "--route", COMP]
            + pot_value("Level", level_db, -40.0, 0.0)
            + pot_value("Attack", attack_ms, 2.0, 100.0)
            + pot_value("Release", release_ms, 50.0, 500.0)
            + pot_value("Ratio", ratio, 1.0, 20.0)
            + pot_value("Boost", boost_db, 0.0, 24.0))


def squeeze(x, window, **kw):
    """How many dB of dynamic range a setting takes out of a performance.

    The p5..p95 spread of the output's half-second levels against the
    input's.  Not the peak reduction, which any limiter wins on, and not
    the mean gain, which is mostly the makeup: the question a compressor
    is bought to answer is whether the quiet and the loud ended up
    closer together.
    """
    dry, _, _ = B.run(["--pot", "Signal Chain:Gate=0"], x, warmup=B.settle())
    di = levels(np.asarray(dry), window)
    wet, _, _ = B.run(routed(**kw), x, warmup=B.settle())
    wo = levels(np.asarray(wet), window)
    spread = lambda v: float(np.percentile(v, 95) - np.percentile(v, 5))
    return spread(di) - spread(wo)


def levels(y, window):
    n = len(y) // window
    return 20 * np.log10(
        np.sqrt((y[:n * window] ** 2).reshape(n, window).mean(axis=1)) + 1e-12)


def main():
    raw = decode(INPUT)
    peak_of = float(np.abs(raw).max())
    peak = peak_of
    x = (raw * (10 ** (PEAK_DBFS / 20) / peak_of)).astype(np.float32)
    rms = 20 * math.log10(float(np.sqrt((x.astype(np.float64) ** 2).mean())))

    print("# measured by analyse-compressor.py - paste into "
          "Documentation/effects/compressor.md\n")
    print("## decode\n")
    print(f"samples      {len(raw)}  ({len(raw) / B.FS:.1f} s)")
    print(f"raw peak     {20 * math.log10(peak):+.2f} dBFS")
    print(f"scaled to    {PEAK_DBFS:+.1f} dBFS peak, {rms:.2f} dBFS rms")

    #
    # Static: where the knee is, from tones.  Exact, and says nothing
    # about whether the compressor is useful.
    #
    print("\n## static transfer, 440 Hz tone, defaults but Level as shown\n")
    tone_in = list(range(-60, -5, 5))
    print("x-axis " + str(tone_in))
    for lv in (-20.0, -30.0):
        row = []
        for dbfs in tone_in:
            m = B.measure(routed(lv), f0=B.MID_HZ, dbfs=float(dbfs))
            row.append(round(float(m["gain_db"]), 2))
        print(f"Level {lv:.0f} " + str(row))

    #
    # Dynamic: the same question asked of somebody playing.
    #
    window = int(WINDOW_S * B.FS)
    dry, _, info = B.run(["--pot", "Signal Chain:Gate=0"], x, warmup=B.settle())
    di = levels(np.asarray(dry), window)
    if info.get("clipped"):
        print(f"\nWARNING: the dry run clipped {info['clipped']:.0f} times")

    print(f"\n## input level over time, {WINDOW_S}s windows, "
          f"{len(di)} of them\n")
    print("seconds 0 --> " + str(round(len(di) * WINDOW_S)))
    print("input " + str([round(float(v), 1) for v in di]))

    for lv in (-20.0, -30.0):
        wet, _, info = B.run(routed(lv), x, warmup=B.settle())
        wo = levels(np.asarray(wet), window)
        gain = wo - di
        print(f"\n## Level {lv:.0f}: output level and gain applied\n")
        if info.get("clipped"):
            print(f"WARNING: clipped {info['clipped']:.0f} times")
        print(f"gain {gain.min():.1f} .. {gain.max():.1f} dB")
        print("output " + str([round(float(v), 1) for v in wo]))
        print("gain " + str([round(float(v), 1) for v in gain]))

        #
        # ...and the same thing as a transfer curve, which is what the
        # static sweep claims to be.  Binned by input level, so the two
        # can be laid over each other and disagree if they want to.
        #
        #
        # Only bins with something in them.  An empty bin reported as
        # zero is not a measurement of anything and would draw a line
        # down to it; a bin holding one window is noise wearing a
        # number.  Two is the smallest that is worth plotting, and the
        # counts are printed so the thin ends can be read sceptically.
        #
        print(f"\n## Level {lv:.0f}: gain against input level, from the "
              f"recording\n")
        centres, row, counts = [], [], []
        for lo in range(-60, -10, 5):
            m = (di >= lo) & (di < lo + 5)
            if m.sum() >= 2:
                centres.append(lo + 2)
                row.append(round(float(gain[m].mean()), 2))
                counts.append(int(m.sum()))
        print("x-axis " + str(centres))
        print("gain " + str(row))
        print("windows " + str(counts))

    #
    # What the knobs are worth, and at what input level.
    #
    # The threshold is compared against an envelope of the input, so how
    # much any setting does depends entirely on how hot the instrument
    # is.  Three levels rather than one, because the answer to "is this
    # default any good" turned out to be different at each.
    #
    print("\n## dynamic range removed, by threshold and input level\n")
    thresholds = list(range(-40, -14, 5))
    print("x-axis " + str(thresholds))
    for peak in (-6.0, -12.0, -20.0):
        y = (raw * (10 ** (peak / 20) / peak_of)).astype(np.float32)
        print(f"peak {peak:.0f} " +
              str([round(squeeze(y, window, level_db=float(t)), 1)
                   for t in thresholds]))

    #
    # Attack and release cost distortion on low notes, because a fast
    # envelope follows the waveform rather than the note.  Measured up
    # the neck as well, since this is a guitar pedal and the bass low E
    # is the worst case rather than the usual one.
    #
    for name, values, kw in (("release", [50, 100, 150, 300, 500], "release_ms"),
                             ("attack", [2, 5, 15, 30, 60, 100], "attack_ms")):
        print(f"\n## THD against {name}, driven 18 dB over threshold\n")
        notes = [40, 80, 160, 320, 640]
        print("x-axis " + str(notes))
        for v in values:
            row = [round(float(B.measure(routed(level_db=-30.0, **{kw: float(v)}),
                                         f0=float(f), dbfs=-12.0)["thd_db"]), 1)
                   for f in notes]
            print(f"{name} {v} " + str(row))

    return 0


if __name__ == "__main__":
    sys.exit(main())
