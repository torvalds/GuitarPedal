#!/usr/bin/env python3
#
# How much of the signal survives the trip out through the DAC, down a
# cable, and back in through the ADC.
#
# test-bench.py checks that the pedal's arithmetic matches the host's,
# and it does that entirely in the digital domain - test tone in,
# USB capture out, no analog anywhere.  That is the right way to check
# the DSP and it says nothing at all about the thing the pedal is
# actually plugged into.  This is the other half.
#
# WHAT IT NEEDS
#
# A patch cable from the pedal's output back to its own input.  One
# board, one cable, nothing else: no generator, no second pedal, no
# second clock.  That last part is what makes the measurement easy -
# the DAC and the ADC are the same codec on the same clock, so there is
# no drift to chase and no resampling to argue about.
#
# HOW IT READS
#
# 'Wet/Dry' on the USB output puts the processed signal on the left
# channel and the *raw ADC sample* on the right, both from the same
# instant.  So the left is what we sent to the DAC and the right is what
# came back, and the only things between them are a converter, a cable
# and another converter.
#
# The two are not in the same units - see the note at the top of
# audio.py - so the right one is scaled by SAMPLE_TO_FLOAT first.
#
# The analog path is then a gain and a delay, and both are removed
# before anything is called a residual.  The delay is solved from the
# phase of the fundamental rather than searched for: at 440 Hz one
# sample is 3.3 degrees, so a search on a 0.01-sample grid leaves 0.03
# degrees of phase error behind, which is -65 dB of residual - larger
# than the distortion being measured.  That mistake is easy to make and
# looks like a result.
#
import sys
import time

try:
    import numpy as np
except ImportError:
    print("test-analog: SKIPPED - no numpy")
    sys.exit(0)

import audio
import pedal

CHAIN, TESTTONE, SETTINGS = 0, 16, 18
CHAIN_GATE, CHAIN_TRIM, CHAIN_VOLUME = 1, 4, 5
TT_LEVEL, TT_FREQ, TT_SHAPE = 1, 2, 3
SETTINGS_USB_OUT, USB_OUT_WET_DRY = 1, 3

TONE_HZ = 440.0
SHAPE_SINE, SHAPE_NOISE = 0, 3


def configure(p, level, shape):
    pedal.set_routing(p, TESTTONE)
    time.sleep(0.2)
    for eff, pot, val in ((CHAIN, CHAIN_GATE, 0),
                          (CHAIN, CHAIN_TRIM, 60),
                          (CHAIN, CHAIN_VOLUME, 80),
                          (TESTTONE, 0, 120),
                          (TESTTONE, TT_FREQ, 60),      # 440 Hz exactly
                          (TESTTONE, TT_SHAPE, shape),
                          (TESTTONE, TT_LEVEL, level),
                          (SETTINGS, SETTINGS_USB_OUT, USB_OUT_WET_DRY)):
        pedal.set_pot(p, eff, pot, val)
        time.sleep(0.02)
    time.sleep(1.0)


def round_trip(card):
    """(sent, returned), both in the pedal's internal float units."""
    y = audio.capture(3, card)
    mid = y[len(y) // 4: len(y) // 4 * 3]
    return mid[:, 0], mid[:, 1] * audio.SAMPLE_TO_FLOAT


def loopback_present(p, card):
    configure(p, 96, SHAPE_SINE)
    sent, back = round_trip(card)
    n = len(back)
    b0 = int(round(TONE_HZ * n / audio.RATE))
    mag = np.abs(np.fft.rfft(back)) * 2 / n
    peak = int(np.argmax(mag))
    return abs(peak - b0) <= 2 and 20 * np.log10(mag[peak] + 1e-30) > -60


def main():
    found = pedal.discover()
    if not found:
        print("test-analog: SKIPPED - no pedal on the USB")
        return 0
    if len(found) > 1:
        print("test-analog: SKIPPED - %d pedals; this wants exactly one" % len(found))
        return 0

    d = found[0]
    p, card = d["port"], d["card"]
    print("test-analog: %s, card %d, port %s" % (d["label"], card, p))

    if not loopback_present(p, card):
        print("test-analog: SKIPPED - the ADC cannot hear the tone, so there is")
        print("             no patch cable from the output back to the input")
        return 0

    print("            output patched back to input; [TESTTONE] 440 Hz out the")
    print("            DAC, back in the ADC, both read off one USB capture")
    print()

    #
    # Linearity first.  A converter that is doing its job is a constant
    # gain, and the interesting question is over how much of the range.
    #
    print("  %9s %11s %10s %9s %10s" %
          ("sent", "returned", "gain", "THD", "noise"))
    gains = []
    for level, dbfs in ((36, -63.0), (60, -45.0), (84, -27.0),
                        (96, -18.0), (108, -9.0), (114, -4.5)):
        configure(p, level, SHAPE_SINE)
        sent, back = round_trip(card)
        n = len(back)
        b0 = int(round(TONE_HZ * n / audio.RATE))
        ms, mb = (np.abs(np.fft.rfft(x)) * 2 / n for x in (sent, back))
        grid = np.zeros(len(mb), dtype=bool)
        grid[::b0] = True
        grid[0] = True
        harm = np.sqrt(sum(mb[b0 * i] ** 2 for i in range(2, 13)))
        gain = 20 * np.log10(mb[b0] / ms[b0])
        gains.append(gain)
        print("  %8.1f%s %10.2f%s %8.2f%s %8.1f%s %9.1f%s" %
              (dbfs, " dBFS", 20 * np.log10(mb[b0]), " dBFS", gain, " dB",
               20 * np.log10(harm / mb[b0]), " dB",
               20 * np.log10(np.sqrt((mb[~grid] ** 2).sum())), " dBFS"))

    spread = max(gains) - min(gains)
    print()
    print("  gain is constant to %.3f dB across 58 dB of level" % spread)

    #
    # ...and then the null, which is the whole question in one number.
    #
    configure(p, 96, SHAPE_SINE)
    sent, back = round_trip(card)
    n = len(sent)
    b0 = int(round(TONE_HZ * n / audio.RATE))
    S, Bk = np.fft.rfft(sent), np.fft.rfft(back)
    f = np.fft.rfftfreq(n)

    #
    # Solved, not searched - see the header.
    #
    H = Bk[b0] / S[b0]
    lag = -np.angle(H) / (2 * np.pi * b0 / n)
    aligned = np.fft.irfft(S * np.exp(-2j * np.pi * f * lag), n) * abs(H)

    resid = back - aligned
    sig = np.sqrt((back ** 2).mean())
    res = np.sqrt((resid ** 2).mean())

    m = np.abs(np.fft.rfft(resid)) * 2 / n
    fund = np.abs(Bk[b0]) * 2 / n
    grid = np.zeros(len(m), dtype=bool)
    grid[::b0] = True
    grid[0] = True

    print()
    print("  nulled against what was sent, at -18 dBFS:")
    print("    delay              %8.4f samples = %.4f ms" % (lag, lag / 48.0))
    print("    gain               %+8.4f dB" % (20 * np.log10(abs(H))))
    print("    residual           %8.1f dB below the signal = %.4f%%"
          % (20 * np.log10(res / sig), 100 * res / sig))
    print("      of which")
    print("      harmonics 2..12  %8.1f dB   converter distortion"
          % (20 * np.log10(np.sqrt(sum(m[b0 * i] ** 2 for i in range(2, 13))) / fund)))
    print("      everything else  %8.1f dB   noise, at %.1f dBFS absolute"
          % (20 * np.log10(np.sqrt((m[~grid] ** 2).sum()) / fund),
             20 * np.log10(np.sqrt((m[~grid] ** 2).sum()))))

    #
    # The latency again, independently.  delay_samples() wants a
    # broadband reference and the test tone has a Noise shape for
    # exactly this - a sine correlates with itself once a cycle and the
    # peak says which cycle the arithmetic liked.
    #
    configure(p, 108, SHAPE_NOISE)
    sent, back = round_trip(card)
    print()
    print("    the same delay from noise and correlation: %.2f samples = %.3f ms"
          % (audio.delay_samples(sent, back, 2000),
             audio.delay_samples(sent, back, 2000) / 48.0))

    #
    # Leave it quiet, and the USB output back where it was found.
    #
    pedal.set_routing(p)
    pedal.set_pot(p, SETTINGS, SETTINGS_USB_OUT, 2)
    print()
    print("test-analog: reported, not judged - see the header of test-loop.py")
    print("             on why a bench measurement is a hypothesis until the")
    print("             cable has been moved and the number followed it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
