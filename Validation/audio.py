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


def find_card(name="Pedal"):
    """The ALSA card number, or None if it is not plugged in."""
    try:
        out = subprocess.run(["arecord", "-l"], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    m = re.search(r"^card (\d+): %s\b" % re.escape(name), out, re.M)
    return int(m.group(1)) if m else None


def capture(seconds, card, during=None):
    """Record from the pedal, as a float array of shape (n, 2).

    'during' is called once recording is under way, for the things that
    have to happen while the tape is rolling.

    How long it took in wall-clock is left in capture.elapsed, and that
    is the honest way to measure a stall.  When the pedal stops feeding
    the endpoint, get_output_samples() returns short and the host simply
    waits - the captured audio has no hole in it, the recording just
    takes longer.  Counting samples cannot see that; a clock can.
    """
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
