# Compressor `[COMPRESSOR]`

Five controls: **Level** (−60..−10 dB, the threshold), **Attack** (2..100 ms),
**Release** (50..500 ms), **Ratio** (1..20×) and **Boost** (0..24 dB, makeup
gain). Defaults are −35 dB, 15 ms, 150 ms, 4.8× and 6 dB.

The arithmetic is short enough to state. An envelope follower tracks the
rectified input with those attack and release times; above the threshold the
gain becomes `(level/env)^(1 − 1/ratio)`, slewed to avoid clicks; and the
result is multiplied by Boost whatever happens. **So Boost is unconditional
and compression only ever subtracts** — quiet playing gets the makeup gain and
nothing else, loud playing gets the makeup gain minus whatever the ratio takes
away.

## The static answer, which is exact and not very useful

A steady tone at a series of levels, measuring the gain the compressor
settles on:

```mermaid
%%{init: {'themeVariables': {'xyChart': {'plotColorPalette': '#0072b2, #d55e00'}}}}%%
xychart-beta
    title "Gain applied to a steady tone. Level -20 blue, Level -30 orange"
    x-axis "input, dBFS peak" -60 --> -10
    y-axis "gain, dB" -10 --> 8
    line [6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 2.95, -1.0]
    line [6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 2.95, -1.0, -4.96, -8.92]
```

The knee sits exactly where Level says, and above it each 5 dB of input buys
1.04 dB of output — a ratio of 4.81, against the 4.8 on the pot. Nothing is
wrong with it.

It is also nearly worthless as a description of the effect, which is why this
page does not stop here. A compressor's whole job is changing its gain over
time in response to a signal that changes over time, and a tone that never
changes measures the one case that never happens.

## The dynamic answer

`Inputs/BassForLinus.mp3` is a friend of mine playing scales on a bass, 77
seconds of real dynamics. It is committed unmodified, so it matches the copy
it came from.

**The level it is played at is a measurement choice, not a detail.** The mp3
decodes to **+2.86 dBFS** — reconstruction overshoot on a master that was
already close to full scale — so used raw it would clip the pedal's input
before the compressor saw any of it. It is scaled to −6 dBFS peak, which is
where an instrument track is usually aimed, giving −25.9 dBFS RMS over the
whole take. A quieter input crosses the threshold less often and everything
below moves with it.

```mermaid
%%{init: {'themeVariables': {'xyChart': {'plotColorPalette': '#0072b2, #d55e00'}}}}%%
xychart-beta
    title "Level -30: input blue, output orange, half-second windows"
    x-axis "seconds" 0 --> 76
    y-axis "dBFS rms" -75 --> -10
    line [-69.5, -73.8, -71.6, -66.2, -25.3, -22.4, -25.1, -24.7, -23.6, -25.4, -28.2, -29.8, -33.6, -34.5, -35.3, -35.1, -32.7, -34.4, -32.4, -32.0, -30.7, -38.4, -36.3, -34.2, -36.1, -30.8, -27.9, -25.1, -23.6, -24.9, -26.0, -24.6, -28.6, -26.5, -25.6, -24.3, -24.5, -25.3, -26.5, -24.2, -25.7, -26.7, -27.4, -28.0, -36.9, -47.4, -63.5, -25.4, -24.5, -22.5, -22.1, -23.9, -25.4, -27.4, -36.1, -24.6, -24.6, -24.3, -23.7, -25.0, -27.0, -29.5, -37.7, -24.3, -24.7, -26.8, -23.3, -22.5, -24.9, -33.2, -41.3, -21.6, -27.8, -29.4, -28.4, -22.7, -23.9, -25.3, -32.2, -67.4, -62.0, -58.6, -54.3, -18.9, -23.5, -49.0, -31.7, -28.4, -23.7, -51.7, -54.0, -20.7, -22.5, -23.7, -30.9, -31.5, -42.8, -63.0, -35.9, -21.4, -22.1, -19.7, -25.8, -30.4, -39.1, -60.7, -44.6, -21.7, -23.1, -20.0, -20.5, -21.2, -23.7, -26.4, -34.6, -21.5, -22.0, -23.0, -24.1, -24.9, -25.2, -25.7, -29.0, -24.1, -25.8, -27.7, -28.3, -22.3, -23.2, -24.4, -25.1, -24.0, -24.5, -24.8, -27.5, -32.8, -29.2, -29.5, -28.2, -29.7, -28.5, -30.9, -30.2, -30.1, -27.1, -31.6, -34.2, -37.9, -25.8, -25.6, -26.3, -26.8, -27.4]
    line [-63.5, -67.8, -65.6, -60.2, -24.4, -24.3, -25.2, -24.9, -24.2, -25.3, -26.3, -26.1, -28.1, -28.5, -29.3, -29.1, -27.8, -28.6, -27.5, -27.2, -27.9, -32.4, -30.3, -28.4, -30.3, -25.9, -25.6, -25.2, -24.6, -25.1, -25.4, -25.0, -26.5, -25.3, -25.2, -25.1, -24.8, -25.9, -26.2, -24.2, -25.5, -25.7, -25.5, -25.4, -33.3, -41.4, -57.5, -24.3, -24.5, -24.0, -24.3, -24.8, -24.8, -25.2, -31.8, -24.6, -24.6, -24.8, -24.6, -25.6, -25.6, -26.2, -32.9, -23.9, -24.6, -25.5, -24.1, -24.6, -25.4, -31.3, -35.3, -23.2, -26.5, -26.0, -26.4, -23.8, -24.1, -24.7, -31.2, -61.4, -56.0, -52.6, -48.3, -21.9, -28.1, -43.0, -27.6, -25.8, -27.4, -45.7, -48.0, -22.8, -26.8, -24.0, -27.3, -26.8, -37.2, -57.0, -30.2, -23.6, -25.4, -24.1, -25.9, -27.2, -33.1, -54.7, -38.6, -23.3, -26.6, -23.1, -24.7, -25.0, -25.8, -26.4, -32.2, -23.4, -24.6, -24.8, -25.1, -25.0, -24.8, -25.0, -27.7, -23.8, -24.7, -25.1, -25.1, -23.5, -24.2, -24.5, -24.6, -24.7, -24.8, -24.8, -27.0, -27.4, -26.1, -27.4, -25.8, -26.8, -26.1, -27.0, -27.1, -26.8, -26.1, -26.5, -28.2, -31.9, -24.9, -25.3, -25.4, -25.3, -25.2]
```

That is the picture a tone cannot give. The blue line wanders over 50 dB; the
orange one spends most of the take between −24 and −28 dB. The passages the
player left quiet come up, the loud ones are held, and the shape of the phrase
survives — the orange line still rises and falls, it just does it over a
quarter of the range.

The silences are the exception and they are correct: where the input drops to
−67 dB between takes, the output sits 6 dB above it, because there is nothing
to compress and the makeup gain is unconditional. A compressor lifts the noise
floor between notes; this one is honest about it.

## What the threshold is worth

The same recording, gain plotted against how loud the playing was, at two
threshold settings:

```mermaid
%%{init: {'themeVariables': {'xyChart': {'plotColorPalette': '#0072b2, #d55e00'}}}}%%
xychart-beta
    title "Gain against playing level. Level -20 blue, Level -30 orange"
    x-axis "input, dBFS rms in half-second windows" -55 --> -16
    y-axis "gain, dB" -6 --> 8
    line [6.0, 6.0, 6.0, 6.0, 6.0, 5.99, 5.5, 3.71]
    line [6.0, 6.0, 5.87, 5.47, 4.23, 1.35, -1.29, -3.5]
```

At −20 dB, against this recording at this level, the effect never applies less
than +3.0 dB and never more than +6.0 — the blue line is flat until the loudest
few seconds and then bends slightly, which is a boost with a hint of
compression on top. The orange line, ten decibels down, is what a compressor's
transfer curve is supposed to look like: +6 dB on the quiet passages falling to
−3.5 dB on the loud ones, about 10 dB of range squeezed out of the performance.

Neither is right or wrong. Which one you get depends on how loud the signal
arriving is, and that is a thing the player sets — with Trim, with a boost, or
with whatever pedal is in front. What the threshold does is fix the distance
between the two.

**A caution about reading the two transfer curves together**: they do not
share an x-axis. The static one is the peak amplitude of a sine; this one is
half-second RMS of a bass. Those differ by the crest factor, which for plucked
bass is a good 12 dB, so the knee appears in a different place on each and
neither is wrong.

## What the knobs are worth

The threshold is compared against an envelope of the input, so how much any
setting does depends entirely on how hot the instrument is. That makes "is
this default any good" a question with three different answers:

```mermaid
%%{init: {'themeVariables': {'xyChart': {'plotColorPalette': '#0072b2, #d55e00, #009e73'}}}}%%
xychart-beta
    title "Dynamic range removed. Input peaking -6 blue, -12 orange, -20 green"
    x-axis "Level, dB" -40 --> -15
    y-axis "dB of range removed" -1 --> 17
    line [15.7, 12.0, 8.2, 4.4, 0.9, -0.0]
    line [11.2, 7.4, 3.6, 0.5, -0.0, -0.0]
    line [5.2, 1.5, -0.0, -0.0, -0.0, -0.0]
```

**At a realistic instrument level the default threshold does nothing at all.**
A guitar around 0.1 V RMS lands near −12 dBFS peak once its crest factor is
allowed for, and on that middle line the default −20 dB removes 0.0 dB of
dynamic range. You have to be at −35 before it takes out a useful 7 dB, and
−35 is most of the way to the bottom of a pot that stops at −40.

The top half of the Level control — everything above about −25 — does nothing
for any input a guitar produces.

## Attack and release cost low notes

A fast envelope follows the waveform rather than the note, and modulating the
gain at the note's own frequency is distortion. It is a bass problem far more
than a guitar one: the penalty falls about 11 dB per octave.

```mermaid
%%{init: {'themeVariables': {'xyChart': {'plotColorPalette': '#0072b2, #d55e00, #009e73'}}}}%%
xychart-beta
    title "THD by note. Release 50ms blue, 150ms orange, 500ms green"
    x-axis "Hz" [40, 80, 160, 320, 640]
    y-axis "THD, dB" -105 --> -35
    line [-40.5, -50.6, -62.0, -73.9, -85.8]
    line [-48.4, -58.5, -70.0, -81.8, -93.8]
    line [-57.6, -67.8, -79.3, -91.1, -103.1]
```

At the default 150 ms, a guitar's low E measures −58.5 dB and a bass low E
−48.4 dB. So the default is comfortable for the instrument this pedal is for
and marginal for the one an octave below it. Dropping to 50 ms costs 8 dB
everywhere and takes bass low E to −40.5 dB, which is audible.

Attack barely matters here by comparison — the whole 2 ms to 100 ms range
moves THD by 5 dB — while it does change how much gets squeezed, from 9.9 dB
at 2 ms to 4.3 dB at 100 ms. So attack is close to a free control and release
is not.

Ratio is nearly free above about 8: at Level −30 it buys 8.8 dB of squeeze at
8:1 and 9.4 dB at 20:1, against 8.2 dB at the default 4.8.

## What the defaults are, and why

**These were made up.** The controls were given plausible-sounding numbers when
the effect was written and had never been measured. The ranges too.

Measuring changed one of them. Because the response is a pure translation, the
end of the pot is a wall: with the old `LINEAR(-40 0)`, a guitar arriving at
about −20 dBFS peak could not get more than roughly 5 dB of compression at any
setting, simply because the knob stopped. The range is now `LINEAR(-60 -10)`,
which costs 0.42 dB per step against 0.33 and forecloses nothing.

With that room, here is what each candidate default does to the instrument
somebody actually plugs in — no trim, nothing in front:

```mermaid
%%{init: {'themeVariables': {'xyChart': {'plotColorPalette': '#0072b2, #d55e00, #009e73, #666666'}}}}%%
xychart-beta
    title "dB removed by pickup. 280mVpp blue, 500mVpp orange, 1Vpp green, 2Vpp grey"
    x-axis "Level, dB" [-30, -33, -35, -37, -40, -45]
    y-axis "dB of range removed" -1 --> 21
    line [-0.0, 0.4, 1.4, 2.9, 5.1, 8.8]
    line [1.4, 3.5, 5.1, 6.6, 8.8, 12.6]
    line [5.9, 8.1, 9.7, 11.3, 13.5, 17.0]
    line [10.4, 12.7, 14.3, 15.8, 17.6, 20.4]
```

Peak-to-peak volts convert through `process.h`: a sample is the instantaneous
volts over √2, because 1.0 is a 1 Vrms sine peaking at 1.414 V. So the README's
"typical" 280 mVpp is −20.1 dBFS peak, and a hot pickup at 500 mVpp is −15.1.

**−35 dB** is the default, and two things pick it. It gives 5.1 dB of reduction
on a hot pickup and 9.7 dB on a humbucker — a compressor doing something the
moment you switch it on, which is presumably why you switched it on. And it
sits at exactly mid-travel on the new range, so the knob's centre is the
sensible setting and both directions mean something.

It is deliberately gentle on a quiet single-coil, 1.4 dB. That is what the 25 dB
of travel below it is for.

The other four hold up. Ratio is in the useful part of its curve at 4.8 and
nearly free above 8. Attack costs 5 dB of THD across its whole range while
changing the squeeze by more than twice that. Release is right for guitar and
marginal an octave below.

## Reproducing this

```
cd Validation
make bench          # a stale bench measures a pedal you no longer have
./analyse-compressor.py
```

Needs `ffmpeg` for the decode. That decode is deterministic for a given
ffmpeg, and the script prints the sample count, raw peak and scaled RMS
first — if those move and nothing else changed, the decoder did.
