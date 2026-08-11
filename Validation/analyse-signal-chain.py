#!/usr/bin/env python3
#
# Measure [CHAIN], and print the numbers that go in its page.
#
# The trim and the volume are scalar gains and measuring them takes a
# tone and no argument.  The gate is the whole of the interest here, and
# it cannot be measured the way the rest of the effects are.
#
# WHY THE OBVIOUS MEASUREMENT IS BLIND
#
# A gate removes material that is fifty or sixty decibels down.  That is
# by construction: everything it silences is, by definition, quieter
# than the threshold.  So the total energy of the output barely moves
# no matter what the gate does - measured over this recording, the whole
# range of Release changes the output energy by 0.00 dB while tripling
# how much of the take gets silenced.
#
# An earlier attempt at this reported that Attack and Release "barely
# change anything" on exactly that evidence.  They change a great deal.
# What was measured was the metric.
#
# So nothing here averages over the recording.  The questions are all
# about *when*: when does the gate shut after a phrase ends, when does
# it open after a note starts, and how much of the take does it silence.
#
# HOW THE GATE IS OBSERVED
#
# chain_step() applies the trim and the gate ramp as one scalar multiply
# on the frame, so dividing the gated output by the ungated output
# recovers chain.mult exactly, sample by sample.  That is a direct
# readout of the firmware's own state, and it borrows no constant from
# the firmware to get it.
#
# Note ends and note starts are located here, from the input, by a
# sliding rms crossing the threshold.  The firmware's envelope follower
# takes no part in that: its behaviour is the thing being measured, so
# it cannot also be the ruler.  See issue 140 for the version of this
# mistake that cost four confident readings.
#
# THE DECODE, which is a measurement choice and not a detail.  Same
# recording and same scaling as analyse-compressor.py, for the same
# reasons: the mp3 is committed unmodified and decodes to +2.86 dBFS, so
# it is scaled to -6 dBFS peak before it goes anywhere near the pedal.
# Needs ffmpeg, and the stats printed under "decode" are the fingerprint
# of it.
#
import math
import shutil
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, ".")
import bench as B
import pots as P

CHAIN = "Signal Chain"
INPUT = "Inputs/BassForLinus.mp3"
PEAK_DBFS = -6.0

#
# The timings are measured with the threshold at the top of its travel,
# -40 dBFS, and that wants saying rather than hiding.
#
# The default is -70 dB.  This is a clean digital recording and almost
# none of it sits that low, so at the default the gate never acts on
# this material at all and there is nothing to time.  -40 is where every
# setting of both pots does something measurable, which is the condition
# CLAUDE.md's pot-travel rule asks for: measure a control somewhere it
# can act, and treat a flat reading as a question about the setup first.
#
# A real pickup in a real room has a floor far above this recording's,
# so the low end of the Gate range is not wasted travel - it is aimed at
# a quieter input than any file on disk.
#
GATE_POT = 120

# How long the input must stay quiet for a gap to count as a phrase end.
GAP_MS = 150.0

# The pot ranges, so the printed tables read in the units on the knob.
# Taken from pots.py rather than written out here - a range moved once
# and the script that had them written out did not.
ATTACK_MS = lambda p: P.value(CHAIN, "Attack", p)
RELEASE_MS = lambda p: P.value(CHAIN, "Release", p)
GATE_DB = lambda p: P.value(CHAIN, "Gate", p)


def decode(path):
    """The mp3 as mono float32 at the pedal's own rate."""
    if not shutil.which("ffmpeg"):
        sys.exit("analyse-signal-chain: needs ffmpeg to decode " + path)
    with tempfile.NamedTemporaryFile(suffix=".f32") as f:
        subprocess.run(["ffmpeg", "-v", "error", "-i", path,
                        "-ac", "1", "-ar", str(int(B.FS)),
                        "-f", "f32le", "-acodec", "pcm_f32le", "-y", f.name],
                       check=True)
        return np.fromfile(f.name, dtype=np.float32)


def run(x, gate=0, attack=18, release=27, trim=60, volume=80):
    args = []
    for label, v in (("Gate", gate), ("Attack", attack), ("Release", release),
                     ("Trim", trim), ("Volume", volume)):
        args += ["--pot", "%s:%s=%d" % (CHAIN, label, v)]
    left, _, info = B.run(args, x, warmup=B.settle())
    return np.asarray(left), info


def multiplier(wet, dry):
    """chain.mult, recovered.  NaN where the dry sample is too near zero."""
    m = np.full(len(dry), np.nan)
    ok = np.abs(dry) > 1e-9
    m[ok] = wet[ok] / dry[ok]
    return m


def sliding_db(y, win_ms):
    """Rms of the input over a short window, in dB.  Our ruler, not theirs."""
    w = max(1, int(win_ms / 1000 * B.FS))
    p = np.convolve(y.astype(np.float64) ** 2, np.ones(w) / w, mode="same")
    return 10 * np.log10(p + 1e-24)


def note_edges(dry, thresh_db, gap_ms=250.0):
    """Where a phrase really ends, and where one really starts.

    Both need a *sustained* crossing, and getting that wrong is the
    easiest way to measure nothing.  A bass note's own waveform dips
    below any threshold twice a cycle, so a bare sample-by-sample
    crossing of a short window finds eighteen hundred "phrase ends" in
    seventy-seven seconds, which is the open string rather than the
    player.
    """
    loud = sliding_db(dry, 20.0) > thresh_db
    gap = int(gap_ms / 1000 * B.FS)
    d = np.diff(loud.astype(np.int8))

    # A phrase end: loud before, and quiet for a whole gap afterwards.
    downs = np.array([i + 1 for i in np.flatnonzero(d == -1)
                      if not loud[i + 1:i + 1 + gap].any()])
    # A note start: quiet for a whole gap before, and loud after.
    ups = np.array([i + 1 for i in np.flatnonzero(d == 1)
                    if i > gap and not loud[i + 1 - gap:i + 1].any()])
    return ups, downs


def latency(m, idx, opening, limit_ms=3000.0):
    """Ms from each edge until the multiplier reaches its rail."""
    lim = int(limit_ms / 1000 * B.FS)
    out = []
    for i in idx:
        seg = m[i:i + lim]
        seg = seg[~np.isnan(seg)]
        hit = np.flatnonzero(seg >= 1.0) if opening else np.flatnonzero(seg <= 0.0)
        if len(hit):
            out.append(hit[0] / B.FS * 1000)
    return np.array(out)


def ramp_ms(m, frm, to, limit=20000):
    """Median ms the multiplier spends travelling between its two rails.

    This is the gate's own fade, and the question it answers is whether
    either pot has any say in it.
    """
    v = m[~np.isnan(m)]
    leaving = np.flatnonzero((v[:-1] == frm) & (v[1:] != frm))
    out = []
    for i in leaving:
        j = i + 1
        seg = v[j:j + limit]
        arrive = np.flatnonzero(seg == to)
        if len(arrive) and not (seg[:arrive[0]] == frm).any():
            out.append(arrive[0] / B.FS * 1000)
    return float(np.median(out)) if out else float("nan")


def silenced(m):
    """Fraction of the take the gate shut completely."""
    return float(np.nanmean(m == 0.0))


def main():
    raw = decode(INPUT)
    peak = float(np.abs(raw).max())
    x = (raw * (10 ** (PEAK_DBFS / 20) / peak)).astype(np.float32)
    rms = 20 * math.log10(float(np.sqrt((x.astype(np.float64) ** 2).mean())))

    print("# measured by analyse-signal-chain.py - paste into "
          "Documentation/effects/signal-chain.md\n")
    print("## decode\n")
    print(f"samples      {len(raw)}  ({len(raw) / B.FS:.1f} s)")
    print(f"raw peak     {20 * math.log10(peak):+.2f} dBFS")
    print(f"scaled to    {PEAK_DBFS:+.1f} dBFS peak, {rms:.2f} dBFS rms")

    dry, info = run(x)                          # gate off: the reference
    if info.get("clipped"):
        print(f"\nWARNING: the dry run clipped {info['clipped']:.0f} times")
    dry_energy = float(np.sum(dry.astype(np.float64) ** 2))

    #
    # What the material itself looks like, which decides everything
    # below.  A threshold only means something against a floor.
    #
    lv = sliding_db(dry, 20.0)
    print("\n## the recording, 20 ms windows\n")
    pct = [1, 5, 10, 20, 50, 90]
    print("percentiles " + str(pct))
    print("dBFS        " + str([round(float(np.percentile(lv, p)), 1) for p in pct]))

    #
    # Where the gate acts at all.  This is the control that matters and
    # the one whose whole travel is obviously live.
    #
    print("\n## Gate threshold: how much of the take is silenced\n")
    gate_pots = [0, 20, 40, 60, 80, 100, 120]
    print("x-axis " + str([round(GATE_DB(p)) for p in gate_pots]))
    row = []
    for p in gate_pots:
        m = multiplier(run(x, gate=p)[0], dry)
        row.append(round(100 * silenced(m), 2))
    print("silenced % " + str(row))

    #
    # The two timing controls, measured as timings.
    #
    th = GATE_DB(GATE_POT)
    ups, downs = note_edges(dry, th, GAP_MS)
    print(f"\n## Release, at Gate {th:.0f} dBFS "
          f"({len(downs)} phrase ends in the take)\n")
    rel_pots = [0, 24, 48, 72, 96, 120]
    print("x-axis " + str([round(RELEASE_MS(p)) for p in rel_pots]))
    sil, shut, lat = [], [], []
    for p in rel_pots:
        m = multiplier(run(x, gate=GATE_POT, release=p)[0], dry)
        d = latency(m, downs, opening=False)
        sil.append(round(100 * silenced(m), 2))
        shut.append(len(d))
        lat.append(round(float(np.median(d)), 0) if len(d) else 0.0)
    print("silenced %  " + str(sil))
    print("closes      " + str(shut))
    print("median wait " + str(lat))

    #
    # The same sweep ten decibels lower, which is where the top of the
    # Release pot stops closing the gate at all.  Not a defect on its
    # own - it is the two controls interacting, and the threshold is
    # what decides whether the longest wait ever expires.
    #
    low = 100
    print(f"\n## Release again, at Gate {GATE_DB(low):.0f} dBFS\n")
    print("x-axis " + str([round(RELEASE_MS(p)) for p in rel_pots]))
    sil, shut = [], []
    for p in rel_pots:
        m = multiplier(run(x, gate=low, release=p)[0], dry)
        sil.append(round(100 * silenced(m), 2))
        shut.append(len(latency(m, note_edges(dry, GATE_DB(low), GAP_MS)[1],
                                opening=False)))
    print("silenced %  " + str(sil))
    print("closes      " + str(shut))

    print(f"\n## Attack, at Gate {th:.0f} dBFS "
          f"({len(ups)} note starts out of silence)\n")
    att_pots = [0, 12, 24, 48, 72, 96, 120]
    print("x-axis " + str([round(ATTACK_MS(p), 1) for p in att_pots]))
    med = []
    for p in att_pots:
        m = multiplier(run(x, gate=GATE_POT, attack=p)[0], dry)
        o = latency(m, ups, opening=True)
        med.append(round(float(np.median(o)), 1) if len(o) else 0.0)
    print("median open " + str(med))

    #
    # And the thing neither pot touches.
    #
    #
    # Release max is left out: it never closes on this material, so it
    # has no closing ramp to measure.  That is a finding rather than a
    # gap, and it is reported above.
    #
    print("\n## the ramp itself, at five settings\n")
    settings = [("Release min", dict(release=0)),
                ("Release mid", dict(release=60)), ("default", {}),
                ("Attack min", dict(attack=0)), ("Attack max", dict(attack=120))]
    print("x-axis " + str([n for n, _ in settings]))
    opening, closing = [], []
    for _, kw in settings:
        m = multiplier(run(x, gate=GATE_POT, **kw)[0], dry)
        opening.append(round(ramp_ms(m, 0.0, 1.0), 1))
        closing.append(round(ramp_ms(m, 1.0, 0.0), 1))
    print("open ms     " + str(opening))
    print("close ms    " + str(closing))

    #
    # The boring half, which is boring on purpose.
    #
    print("\n## Trim and Volume, 440 Hz tone at -30 dBFS\n")
    tone = B.tone(B.MID_HZ, -30.0)
    pots = [0, 24, 48, 60, 72, 96, 120]
    print("Trim x-axis   " + str([round(P.value(CHAIN, "Trim", p)) for p in pots]))
    got = []
    for p in pots:
        y, _ = run(tone, trim=p)
        got.append(round(float(B.gain_db(y, tone)[0]), 2))
    print("gain dB       " + str(got))

    #
    # Volume from the second step up.  The bottom of its travel is an
    # explicit silence rather than -40 dB, so the first point is not a
    # number on the same scale as the rest and would only be plotted as
    # a cliff - it is stated in the page instead.
    #
    print("Volume x-axis " + str([round(P.value(CHAIN, "Volume", p))
                                  for p in pots[1:]]))
    got = []
    for p in pots[1:]:
        y, _ = run(tone, volume=p)
        got.append(round(float(B.gain_db(y, tone)[0]), 2))
    print("gain dB       " + str(got))
    y, _ = run(tone, volume=0)
    print("volume at 0   %.1f dB (silence, not the bottom of the range)"
          % B.gain_db(y, tone)[0])

    #
    # The blindness itself, stated as a number, because it is the
    # reason this script exists in the shape it does.
    #
    print("\n## why total energy says nothing\n")
    for label, kw in (("Release min", dict(release=0)),
                      ("Release max", dict(release=120))):
        y, _ = run(x, gate=GATE_POT, **kw)
        e = 10 * math.log10(float(np.sum(y.astype(np.float64) ** 2))
                            / dry_energy + 1e-30)
        m = multiplier(y, dry)
        print("  %-12s energy vs gate off %+6.2f dB   silenced %5.2f %%"
              % (label, e, 100 * silenced(m)))


main()
