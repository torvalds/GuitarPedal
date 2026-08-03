#
# Capturing and measuring the pedal's USB audio.
#
# The pedal is a USB audio device in both directions, so anything it
# does to a signal can be measured from here without a guitar, an
# amplifier, or a trip to another room.  What it cannot measure is the
# analog output, because that is fed by the audio core straight into
# i2s DMA - see the note on save_gap() below.
#
# The interesting capture mode is 'Wet/Dry' on the USB output, which
# puts the processed signal on the left channel and the *raw ADC sample*
# on the right.  Both come from the same instant, so a null test needs
# no alignment and has no clock drift to worry about: subtract one from
# the other and what is left is what the pedal did.
#
# That only holds for an analog source.  A signal injected over USB
# audio never reaches the dry tap - process_output() takes the untouched
# input - so injected signals have to be compared against the host's own
# copy instead.
#
import re
import subprocess
import sys
import threading
import time
import wave

import numpy as np

RATE = 48000

#
# What the internal -1.0 .. 1.0 scale is supposed to mean.
#
# audio/process.h arranges things so that a 1Vrms sine peaks at 1.0, and
# every effect's dB markings are denominated in that - it is what Trim
# exists to put a pickup onto.  It was set by hand off an oscilloscope,
# so it is a claim to be checked rather than a constant to trust.
#
FULL_SCALE_VRMS = 1.0

#
# What the two USB channels are actually in.
#
# 'Wet/Dry' puts the processed signal on the left and the raw ADC sample
# on the right, and those are not in the same units.  process_input()
# scales a full-scale sample to 1.2198 and convert_output() scales 1.0
# back to full scale, so the two are deliberately not inverse - that
# asymmetry is precisely what makes the peak of the internal float equal
# the RMS volts of a sine, which is what "0dBFS is 1Vrms" means.
#
# So the left channel already *is* the internal float, and the right one
# has to be multiplied by this to be compared with it.  Getting that
# wrong makes a perfectly transparent pedal look 1.7dB off.
#
SAMPLE_TO_FLOAT = 3.45 / 2.82843


def find_cards(match=""):
    """Every pedal arecord can see, as [(card number, name)].

    A card line carries a short name and a longer one in brackets, and
    which of them the USB product string ends up in depends on whether the
    product starts with the manufacturer - "Linus Pedal" reduced to
    "Pedal", "TAC5242 Pedal" does not.  So the match is against the whole
    line rather than either field, and the name handed back is the
    bracketed one, which is the full product string and therefore names
    the codec.

    'match' narrows that further, and is how a caller says which pedal it
    means when there is more than one: "TAC5242", or "5112".
    """
    try:
        out = subprocess.run(["arecord", "-l"], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return []

    found = {}
    for line in out.splitlines():
        m = re.match(r"card (\d+): (\S+) \[([^]]*)\]", line)
        if not m or "pedal" not in line.lower():
            continue
        if match.lower() not in line.lower():
            continue
        # One line per device, and only the first device is the audio one
        found.setdefault(int(m.group(1)), m.group(3))
    return sorted(found.items())


def find_card(match=""):
    """One pedal's ALSA card number, or None if it is not plugged in."""
    cards = find_cards(match)
    return cards[0][0] if cards else None


def discard(card):
    """One capture thrown away, to get the stale one out of the way.

    The first arecord after the device has been left alone for even a
    second comes back holding audio from *before* it was left alone.
    Measured: set a pot, wait, capture, and the capture shows the old
    value; capture again and it shows the new one, and every capture
    after that is right until the next pause.

    pipewire is what is doing it.  It has the pedal open - `pactl list
    short sources` shows an alsa_input for each one, SUSPENDED - and the
    backlog it hands over is about a second, which is far more than the
    three packets the pedal's own endpoint holds.

    This is worth knowing beyond the value being wrong, because a stale
    capture is not merely late: it was recorded across the moment the
    device was picked up again, so it is also where the discontinuities
    come from that get counted as breaks, and then as noise, and then as
    distortion.  A test that measures the first capture after a gap is
    measuring the gap.
    """
    subprocess.run(
        ["arecord", "-D", f"hw:{card},0", "-f", "S32_LE", "-c", "2",
         "-r", str(RATE), "-d", "1", "-t", "raw", "-q"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def capture(seconds, card, during=None, warm=True):
    """Record from the pedal, as a float array of shape (n, 2).

    'during' is called once recording is under way, for the things that
    have to happen while the tape is rolling.

    'warm' throws one capture away first - see discard().  It is on by
    default because the alternative default is a number that is silently
    a second out of date, and costs a second per capture, which is worth
    it.  Turn it off only when the staleness is the thing being measured.

    How long it took in wall-clock is left in capture.elapsed, and that
    is the honest way to measure a stall.  When the pedal stops feeding
    the endpoint, get_output_samples() returns short and the host simply
    waits - the captured audio has no hole in it, the recording just
    takes longer.  Counting samples cannot see that; a clock can.

    The warm-up is deliberately outside that clock: it is the host being
    got ready, not the pedal being measured.
    """
    if warm:
        discard(card)

    t0 = time.time()
    proc = subprocess.Popen(
        ["arecord", "-D", f"hw:{card},0", "-f", "S32_LE", "-c", "2",
         "-r", str(RATE), "-d", str(int(round(seconds))), "-t", "raw", "-q"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    #
    # In a thread, because arecord writes to a pipe and a pipe holds 64kB
    # - a fraction of a second of audio.  Anything done here before the
    # reading starts blocks arecord instead of the pedal, and then the
    # clock below measures this process rather than the device.
    #
    if during:
        threading.Thread(target=during, daemon=True).start()

    raw, err = proc.communicate()
    if proc.returncode:
        sys.exit(f"audio: arecord failed:\n{err.decode()}")

    d = np.frombuffer(raw, dtype="<i4").reshape(-1, 2).astype(np.float64)
    capture.elapsed = time.time() - t0
    return d / 2**31


def wav(path):
    """A capture from disk, for looking at one again later."""
    w = wave.open(path)
    d = np.frombuffer(w.readframes(w.getnframes()), dtype="<i4")
    return d.reshape(-1, 2).astype(np.float64) / 2**31


def peak(x):
    return float(np.abs(x).max())


def rms(x):
    return float(np.sqrt(np.mean(x * x)))


def dbfs(v):
    return -np.inf if v <= 0 else 20.0 * np.log10(v)


def dominant(x):
    """The strongest frequency present, in Hz.

    Parabolic interpolation across the peak bin, so a one-second window
    resolves rather better than its 1Hz bin spacing - which matters when
    the thing being checked is whether the sample clock is right.
    """
    sp = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    k = int(np.argmax(sp))
    if 0 < k < len(sp) - 1:
        a, b, c = sp[k - 1], sp[k], sp[k + 1]
        denom = a - 2 * b + c
        k = k + 0.5 * (a - c) / denom if denom else k
    return float(k) * RATE / len(x)


def thd(x, f0, harmonics=5):
    """Harmonic distortion, in dB below the fundamental."""
    sp = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    step = RATE / len(x)

    def near(f):
        k = int(round(f / step))
        return float(sp[max(0, k - 2):k + 3].max()) if k < len(sp) else 0.0

    fund = near(f0)
    if fund <= 0:
        return -np.inf
    power = sum(near(f0 * n) ** 2 for n in range(2, harmonics + 1))
    return dbfs(np.sqrt(power) / fund)


#
# What is left when the signal is taken away.
#
# The generator cannot be switched off from here, so the tone is removed
# arithmetically instead: fit it by least squares at its measured
# frequency, subtract, and what remains is noise plus whatever the fit
# could not describe.  Harmonics are fitted too and reported separately,
# because distortion is not noise and lumping them together flatters
# neither.
#
# Assumes the generator itself is clean.  It is not, so this is an upper
# bound on the pedal's own noise rather than a measurement of it - what
# comes out is "no worse than this".
#
def noise_floor(x, f0=None, harmonics=8, guard=12):
    """Everything that is not the tone or its harmonics.

    Done in the frequency domain by leaving bins out rather than in the
    time domain by fitting and subtracting.  A fit has to know the
    frequency exactly: a thousandth of a Hz of error walks a quarter
    turn out of phase over five seconds and the "residual" it leaves is
    mostly signal.  Dropping bins does not care, which matters when the
    frequency is a dial on a bench rather than a number.

    'guard' bins either side of each harmonic go too, because a Hann
    window spreads a pure tone over about three.
    """
    if f0 is None:
        f0 = dominant(x)

    #
    # Blackman-Harris rather than Hann, because this is looking for
    # something a hundred dB down from a tone that is present.  Hann's
    # sidelobes put the tone's own leakage at about -86dBFS across the
    # whole spectrum, and a noise floor measured through that is a
    # measurement of the window.  Blackman-Harris buries its sidelobes
    # near -92dB and the leakage stops being what is being measured.
    #
    # The cost is a wider main lobe - eight bins rather than three - so
    # the guard has to widen with it.
    #
    n = np.arange(len(x))
    z = 2 * np.pi * n / len(x)
    win = (0.35875 - 0.48829 * np.cos(z) + 0.14128 * np.cos(2 * z)
           - 0.01168 * np.cos(3 * z))
    gain = np.sqrt(np.mean(win ** 2))
    sp = np.fft.rfft(x * win) / (len(x) / 2) / gain
    power = np.abs(sp) ** 2
    step = RATE / len(x)

    def band(f):
        k = int(round(f / step))
        return slice(max(0, k - guard), min(len(power), k + guard + 1))

    fund = np.sqrt(power[band(f0)].sum() / 2)
    harm = 0.0
    keep = np.ones(len(power), dtype=bool)
    keep[:2] = False                      # DC and the bin next to it
    keep[band(f0)] = False
    for h in range(2, harmonics + 1):
        if f0 * h < RATE / 2:
            harm += power[band(f0 * h)].sum() / 2
            keep[band(f0 * h)] = False

    noise = np.sqrt(power[keep].sum() / 2)
    harm = np.sqrt(harm)

    return {
        "noise_dbfs": dbfs(noise),
        "noise_vrms": noise * FULL_SCALE_VRMS,
        "snr_db": dbfs(fund) - dbfs(noise),
        "harmonics_db": dbfs(harm) - dbfs(fund),
        "density_dbfs": dbfs(noise / np.sqrt(RATE / 2)),
        "f0": f0,
    }


def ratio_db(a, b):
    """How much louder a is than b."""
    return dbfs(rms(a)) - dbfs(rms(b))


def null_db(a, b):
    """What is left after subtracting one from the other, relative to b.

    The transparency measure: two signals that are the same cancel to
    nothing, and how far down the residue sits is how transparent
    whatever came between them was.
    """
    return dbfs(rms(a - b)) - dbfs(rms(b))


def trim(x, edge=0.05):
    """Drop the ends of a capture.

    arecord's first moments catch the stream starting up, and a test
    that measures that instead of the signal is measuring the operating
    system.
    """
    n = int(len(x) * edge)
    return x[n:len(x) - n]


#
# Where a steady tone stopped being steady.
#
# A pure tone can only move so far between one sample and the next -
# amplitude times 2*pi*f/rate - so anything moving a lot further than
# that is a break in the stream rather than a signal.  Crude, and it
# needs no assumptions beyond "this is a sine", which is exactly what is
# on the input.
#
# Phase unwrapping was the first attempt and was harder to trust: it
# reports a number for every sample, so a noisy small signal produces
# candidates that have to be thresholded anyway, and the threshold is
# less obvious than "thirty times faster than physically possible".
# What it deliberately does *not* report is how many samples went
# missing.  Phase unwrapping can only see gaps shorter than one period -
# 109 samples at 440Hz - and a stall worth measuring is far longer than
# that, so the answer would wrap and look small.  What is reported
# instead is when each break happened and how big the step was, and
# breaks close together are grouped: the span of a burst is the length
# of the disturbance, which is the number somebody recording through the
# pedal actually experiences.
#
# What this measures is core 0.  usb_output is a 512-sample ring that
# the audio core fills and core 0 drains from the main loop, resyncing
# to the half-buffer mark when it is more than three quarters full.  So
# this sees anything that stops core 0 - a flash write, most obviously -
# and sees nothing at all about the analog output, which the audio core
# feeds through DMA without core 0 in the path.
#
def discontinuities(x, f0, factor=6.0):
    """Every break in a steady tone, as {at_ms, lost_samples, size}."""
    amp = peak(x)
    slew = amp * 2 * np.pi * f0 / RATE
    if slew <= 0:
        return []

    dif = np.abs(np.diff(x))
    idx = np.flatnonzero(dif > factor * slew)
    if not len(idx):
        return []

    # Neighbouring samples are one event.
    events, start = [], idx[0]
    for a, b in zip(idx, idx[1:]):
        if b - a > 2:
            events.append((start, a))
            start = b
    events.append((start, idx[-1]))

    return [{"at_ms": a * 1000.0 / RATE,
             "size": float(dif[a:b + 1].max() / slew)}
            for a, b in events]


def bursts(events, join_ms=15.0):
    """Breaks close together, as one disturbance each.

    A stall does not produce a single clean edit - the ring overflows,
    resyncs, and keeps doing it until core 0 comes back - so what
    matters is how long the mess lasted rather than how many edges it
    had.  Anything within join_ms of the last one is the same event.
    """
    if not events:
        return []
    out = [[events[0], events[0]]]
    for e in events[1:]:
        if e["at_ms"] - out[-1][1]["at_ms"] <= join_ms:
            out[-1][1] = e
        else:
            out.append([e, e])
    return [{"at_ms": a["at_ms"],
             "span_ms": b["at_ms"] - a["at_ms"],
             "breaks": 1 + sum(1 for e in events
                               if a["at_ms"] <= e["at_ms"] <= b["at_ms"]) - 1}
            for a, b in out]


def hilbert_like(x):
    """Quadrature partner of x, via the FFT.

    scipy would have hilbert(), and numpy alone is one less thing to
    have installed on a machine whose whole job here is running arecord.
    """
    n = len(x)
    sp = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0:
        h[n // 2] = 1
        h[1:n // 2] = 2
    else:
        h[1:(n + 1) // 2] = 2
    return np.fft.ifft(sp * h)
