#!/usr/bin/env python3
"""
A stereo test tone for measuring samples dropped or repeated on the way.

    vernier-tone.py out.wav [minutes]

The left channel is a sine of exactly 48 samples a cycle at 48 kHz, 1000 Hz;
the right, exactly 47 samples, about 1021 Hz; both at -12 dBFS.  Upward zero
crossings on each channel come exactly one period apart, so a run of samples
dropped or repeated anywhere on the way moves them by that many samples -
modulo 48 on the left, modulo 47 on the right, which together say how many
outright, up to 1128 either way.  Unlike a level, that survives somebody
turning the volume.  The pedal checks it in Firmware/hardware.h.

Convert it for a phone with ffmpeg -i out.wav out.flac.  It has to be played
at 48 kHz: resampled, the periods are not whole numbers of samples.
"""

import math
import struct
import sys
import wave

RATE = 48000
PERIODS = (48, 47)
LEVEL = 10 ** (-12 / 20)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit('usage: vernier-tone.py out.wav [minutes]')
    minutes = float(sys.argv[2]) if len(sys.argv) == 3 else 30

    # One whole repeat of both, so the file's end meets its start cleanly
    cycle = PERIODS[0] * PERIODS[1]
    frames = bytearray()
    for n in range(cycle):
        frames += struct.pack('<hh', *(round(32767 * LEVEL *
                                             math.sin(2 * math.pi * n / p))
                                       for p in PERIODS))

    repeats = int(minutes * 60 * RATE / cycle)
    with wave.open(sys.argv[1], 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        for _ in range(repeats):
            w.writeframes(frames)


if __name__ == '__main__':
    main()
