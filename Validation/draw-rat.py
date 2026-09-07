#!/usr/bin/env python3
#
# The pictures for Documentation/effects/rat.md.
#
# Three of them, each answering a question a table cannot: what a
# clipped edge looks like, what real playing looks like, and where the
# brightness that a listener actually picked out of a blind test is
# sitting.
#
import argparse
import importlib.util
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import audio
import bench as B
import figure
import loop
import targets as T

#
# analyse-rat.py has a hyphen in it, because check-analysis.py finds a
# page's measurements by looking for analyse-<page>.py and the page is
# rat.md.  A hyphen is not an identifier, so it cannot be imported by
# name - and it has to be imported rather than re-implemented, because
# the whole point is that the page's pictures and the page's numbers
# come from one run of one measurement.
#
A = importlib.util.module_from_spec(
    importlib.util.spec_from_file_location(
        "analyse_rat", os.path.join(HERE, "analyse-rat.py")))
A.__spec__.loader.exec_module(A)

FS, VPEAK = 48000.0, 1.41421356


def need(path, how):
    """A missing input says how to make it, and does not traceback."""
    if not os.path.exists(path):
        raise SystemExit("draw-rat: %s is not there.\n  %s" % (path, how))
    return path


def wav(path):
    w = wave.open(need(os.path.join(HERE, path), TAKES_HOW))
    a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
    return a[::w.getnchannels()] / 32768.0


def align_to(ref, y, sl=None, maxlag=4000):
    """Slide y onto ref, over the window that is going to be drawn.

    Aligning once over a whole take does not work here.  The USB audio
    endpoint is asynchronous, so the pedal runs on its own clock and the
    offset between a capture and the same material through the bench
    drifts across the take - about a millisecond over six seconds, which
    is nothing for a band measurement and is the whole picture when the
    picture is thirty milliseconds of waveform.  So the alignment is
    measured where the drawing is.
    """
    if sl is not None:
        a = max(sl.start - maxlag, 0)
        b = min(sl.stop + maxlag, len(ref))
        ref, y = ref[a:b], y[a:b]
    n = 1
    while n < len(ref) + maxlag:
        n *= 2
    c = np.fft.irfft(np.fft.rfft(y, n) * np.conj(np.fft.rfft(ref, n)), n)
    c = np.concatenate([c[-maxlag:], c[:maxlag]])
    return int(c.argmax()) - maxlag


def edge(out, capture):
    """One clipped edge, three ways.  This is where the asymmetry is."""
    import json
    import ngspice as NG

    d = json.load(open(need(capture, CAPTURE_HOW)))
    hw = np.array(d["waves"]["burst-12"]["back"])
    t = T.target("rat")
    x = loop.tone(220.0, -12.0, len(hw))

    model, _i = B.through(B.BENCH, T.pot_args(t, t["knobs"]), x.astype(np.float32))
    model = np.asarray(model, float)
    deck = NG.tran("rat-helios", "out", x * VPEAK, fs=FS, settle=0.2,
                   params=dict(dist=0.120, filter=0.125, sweep=0.0,
                               mode=1)) / VPEAK

    a = int(0.12 * len(hw))
    sl = slice(a, a + int(2.2 * FS / 220.0))
    model = np.roll(model, -align_to(hw, model, sl))
    deck = np.roll(deck, -align_to(hw, deck, sl))
    ms = np.arange(sl.stop - sl.start) / FS * 1000.0

    fig, ax = figure.figure(height=3.6)
    figure.leg(ax, ms, hw[sl], "hardware")
    figure.leg(ax, ms, deck[sl], "netlist")
    figure.leg(ax, ms, model[sl], "model")
    ax.axhline(0.0, color="#b0b0ad", lw=0.8, zorder=1)
    ax.set_xlabel("ms"); ax.set_ylabel("output, pedal scale")
    return figure.finish(
        fig, ax, out,
        title="Two clipping levels, not one: 220 Hz at -12 dBFS, Distortion noon",
        caption="Helios / rat-helios.cir / [RAT] on the bench.\n"
                "Rebuildable from a capture-rat.py run; the capture itself "
                "needs the pedal.")


def model_leg(seconds=6.0, offset=12.0, peak=0.35, **knobs):
    """The model's answer to the same guitar, rendered now.

    Rendering rather than reading a wav off disk, and that is the point:
    shoot-loop.py can write the model leg out beside the hardware one,
    but a wav on disk is a model from whenever it was captured, and this
    page is about a model that is expected to keep moving.  Only the
    hardware leg has to come from a file, because only it needs the
    pedal.
    """
    x, _ = audio.decode(need(os.path.join(HERE, "Inputs/Dry-Guitar.wav"),
                             SOURCE_HOW), seconds=seconds, offset=offset)
    x = x / max(np.abs(x).max(), 1e-9) * peak
    t = T.target("rat")
    k = dict(t["knobs"])
    k.update(knobs)
    y, _i = B.through(B.BENCH, T.pot_args(t, k), x.astype(np.float32))
    return x, np.asarray(y, float)


def match(ref, y):
    return y / np.sqrt(np.mean(y ** 2)) * np.sqrt(np.mean(ref ** 2))


def playing(out, ms=70.0, dpi=200):
    """Real playing at full Distortion, which is what was listened to.

    Two legs and not three.  The netlist was drawn here as a third
    opinion and had to come out again: on this material it correlates
    with the pedal at 0.574, where the model manages 0.990, and putting
    a curve on a page next to prose saying the deck reproduces the pedal
    would have been a picture arguing against its own caption.

    The deck is not wrong about tones - its own header records 0.62 dB
    rms of small-signal error and harmonics inside 0.4 dB, and the duty
    cycle figure on this page uses it happily.  It is wrong about a
    chord, and by an amount no tone measurement here would have found.
    See ISSUES 354.
    """
    hw = wav("rat-full-hardware.wav")
    _x, now = model_leg(Distortion=1.0)
    #
    # The capture came through the pedal's own analog input and the
    # model did not, so one of the two has to be moved before they can
    # be drawn together.  Colouring the model rather than uncolouring
    # the capture, because this is six seconds of playing: the inverse
    # puts 64% of what it adds below 20 Hz and doubles the baseline
    # wander, and a baseline is what the eye reads a plateau against.
    # The forward direction has no boost in it and nothing to tune.
    #
    now = loop.colour(now)
    #
    # Not level-matched.  The model is 1.45 dB quieter than the pedal
    # over these six seconds and hiding it would be hiding a result.
    #
    # It is not a gain constant.  At Distortion minimum, where the
    # op-amp is a follower and the only things left are the input
    # network, the clamp curve, the follower and the output network,
    # the model matches the pedal to 0.02 dB across the whole ladder -
    # so all four are right and rat-helios.cir's "cannot be told apart,
    # the three enter as one product" is answered from the other end.
    # What is left is stimulus-dependent: on a 220 Hz tone at full
    # Distortion this model is 0.80 dB *louder* than the pedal, in the
    # fundamental and in total rms alike.  What differs is the droop:
    # driven with the pedal's own recorded stimulus and windowed the
    # same way, this model's plateaus tilt 1.27 dB where the pedal's
    # tilt 2.17, at every drive level tested.  A flatter plateau is
    # less peaky for the same energy, and on broadband material that
    # comes out 1.45 dB the other way.
    #
    a = int(2.0 * FS)
    sl = slice(a, a + int(ms / 1000.0 * FS))
    now = np.roll(now, -align_to(hw, now, sl))
    t_ms = np.arange(sl.stop - sl.start) / FS * 1000.0

    fig, ax = figure.figure(width=14.0, height=4.4)
    figure.leg(ax, t_ms, hw[sl], "hardware")
    figure.leg(ax, t_ms, now[sl], "model")
    ax.axhline(0.0, color="#b0b0ad", lw=0.8, zorder=1)
    ax.set_xlabel("ms")
    ax.set_ylabel("output, dBFS scale")
    return figure.finish(
        fig, ax, out, dpi=dpi,
        title="%g ms of guitar, Distortion full" % ms,
        caption="Real levels, not matched: the model runs 1.45 dB quieter "
                "over these six seconds.  Not a gain constant - at Distortion "
                "minimum the two agree to 0.02 dB across the whole ladder, and "
                "on a 220 Hz tone here the model is 0.80 dB *louder*.  "
                "Correlation over 150 ms is 0.990.\n"
                "Both legs have been through the bench's own input high-pass: "
                "the capture because it had to be, the model because it is "
                "put there, so the two are alike.  Aligned over the window "
                "drawn, since the pedal runs on its own clock and drifts about "
                "a millisecond across six seconds.\n"
                "The model leg is rebuilt from the bench; only the hardware "
                "leg needs the pedal.")


def bright(out):
    """Where the difference a listener picked out actually sits."""
    hw, was = wav("rat-full-hardware.wav"), wav("rat-full-before.wav")
    _x, now = model_leg(Distortion=1.0)

    def bands(y):
        Y = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
        f = np.fft.rfftfreq(len(y), 1.0 / FS)
        edges = np.array([80, 250, 800, 2500, 6000, 12000, 20000], float)
        return np.array([10*np.log10(max(Y[(f >= lo) & (f < hi)].sum(), 1e-30))
                         for lo, hi in zip(edges[:-1], edges[1:])]), edges

    H, e = bands(hw)
    M, _ = bands(now)
    W, _ = bands(was)
    mid = np.sqrt(e[:-1] * e[1:])

    fig, ax = figure.figure(height=3.2)
    ax.axhline(0.0, color="#b0b0ad", lw=0.8, zorder=1)
    figure.leg(ax, mid, H - H, "hardware", marker="o", ms=3)
    figure.leg(ax, mid, M - H, "model", marker="o", ms=3,
               label="[RAT], with the op-amp's limits")
    figure.leg(ax, mid, W - H, "netlist", marker="o", ms=3,
               label="[RAT], with an ideal op-amp")
    ax.set_xscale("log"); ax.set_xlabel("Hz")
    ax.set_ylabel("dB against the pedal")
    return figure.finish(
        fig, ax, out,
        title="What a blind listener picked out, Distortion full",
        caption="Band energies over six seconds of playing, level-matched.")


#
# The four analysis charts, drawn as PNGs rather than as mermaid
# xychart blocks in the page.  There is a real argument for the latter -
# a number in a diff is legible and a curve in a diff is not - and it
# holds for a coarse result and stops holding here.  Mermaid has no
# logarithmic
# axis, so a frequency response had to be eleven evenly spaced points
# with log-spaced labels; it ties one x label to one data point, so more
# resolution means more clutter; and it cannot overlay a measurement of
# the pedal on a curve of the model, which is the single most useful
# thing this page can show.
#
# What replaces the safety net is not nothing: the numbers stay in the
# page as tables and check-analysis.py re-measures those.  The picture
# got better and the record stayed text.
#


def _bode(out, series, title, caption, ylo, yhi):
    fig, ax = figure.figure(width=8.0, height=4.0)
    figure.sweep(ax, A.SEMITONES, series)
    ax.set_xscale("log")
    ax.set_xlim(A.SEMITONES[0], A.SEMITONES[-1])
    ax.set_xticks([20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000])
    ax.set_xticklabels(["20", "50", "100", "200", "500", "1k", "2k", "5k",
                        "10k", "20k"])
    ax.set_ylim(ylo, yhi)
    ax.set_xlabel("Hz")
    ax.set_ylabel("dB")
    return figure.finish(fig, ax, out, title=title, caption=caption, dpi=200)


def response(out):
    """What the feedback network asks for, and where it stops getting it."""
    return _bode(out, A.distortion_response(),
                 "Small signal against Distortion, Filter noon, Sweep minimum",
                 "The peak moves down as the gain goes up: the closed-loop "
                 "corner sits at GBW over whatever the network is asking "
                 "for.\nA semitone grid, measured at -100 dBFS, which is "
                 "where this is still linear at full Distortion.",
                 -25, 60)


def filt(out):
    """The one tone control, which runs backwards."""
    return _bode(out, A.filter_response(),
                 "The Filter control at Distortion noon",
                 "Turning it up makes it darker.  One pole, after "
                 "everything.\nRebuildable with analyse-rat.py; no hardware "
                 "needed.", -10, 45)


#
# What the Mode switch is for, which the figure above cannot show.
#
def modes(out):
    """Three ceilings, once there is gain in front of them."""
    fig, ax = figure.figure(width=11.0, height=4.0, rows=1, cols=2,
                            sharey=True)
    for panel, (dist, label) in zip(ax, ((0.45, "noon"), (1.0, "full"))):
        figure.sweep(panel, A.CEILING_RUNGS, A.mode_ceilings(dist))
        panel.set_title("Distortion %s" % label, fontsize=9,
                        color=figure.FG, loc="left")
        panel.set_xlabel("in, dBFS")
        panel.set_ylim(-45, 6)
    ax[0].set_ylabel("out, dBFS")
    #
    # The heading goes on the figure, not on the first panel.  finish()
    # puts a title on axes[0], which for one panel is the same thing and
    # for two reads as a label for the left one - it sat next to the
    # right panel's own title looking like a pair.
    #
    fig.suptitle("What the Mode switch is worth, once there is gain in front",
                 color=figure.FG, fontsize=10, x=0.01, ha="left")
    return figure.finish(
        fig, ax, out, dpi=200, title=None,
        caption="Silicon stops at -4.9 dBFS, Stacked at +0.4, LED at +2.7 - "
                "7.6 dB of ceiling between the ends of the switch.\nAt "
                "Distortion minimum they are within a decibel of each other, "
                "which is why that setting is the wrong place to look.")


def clamp(out):
    """The transfer curve, where the pedal will show it."""
    fig, ax = figure.figure(width=8.0, height=4.0)
    figure.sweep(ax, A.CLAMP_RUNGS, A.clamp_curves())
    ax.axhline(0.0, color="#b0b0ad", lw=0.9, ls=(0, (1, 2)), zorder=1)
    ax.set_xlabel("in, dBFS")
    ax.set_ylabel("compression, dB")
    return figure.finish(
        fig, ax, out, dpi=200,
        title="The three clamps at Distortion minimum",
        caption="Output against a straight line, so what is drawn is how "
                "far each clamp bends.  The op-amp is a follower here, so "
                "the input drives the diodes through R5 alone.\nNone of the "
                "three goes flat.  LED does not bend at all: it clamps at "
                "1.81 V, above the 1.414 V the input can reach.")


#
# The pedal's five points come from ISSUES 343, measured on the Helios on
# 2026-09-06.  They are typed in rather than measured here because they
# need the pedal, and a chart that cannot be drawn without one is a chart
# that stops being drawn.  Their date is on the picture for the same
# reason.
#
PEDAL_TRAVEL = ((0.0, 50.08), (0.25, 48.07), (0.5, 46.13),
                (0.75, 46.80), (1.0, 47.12))


#
# The deck across the same travel.  Slow - about ninety seconds of
# ngspice, cached after the first run - and worth it, because it is what
# separates "the model has this wrong" from "neither description has
# it", and here it is the second: the netlist misses the pedal's shape
# in the same place the model does.
#
#
# Dense where the shape is and coarse where it is not.  The transition
# happens between 0.25 and 0.30 of the travel and a 0.1 grid draws it as
# one straight segment, which is the same make-believe as joining the
# pedal's five points with a ruler.
#
#
# And very fine through 0.255 to 0.285, because that is where one rail
# engages before the other.  At 0.27 the netlist's negative peak is
# momentarily the larger one - the ratio goes -0.39 dB where it is +0.05
# on one side and +1.49 on the other - so the duty cycle goes *up*
# through 51% before it comes down.  On a 0.01 grid that is one point
# and draws as a spike, which reads as an artefact.  It is not one.
#
FINE = sorted(set(
    [round(0.05 * i, 4) for i in range(5)]
    + [round(0.20 + 0.01 * i, 4) for i in range(6)]
    + [round(0.255 + 0.0025 * i, 4) for i in range(13)]
    + [round(0.30 + 0.01 * i, 4) for i in range(16)]
    + [round(0.45 + 0.05 * i, 4) for i in range(12)]))


def deck_travel():
    import ngspice as NG

    x = np.asarray(loop.tone(220.0, -12.0, 48000), float)
    out = []
    for pot in FINE:
        v = NG.tran("rat-helios", "out", x * VPEAK, fs=FS, settle=0.3,
                    params=dict(dist=max(pot ** 3, 1e-6), filter=0.125,
                                sweep=0.0, mode=1))
        b = v[int(0.5 * len(v)):]
        out.append(100.0 * np.mean((b - b.mean()) > 0))
    return out


def travel(out):
    """The asymmetry across the knob.

    The pedal is five markers and no line.  It was measured at five
    settings and nothing is known about what it does between them;
    joining them with straight segments draws a curve that was never
    measured, and next to two densely sampled curves it reads as
    "the hardware does something else entirely".  It does not - point
    for point at those same five settings the model is within 0.6% at
    four of them.
    """
    duty = [A.tone_shape(A.knobs(Distortion=p))[0] for p in FINE]
    fig, ax = figure.figure(width=8.0, height=4.0)
    figure.leg(ax, FINE, deck_travel(), "netlist")
    figure.leg(ax, FINE, duty, "model", label="[RAT]")
    figure.leg(ax, [p for p, _ in PEDAL_TRAVEL], [d for _, d in PEDAL_TRAVEL],
               "hardware", ls="none", marker="o", ms=5,
               label="the Helios, measured")
    #
    # Held to the range anybody cares about.  Through the transition the
    # netlist's duty cycle swings between 39.6% and 55.3% over a
    # hundredth of the travel, which is not the circuit being violent -
    # it is the statistic being ill-conditioned exactly where the
    # waveform is nearly symmetric and a hair decides which peak is the
    # larger.  Drawn to scale it is a spike four times the height of
    # everything the figure is about.
    #
    ax.set_ylim(45.0, 51.0)
    ax.axhline(50.0, color="#b0b0ad", lw=0.8, zorder=1)
    ax.set_xlabel("Distortion")
    ax.set_ylabel("above zero, %")
    return figure.finish(
        fig, ax, out, dpi=200,
        title="Duty cycle across the Distortion travel, 220 Hz at -12 dBFS",
        caption="Point for point at the pedal's own five settings the model "
                "is within 0.6% at four of them and 1.9% out at 0.25, where "
                "the pedal is part way into a transition the model has not "
                "started.  Both model and netlist turn on more abruptly and "
                "slightly later than the pedal does, and neither reproduces "
                "the rise above noon.\nPedal measured 2026-09-06 at five "
                "settings; there is no line through them because nothing is "
                "known about what happens between.\nThe netlist leaves the "
                "top of the axis for a hundredth of the travel at the "
                "transition, where the waveform is symmetric enough that this "
                "statistic stops being well conditioned.  See ISSUES 343 and "
                "353.")


CAPTURE_HOW = ("Make it with:  ./capture-rat.py <file.json>\n"
               "  which needs the Helios on the bench at Distortion noon, "
               "Filter noon,\n  Sweep minimum, Level maximum, one silicon "
               "pair.")
TAKES_HOW = ("Make them with:  ./shoot-loop.py rat --knob Distortion=1.0 "
             "--against <ref> \\\n                     --out rat-full\n"
             "  which needs the Helios on the bench at full Distortion.\n"
             "  Only the hardware leg is read from disk now; the model "
             "leg is rebuilt.")
SOURCE_HOW = ("The guitar this page is drawn from is not ours to commit.\n"
              "  Any six seconds of dry playing will redraw the shapes; the "
              "hardware\n  leg will not line up with it, so redraw that too "
              "or draw the model alone.")


def main():
    ap = argparse.ArgumentParser(
        description="Draw the figures for Documentation/effects/rat.md.",
        epilog="Both inputs come off the bench and neither is committed - "
               "the captures are large and the guitar recording is not ours "
               "to redistribute.  Each figure says on its face whether it "
               "can be rebuilt.")
    ap.add_argument("--capture", default=os.path.join(HERE, "rat-noon.json"),
                    help="the ladder and waveform capture at Distortion noon")
    #
    # Next to the page, not next to the script.  Validation/figures is
    # gitignored scratch space, and a published page cannot reference
    # scratch space - it renders as a broken image on GitHub and nobody
    # notices locally, where the file is right there.
    #
    ap.add_argument("--out", default=os.path.join(
                        HERE, "..", "Documentation", "effects", "figures"),
                    help="where to write the PNGs")
    ap.add_argument("--ms", type=float, default=70.0,
                    help="how much of the playing figure to draw")
    ap.add_argument("--dpi", type=int, default=200)
    ap.add_argument("--only", action="append", default=None,
                    choices=("edge", "playing", "brightness", "response",
                             "filter", "clamp", "modes", "travel"))
    args = ap.parse_args()

    figures = (("edge", lambda o: edge(o, args.capture), "rat-edge.png"),
               ("playing", lambda o: playing(o, args.ms, args.dpi), "rat-playing.png"),
               ("brightness", bright, "rat-brightness.png"),
               ("response", response, "rat-response.png"),
               ("filter", filt, "rat-filter.png"),
               ("clamp", clamp, "rat-clamp.png"),
               ("modes", modes, "rat-modes.png"),
               ("travel", travel, "rat-travel.png"))
    #
    # A missing input skips its figure rather than ending the run.
    #
    # Three of the eight need the pedal or a guitar recording and five
    # need neither, and aborting at the first missing capture meant a
    # fresh clone drew none of them - including the five it could have.
    # Each skip says what would make it, and the exit status still
    # reports that something did not draw, so this is quieter than a
    # traceback without being silent.
    #
    missing = []
    for key, fn, name in figures:
        if args.only and key not in args.only:
            continue
        try:
            print("  wrote", fn(os.path.join(args.out, name)))
        except SystemExit as e:
            missing.append(key)
            print("  skipped %s - %s" % (key, e))
    if missing:
        print("\n%d of %d figures need an input this tree does not have: %s"
              % (len(missing), len(figures), ", ".join(missing)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
