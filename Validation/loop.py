#!/usr/bin/env python3
#
# The pedal, used as an instrument for measuring an analog pedal.
#
# THE LOOP
#
# The pedal's output goes into the pedal being measured and its output
# comes back into the pedal's input.  With 'USB In = Replace' the host's
# audio is the pedal's input and whatever is in the input jack is
# ignored, and with 'USB L/R Out = Wet/Dry' one capture carries both
# sides of that loop in one frame:
#
#	left    what the pedal sent to its DAC, and so what the pedal
#	        under test was driven with
#	right   the raw ADC, and so what came back
#
# Same frame, same clock, no ordering to get wrong.  Replace never
# touches the Dry tap, because process_output()'s dry argument is the
# sample taken before any USB substitution - which is what makes the two
# channels comparable rather than merely simultaneous.
#
# TWO LEGS, AND THE BYPASS IS WHAT PICKS ONE
#
#	hardware   nothing routed.  The DAC gets the stimulus itself, so
#	           the right channel is the real pedal's answer to it.
#	model      the effect routed.  The left channel is what the model
#	           made of the same stimulus, computed on the same board.
#
# So the two legs differ in one switch and share everything else - the
# converters, the cables, the clock and the stimulus.  Whatever the loop
# does to a signal, it does to both.
#
# WHAT HAS TO BE CHECKED BEFORE ANY OF IT IS BELIEVED
#
# The board must be running this tree (issue 144 has the bench half of
# that argument; this is the hardware half, and it caught a stale [RAT]
# the first time it was asked).  The capture window must hold what it is
# supposed to hold and nothing else - issue 336 is a switch-off click
# inside a window producing a confident wrong answer, and the free check
# is that a sine's crest factor is 1.414 and cannot be anything else.
#
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio
import feed
import pedal
import targets as T

FS = 48000.0
VPEAK = 1.41421356

#
# A sine's crest factor, and how far out is far enough to complain.
#
# 1.414 exactly, for any amplitude, any frequency and any phase, so it
# costs nothing to check and cannot be argued with.  The contaminated
# windows in 336 read 2.03 against a corrected 1.415, so a tolerance of a
# couple of percent separates the two cases with room to spare.
#
CREST_SINE = 1.41421356
CREST_TOL = 0.03


class LoopError(Exception):
    pass


def refuse_if_stale(p):
    """The hardware twin of bench.refuse_if_stale().

    An effect map that has changed renumbers everything after the
    change, so a pot write to a board running yesterday's firmware lands
    on a different effect and says nothing at all.
    """
    want = pedal.elf_build()
    if want is None:
        raise LoopError("no built elf to compare against - run 'make'")
    #
    # Asked more than once before giving up.  The first request after
    # the bench has been idle for a while goes unanswered often enough
    # to have happened twice in one session, and works every time on the
    # next try - so a single miss says nothing about the firmware, and a
    # guard that reports one as a fault is a guard that gets commented
    # out.  A real mismatch answers, and answers wrong.
    #
    got = None
    for _ in range(3):
        got = (pedal.identity(p, wait=3.0) or {}).get("build")
        if got is not None:
            break
    if got is None:
        raise LoopError("the pedal did not answer three identity requests")
    if got != want:
        raise LoopError("the pedal is running %r and this tree builds %r"
                        " - run 'make flash'" % (got, want))
    return got


def configure(p, leg, t=None, knobs=None, settle=0.3):
    """Put the pedal into one of the two legs, from scratch.

    Nothing is assumed about what was routed or what a scene left
    behind: every pot this measurement depends on is written, including
    the ones whose default is what is wanted.
    """
    import time

    if leg not in ("hardware", "model"):
        raise LoopError("no such leg: %r" % (leg,))

    settings = pedal.settings_effect()
    pedal.set_pot(p, settings, pedal.SETTINGS_USB_IN, pedal.USB_IN_REPLACE)
    pedal.set_pot(p, settings, pedal.SETTINGS_USB_OUT, pedal.USB_OUT_WET_DRY)

    #
    # The gate off, and the trim and volume where the bench has them.
    # A gate is the one thing here that would silence exactly the part
    # of a decaying note the measurement is about.
    #
    pedal.set_pot(p, pedal.CHAIN, pedal.CHAIN_GATE, 0)

    if leg == "hardware":
        pedal.set_routing(p)
    else:
        if t is None:
            raise LoopError("the model leg needs a target")
        eff = pedal.effect_id(t["short"])
        pedal.set_routing(p, eff)
        for name, v in (knobs or t["knobs"]).items():
            raw = int(v) if isinstance(t["knobs"][name], int) else round(v * 120)
            idx = pedal.pot_index(t["short"], name)
            if idx is None:
                raise LoopError("%s has no pot %r in the generated map"
                                % (t["short"], name))
            pedal.set_pot(p, eff, idx, raw)

    time.sleep(settle)
    return settings


def crest(x, trim=0.0):
    """Peak over rms, with the peak taken off the analytic envelope.

    1.414 for a sine, and more for anything else - but only if the peak
    is the waveform's and not the sampling grid's.  A 16 kHz sine at
    48 kHz has three samples a cycle and they land where the phase puts
    them, so the largest *sample* can be anywhere from 0.87 to 1.00 of
    the real peak and the crest factor reads between 1.22 and 1.41 for a
    signal that is a perfect sine.  This check refused one, which is how
    it was noticed.

    The envelope has no such opinion: |x + j*H{x}| is the amplitude at
    every instant regardless of where the samples fell, so a tone reads
    1.414 at any frequency below Nyquist and a click still reads high.
    """
    x = np.asarray(x, dtype=float)
    env = envelope(x)
    if trim > 0.0:
        #
        # The envelope is built on the whole window and only then looked
        # at in the middle.  Slicing first is much worse than not
        # trimming at all: the transform reads a truncated segment as
        # periodic, and the discontinuity at the join puts an envelope
        # spike at each end - a 220 Hz tone trimmed before the transform
        # reads 4.28 instead of 1.41.
        #
        # What is being trimmed away is the stimulus's own fade in and
        # out.  A raised-cosine edge is a real amplitude change, and at
        # 40 Hz five milliseconds is a fifth of a cycle, which is too
        # fast for an envelope to be meaningful across.
        #
        k = int(trim * len(x))
        env, x = env[k:len(x) - k], x[k:len(x) - k]
    r = float(np.sqrt(np.mean(x ** 2)))
    if r <= 0:
        return float("inf")
    return float(env.max()) / r


def envelope(x):
    """|x + j H{x}|, the amplitude at every instant.

    The analytic signal the textbook way: keep DC and Nyquist, double
    the positive frequencies, drop the negative ones.
    """
    n = len(x)
    h = np.zeros(n)
    h[0] = 1.0
    if n % 2 == 0:
        h[n // 2] = 1.0
        h[1:n // 2] = 2.0
    else:
        h[1:(n + 1) // 2] = 2.0
    return np.abs(np.fft.ifft(np.fft.fft(x) * h))


def check_sine_window(x, what="window"):
    """Refuse a window that is not the tone it is supposed to be.

    Cheap, and it is the whole of 336: a switch-off click inside the
    window makes every peak and every rms describe the click, and
    nothing else complains because a peak is a peak.

    This is a check on the *sent* channel and not on the returned one.
    What comes back has been through the pedal being measured, and a
    pedal that is doing anything at all has made it something other than
    a sine - a Helios with its Distortion up returns 1.46 against 1.42,
    and that is the measurement rather than a fault in it.
    """
    c = crest(x, trim=0.03)
    if abs(c - CREST_SINE) > CREST_TOL:
        raise LoopError("%s has crest factor %.3f, not a sine's 1.414 -"
                        " something other than the tone is in it" % (what, c))
    return c


def chirp(n, f0=20.0, f1=22000.0, dbfs=-14.0, ramp=0.02):
    """A log sweep, so one capture carries the whole band."""
    t = np.arange(n) / FS
    k = (n / FS) / np.log(f1 / f0)
    x = np.sin(2.0 * np.pi * f0 * k * (np.exp(t / k) - 1.0))
    x *= 10.0 ** (dbfs / 20.0)
    r = int(ramp * FS)
    if r > 0 and 2 * r < n:
        w = 0.5 - 0.5 * np.cos(np.pi * np.arange(r) / r)
        x[:r] *= w
        x[-r:] *= w[::-1]
    return x


def calibrate(card, seconds=1.0):
    """What the loop itself does, with the pedal under test bypassed.

    Returns (delay_samples, freqs, H).  The delay is the slope of the
    unwrapped phase, which is the whole of what has to come out of a
    waveform before two of them can be laid on top of each other.

    Measured on this bench with a Helios in true bypass: 52.92 samples
    of round trip, the same to two decimals over four captures, and once
    that pure delay is removed the loop is flat to 0.4 degrees
    everywhere above 1 kHz and down 0.24 to 0.59 dB from 30 Hz to
    20 kHz.  So the correction an overlay needs is a delay and not a
    filter - which is worth knowing, because a deconvolution would have
    been the expensive way to discover the same thing.

    The residual below 200 Hz is larger (6 degrees, spreading 16) and is
    the analog path's DC blocking, which is where a high-pass puts its
    phase.
    """
    x = chirp(int(seconds * FS))
    sent, back = play_capture(card, x)
    n = len(x)
    X, Y = np.fft.rfft(sent), np.fft.rfft(back)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    m = (f > 30) & (f < 20000) & (np.abs(X) > np.abs(X).max() * 1e-3)
    ph = np.unwrap(np.angle(Y[m] / X[m]))
    a = np.vstack([f[m], np.ones(m.sum())]).T
    slope = np.linalg.lstsq(a, ph, rcond=None)[0][0]
    return -slope / (2.0 * np.pi) * FS, f[m], Y[m] / X[m]


def measure_tone(card, hz, dbfs, seconds=0.5, tries=4, report=None):
    """One tone through the loop, with a window that passed its check.

    A capture goes wrong occasionally - a window comes back with
    something in it that is not the tone, and reads a crest factor of
    1.6 where the stimulus reads 1.41.  It is not frequent and it is not
    systematic, and both of those are reasons to re-take rather than to
    widen the tolerance: a check that is loose enough to pass every
    window is not checking anything, and the window that got refused
    here would have been believed.

    Returns (sent, returned).  Raises only if every attempt is refused,
    which is then a fault rather than a glitch.
    """
    x = tone(hz, dbfs, int(seconds * FS))
    last = None
    for attempt in range(tries):
        sent, back = play_capture(card, x)
        try:
            check_sine_window(sent, "the %g Hz window" % hz)
        except LoopError as e:
            last = e
            if report:
                report("   retaking %g Hz: %s" % (hz, e))
            continue
        return sent, back
    raise LoopError("%g Hz refused %d times; last was: %s"
                    % (hz, tries, last))


def check_linear(card, hz=220.0, at=(-30.0, -24.0), tol_db=1.0,
                 seconds=0.5):
    """Refuse to measure a frequency response through a clipping pedal.

    A response sweep assumes the thing it sweeps is linear, and says
    nothing about whether it is.  Measured through a Helios with its
    Distortion up, seven frequencies came back flat to a quarter of a
    decibel and looked like a clean buffer - because every one of them
    was sitting on the clipping ceiling, which is flat.  The output was
    the same at -30 dBFS in as at 0.

    So: two levels six decibels apart, and the output has to move by six
    decibels too.  That is 316's lesson as a precondition rather than as
    a thing to remember - the instrument can express the answer, so it
    should be made to give it before anything downstream is believed.
    """
    got = []
    for dbfs in at:
        x = tone(hz, dbfs, int(seconds * FS))
        sent, back = play_capture(card, x)
        got.append(tone_db(back, hz) - tone_db(sent, hz))
    want = at[1] - at[0]
    moved = (got[1] + at[1]) - (got[0] + at[0])
    if abs(moved - want) > tol_db:
        raise LoopError(
            "the output moved %.2f dB for %.2f dB of input - the pedal is "
            "not linear here, so a response sweep would be measuring its "
            "clipping ceiling" % (moved, want))
    return moved


def tone(hz, dbfs, n, ramp=0.005):
    """One tone, with edges soft enough not to be a click of their own.

    dbfs is the peak, which is bench.tone()'s convention and so the one
    every level in this directory is quoted in: 0 dBFS is the sine that
    just touches full scale, which is one volt rms out of the DAC.
    Reading it as an rms instead builds a 0 dBFS tone with a peak of
    1.414 and hands the converter something it cannot play - which the
    window check refused four times running before this was fixed.
    """
    t = np.arange(n) / FS
    x = np.sin(2.0 * np.pi * hz * t) * (10.0 ** (dbfs / 20.0))
    r = int(ramp * FS)
    if r > 0 and 2 * r < n:
        w = 0.5 - 0.5 * np.cos(np.pi * np.arange(r) / r)
        x[:r] *= w
        x[-r:] *= w[::-1]
    return x


def play_capture(card, x, repeats=3, lead=0.4, tol_db=6.0):
    """Push x through the loop and hand back (sent, returned).

    The stimulus is played several times and a whole pass taken out of
    the middle, so what is measured is a loop in steady state rather
    than whatever aplay was doing while it filled its buffer.

    The window is checked before it is returned.  Correlation always
    reports a best position, including when the best position is
    silence, and a silent window reads as a working measurement of a
    dead pedal - which is 336 from the other end.  So the pass that gets
    returned has to carry the level the stimulus was played at.
    """
    #
    # Say who has the device before arecord says it cannot have it.
    #
    # Everything in this directory opens the raw ALSA device and needs
    # it exclusively, and anything that monitors an input holds it: a
    # PipeWire loopback, or - the one that actually happened - the GNOME
    # sound panel left open, whose level meter keeps the source running.
    # What comes back without this is "Device or resource busy", which
    # reads as a broken pedal rather than as a window that wants closing.
    #
    busy = feed.capture_busy(card)
    if busy:
        raise LoopError("%s is holding the capture stream on card %d; "
                        "close whatever is monitoring the input (a sound "
                        "settings panel counts)" % (busy, card))

    blob = feed.stereo_s32(x)
    n = len(x)
    seconds = int(np.ceil(lead + repeats * n / FS)) + 1
    cap = audio.capture(seconds, card,
                        during=lambda: feed.player(card, blob, repeats))
    wet, dry = cap[:, 0], cap[:, 1]
    if len(wet) < 2 * n:
        raise LoopError("captured %d samples for a %d-sample stimulus"
                        % (len(wet), n))

    #
    # Every pass that fits, scored by how much of the stimulus's own
    # level it actually has.  Taking the correlation peak and stepping
    # one pass on was what put a window in the silence after the last
    # pass, twice out of seven, and said nothing about it.
    #
    # The candidates come from the lag modulo the stimulus length rather
    # than from the lag itself.  A repeating stimulus correlates equally
    # well against every pass, so which one the peak lands on is
    # arbitrary and can come back wrapped and negative - a 1 s chirp in
    # a 5 s capture reported -164179, whose only defect is that it is
    # 27821 minus four passes.  The remainder is the phase, and the
    # passes are what follow from it.
    #
    want = float(np.sqrt(np.mean(x ** 2)))
    #
    # max_lag is the stimulus length and not the whole capture.
    #
    # align() sizes its transform at len(stimulus) + max_lag and then
    # reads the correlation out of c[-max_lag:] and c[:max_lag].  Ask it
    # to search the entire capture and those two slices overlap, and the
    # peak it reports is wherever the wrap happened to land.  A repeating
    # stimulus only ever needs its phase, which is one period.
    #
    # Tones did not notice.  Any lag that is right modulo the period
    # gives a valid window of a periodic signal, so every measurement
    # taken this way was fine and a guitar passage came back several
    # seconds out of step with the same passage through the bench.
    #
    lag = feed.align(x, wet, max_lag=n) % n
    best, best_err = None, None
    for a in range(lag, len(wet) - n + 1, n):
        got = float(np.sqrt(np.mean(wet[a:a + n] ** 2)))
        err = abs(20.0 * np.log10(max(got, 1e-12) / max(want, 1e-12)))
        if best_err is None or err < best_err:
            best, best_err = a, err
    if best is None:
        raise LoopError("no whole pass of the stimulus fits in the capture")
    if best_err > tol_db:
        raise LoopError("the best window is %.1f dB off the level the "
                        "stimulus was played at - it does not hold the tone"
                        % best_err)
    return wet[best:best + n], dry[best:best + n]


def tone_db(x, hz, bins=4.0):
    """The level at one frequency, as a band wide enough to hold it.

    audio.tone_level()'s default band is 2 % of the frequency, which is
    right well up the band and narrower than a single FFT bin down at
    the bottom of it: 60 Hz in a 0.4 s window asks for +/-1.2 Hz out of
    a 2.5 Hz grid and reads 2.4 dB low for no reason but arithmetic.

    So the band is whichever is wider - 2 %, or enough bins to hold a
    windowed tone's skirt.
    """
    frac = max(0.02, bins * (FS / len(x)) / float(hz))
    return audio.tone_level(x, hz, frac=frac)

#
# TAKING THE BENCH BACK OUT OF A CAPTURE
#
# Everything measured through this loop arrives having been through the
# receiving board's analog input, whose coupling capacitor into the
# codec is a single-pole high-pass.  test-loop.py measures it - that is
# what its corner_hz is - and ISSUES 91 settled the current hardware at
# 9.28 Hz across three board generations, predicted before the third was
# built.
#
# It is 0.008 dB at 220 Hz, so nothing that measures a level or a
# harmonic has ever needed to care.  What it is not small in is the time
# domain: tau is 17 ms, so across a 2.27 ms half cycle a held value
# decays 12 %, and a clipped waveform's flat top tilts by 1.1 dB.  That
# is a shape rather than a level, and it is entirely a property of the
# bench rather than of whatever is being measured.
#
# So a capture can be corrected back to what an ideal one would have
# caught, which is what makes a number about a pedal rather than about a
# pedal and this rig together.  Two rigs would then agree.
#
# THE LEAK IS NOT OPTIONAL
#
# The exact inverse of a high-pass has a pole at DC and unbounded gain
# there, so any offset or drift in a capture integrates away.  Moving
# that pole to 'leak' bounds the boost at fc/leak and leaves everything
# above fc untouched.  Measured on a synthetic square, round-tripped:
#
#	leak      1.0 Hz   0.3 Hz   0.1 Hz
#	error      1.25 %   0.41 %   0.14 %
#	boost     19.4 dB  29.8 dB  39.4 dB
#
# WHERE IT IS SAFE, AND WHERE IT WANTS WATCHING
#
# On steady tones and bursts it is safe.  On six seconds of real
# playing, 64 % of what it adds lands below 20 Hz where 0.9 % of the
# signal is, and the baseline wander over 50 ms blocks doubles - which
# is exactly the quantity a droop or a duty cycle is measured against.
# So uncolour a burst freely and think before uncolouring a chord.
#
# The real fix is hardware and is planned rather than done: DC-coupling
# the codec inputs removes the pole entirely, and the next-generation
# board wants that anyway for its headphone amplifier.  With a 1 Mohm
# input and only a DC-blocking capacitor in front of it, what is left is
# too small to correct for.
#
INPUT_CORNER_HZ = 9.28


def measured_corner(path):
    """The receiving board's input corner, from a test-loop.py run.

    Prefer this to the constant above whenever a measurement is to hand:
    the constant is one board's number and test-loop.py's own caveats
    say corner_hz tracks the receiving board.  Returns (corner, residual)
    so a caller can refuse a bad fit rather than quote it.
    """
    import json

    with open(path) as f:
        d = json.load(f)
    links = d.get("links") or []
    if not links:
        raise LoopError("%s has no links in it" % path)
    fc = float(np.mean([l["corner_hz"] for l in links]))
    resid = max(float(l["fit_residual_db"]) for l in links)
    return fc, resid


def colour(x, fc=INPUT_CORNER_HZ):
    """Put the input high-pass *on* a signal that never went through it.

    The other way round from uncolour(), and the one to reach for when
    the capture is broadband: there is no deconvolution here and so no
    boost, no leak and nothing to go wrong.  What it gives up is that
    the result describes the pedal as this bench would have measured it
    rather than the pedal, so it belongs in a figure that sits beside a
    capture and not in a number that gets quoted.
    """
    x = np.asarray(x, dtype=float)
    a = np.exp(-2.0 * np.pi * fc / FS)
    y = np.empty_like(x)
    prev_x = prev_y = 0.0
    for i, v in enumerate(x):
        prev_y = a * (prev_y + v - prev_x)
        prev_x = v
        y[i] = prev_y
    return y


def uncolour(x, fc=INPUT_CORNER_HZ, leak=0.1):
    """Undo the input high-pass a capture came through.

    Returns what an ideal capture would have caught.  See the note above
    for why the leak is there and where this is safe.
    """
    x = np.asarray(x, dtype=float)
    a = np.exp(-2.0 * np.pi * fc / FS)
    p = np.exp(-2.0 * np.pi * leak / FS)
    y = np.empty_like(x)
    prev_x = prev_y = 0.0
    for i, v in enumerate(x):
        prev_y = (v - a * prev_x) / a + p * prev_y
        prev_x = v
        y[i] = prev_y
    return y
