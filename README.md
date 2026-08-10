## Resurrected random guitar pedal project

This is a resurrected version of my old guitar pedal project: a digital
guitar pedal built around an RP2354A and a TI audio codec, with all the
effects done in software on a core of its own.

It went through a phase with a little OLED screen and a couple of rotary
encoders on it, and that's gone now.  Editing seventeen effects through a
128x128 screen and two knobs was never going to be pleasant, and I'm not
exactly known for my mad UI designing skillz, so the pedal itself is down
to one knob and one stomp switch and the deep editing happens somewhere
that already has a screen: there's a web app that talks to it over USB
MIDI from a browser.

So what's in here:

 - `Hardware` has the kicad files.
 - `Software` has the firmware that makes it do something, and
   `Software/WebMIDI` is the web app that edits it.
 - `Validation` is the test suite.  Some of it runs on your machine and
   some of it wants a pedal plugged in.
 - `Documentation` remains a very optimistic name for a directory.

## Firmware

I've only ever built the firmware on Linux, but it *should* be perfectly
possible to build on MacOS or Windows too if you just figure out the
platform requirements. The project depends on the `pico-sdk` and
`tinyusb` libraries, and has submodules for both, so they get built
automatically, but the build tools your platform has to provide.

Regardless of platform, you'll need the basics:
 - git
 - make
 - python3
 - cmake

and a 32-bit arm cross-build environment.  On Linux, that would be
something like
 - arm-none-eabi-binutils-cs
 - arm-none-eabi-gcc-cs
 - arm-none-eabi-newlib

There's more than one board this can be built for, and the build won't
guess.  You say which one this tree is about once, in
`Software/board.local`, and it stays said - it's a file rather than a
cmake cache variable specifically so that blowing away `build/` doesn't
silently change your mind for you.  So:
```
	git clone https://github.com/torvalds/GuitarPedal.git
	cd GuitarPedal
	cd Software
	echo unified > board.local
	make prep
	make
```
`unified` is the current one-board pedal.  The other is `split`, which is
the older modular pair of boards.  If you forget this step the configure
will stop and tell you, which is the intended behaviour - a wrong board
should be an error you fix once, not a working build for hardware you
don't have.

That gets you `build/pedal-unified.uf2` (and `.elf`, and `.bin`).  Every
artifact is named for the board it's for, so there is deliberately no
`pedal.uf2` to grab by mistake.  You can build the other board without
changing what the tree is about - ``make split`` - or both with ``make
all-boards``.

To get the `.uf2` onto the pedal you need it in BOOTSEL mode, and there
are two ways.  If it's running and plugged in, send it MIDI CC 20 with
value 126 and it will reboot into BOOTSEL on its own:
```
	amidi -l                          # find which card the pedal is
	amidi -p hw:5,0 -S "B0 14 7E"     # ...and that 5 is mine, not yours
```
Otherwise, hold the knob in while you plug it in.  The rotary encoder's
switch doubles as BOOTSEL on this board - through a Schottky, so that
QSPI traffic can't fake pulling it low - and that's the route that still
works on a pedal too broken to enumerate.  The older boards had a proper
BOOTSEL button, but it was on the little MCU daughtercard, which is fine
on a bench and useless the moment the thing is screwed into a box.

Either way a USB mass storage device shows up and you copy the file to
it.

If you have installed picotool with USB support (the pico-sdk build only
builds a cut-down version without it), you can skip all that and just do
``make flash``, which builds and flashes the board in `board.local`.
``make flash-split`` does the other one.

### Testing

`Validation` has the test suite, and it's split by what it needs.

``make check`` runs the part that needs no hardware at all: the MIDI
packetiser, whether the web app still loads and still draws the right
conclusions, and a sweep that asks every biquad in the pedal where it
actually landed.  ``make check-biquad`` is that last one on its own, and
``make check-effects`` puts signals through the effects themselves.

That last one works because of `Validation/bench`, which builds the
pedal's actual audio core - the same `single_sample()`, the same effect
headers, the same compiler flags - as a program on your machine that
takes float samples on stdin and hands them back on stdout.  It is not a
model of the signal path.  It's the signal path, with the two register
blocks the audio core touches replaced by ordinary memory.

Then there are the ones that want hardware: ``make check-hw`` with a
signal generator, ``make check-analog`` with a patch cable from the
pedal's output back to its own input, ``make check-bench`` to ask whether
a real board agrees with the host build, and ``make check-loop`` if you
have two pedals to patch into each other.

## Hardware

The kicad design files (and some supporting infrastructure, like the 3D
printed insert and the enclosure drill rules) are in the ``Hardware``
subdirectory.

These days it's just the one board.  `Hardware/usb-stomp` has the RP2354A
and the TI TAC5242 codec sitting directly on it, and that's the whole
pedal: no cable, no daughtercards, no connectors in between.

What's on it:

 - a 1/4" input jack and a 1/4" output jack
 - a third 1/4" jack for an expression pedal or a remote stomp switch
 - MIDI in and out, on 3.5mm "Type A" TRS jacks (out through a 5V
   buffer, in through an optocoupler, like MIDI has always wanted)
 - USB-C, and the usual 9V guitar pedal power jack
 - one rotary encoder with a switch under it, and one stomp switch
 - three addressable WS2812B LEDs

The MIDI jacks and the expression jack are the genuinely new things.  The
older boards had nowhere to put them - the expression jack in particular
wants two ADC pins, and those are exactly the pins the old MCU board was
spending on hardware MIDI, so it could not have existed there at all.

Fair warning on that expression jack, though: the hardware is done and
the firmware can read it, but only when the host asks it to over SysEx.
Right now it's a probe for figuring out what people actually plug in
there - expression pedals don't even agree on which contact is the wiper
- and nothing is wired up to *do* anything with the result yet.

### How it used to be

It used to be modular, and those board files are all still here.  There
were two little daughtercards for the "core" hardware - the RP2354
microcontroller (`Hardware/rp2354`) and the TI TAC5112 codec
(`Hardware/codec`) - plus a board for the audio and 9V DC power jacks
(`Hardware/audio-jacks`), and a main board (`Hardware/pedal-board`) for
the pedal IO: i2c connector for the screen, USB-C programming port,
rotary encoders, pin header for stomp switches.  The two halves were
joined by a 12P 0.5mm FFC cable carrying power and data lines (i2c for
control, i2s for audio).

That was mostly so I could try out different form factors.  It let me lay
the two halves out independently, and it was flexible enough to cram a
125B enclosure full of jacks, rotaries and the screen all in one top
area.  There was also a worry about noise isolation, which in the end
never had to be tested.

I used the nice HiRose BM28 series connectors on the modular boards.
They are absolutely tiny, which makes for a great board footprint but
admittedly also makes for a slightly more complicated board due to the
tiny 0.35mm pitch.  I'm not a fan of the traditional pin headers simply
because they make it so hard to do compact form factors.

Those boards are finished and they still work - the firmware builds for
them, and they're still useful for testing.  But what this section used
to say was that if you know what you want, you should just put the codec
directly on the audio jack board and the rp2354 on the IO board.  Which,
yes.  That's the board above.

### Images

![The unified board](Images/unified.jpg)

The bare `usb-stomp` board, powered up.

## Basic UI

There's no screen.  The pedal has one knob, one footswitch and three
LEDs, and everything else happens in a browser.

### On the pedal

Out of the box the footswitch is bypass - tap for in-circuit or not, hold
it for the tuner - and the knob is the master volume, with either kind of
press on it putting the volume back where it started.  That last one
sounds trivial and isn't: it's the way back when you can't see anything.

But none of that is compiled in.  There are five gestures - turn the
knob, tap it, hold it, tap the switch, hold the switch - and what each
one means is a table the pedal looks up and the app writes.  You can
point the knob at any parameter of any effect, or make a single press
take one effect's mix up while it takes another's down.  A pedal with no
screen can't tell you what its knob is for, so that has to be settable
from something that can.

The three LEDs, left to right: something is wrong (red for clipping,
amber for dropped samples), in circuit or bypassed, and the chain is
busy - where "busy" means whatever the effect thinks it means, so the
gate lights it while it's gating and the compressor while it's
compressing.

### In the browser

`Software/WebMIDI` is a static web page that talks to the pedal over USB
MIDI.  There's nothing to install and it runs off a phone.

It doesn't know what the effects are.  It asks.  The pedal describes
every effect it has - every pot, the ranges, the units, the hover text -
and the app builds itself out of that, so it can't drift out of step with
the firmware, and adding an effect to the pedal makes it show up in the
app without touching the app.

What you actually do there is *route*.  There are seventeen effects and
none of them are in your signal path until you say so: routing picks
which ones run, and in what order, and the order is yours, because a
boost in front of a delay is a different pedal from a delay in front of a
boost.  Whatever you don't route costs nothing.

Pots are mostly just numbers, except where they aren't - the tone stacks,
the parametric EQ and the cab sim draw their actual frequency response,
so you can see the curve instead of inferring it from three knob
positions.

<img src="Images/WebMIDI-screenshot.png" width="420"
     alt="The WebMIDI app with a tone control and a tape echo routed">

A tone stack with a 10dB mid bump at 810Hz and a tape echo behind it, and
the fifteen effects that aren't routed sitting at the bottom waiting to
be.  The faint grey line is the phase response.

Eight scenes fit in the pedal's own flash.  A scene is a routing plus
everything set inside it, and a MIDI program change recalls one, which is
half of what those MIDI jacks are for: a cheap footswitch can step
between scenes with no computer in sight.

## Audio effects

The current effects are:

 - Noise gate

This one is fairly simple.  Depending on how noisy your guitar
environment is, you may or may not need this one.  But particularly if
you use the boost effect very aggressively, you probably want it even if
you don't have a lot of 50Hz / 60Hz hum.

The default level is -70dBV, which is pretty quiet.

Anyway, 0dBV is very loud - most guitar levels are roughly in the -20dB
range (0.14V peak, aka 280mV peak-to-peak voltage).

-40dB is a "quiet sound" (14mV peak voltage), and -60dB is pretty much
silence.  So a -70dB noise gate *should* be a good starting point for a
good low-noise pickup.

That noise gate allows going down all the way to a -100dB noise floor,
which is ridiculously border-line for what the hardware can actually do.
But my environment and guitar is actually quiet enough that I
*can* go down to -85dB, and it will glow brightly to show that the gate
is on and the signal is smaller than that.

I'm actually pretty happy with that, in that it's about a 0.1mV
peak-to-peak signal.  It's not just that my guitar isn't picking up a
lot of noise from the environment, it also means that the pedal itself
is not noisy.

Alternatively, it just means that I got all the math wrong, and it's
lying to me.

There's also attack/release values that can tune just how the size of
the envelope is calculated, and how quickly it reacts to noise (and
how quickly it goes back to gating).

 - Compressor

This does what a compressor does.  Like a noise gate, there's a
attack/release to tune how the signal envelope is tracked.  It has a
"boost" setting to allow it to just boost the signal in general, but the
"level" is then the level at which it starts compressing.

The "ratio" is how aggressively it compresses signals that go over the
level (but the attack is also very relevant: the attack is about hoq
*quickly* - or slowly - it reacts to signals that go over the level).
So the "attack" basically says how quickly it starts reacting to a
signal that goes over, and then the ratio is how aggressive it is once
it starts reacting to it.

 - Boost (w/ distortion)

I like this one.  Others may not.

It can be used as just a clean boost - but so can the compressor.  But
what I like doing with it is to set it to some ridiculously high boost
value (like +20dB), and then set the *level* down to something fairly
low (like -20dB).

A +20dB signal boost is basically increasing the voltage level by 10x,
but then the "-20dB level" means that the "level" is set to 0.14V.

And what that boost effect does is that when the signal hits the voltage
level, it "folds" it down (or up, if it hit the negative level).  So the
+20dB boost will first make the signal much bigger, but then the level
folding will limit the end result to sane levels, but instead of just
clipping at that level, the signal folds down and you get higher
harmonics.

I think it sounds more interesting than the typical soft- or
hard-clipping effects.

 - phaser
 - flanger

Nothing particular about these.  They are very simple effects.

 - echo

This is the Echo King effect from Cleveland Music Co, converted from the
Hothouse example effects to this pedal.  All credit for it goes to Ricky
Sheaves, except if I screwed up in the conversion, in which case you get
to blame me.

It's a DSP model of the Maestro Echoplex family of tape delay.

 - pitch shifter

This one is almost certainly not useful, but it's fun.  It's a pitch
shifter, but it's *not* the smart kind of "do an FFT, shift frequencies
up or down".

Instead it's based on a delay loop, and walking the delay either faster
than realtime (shifting the pitch up) or slower than real-time (shifting
it down).  And then to avoid the sudden discontinuities when you have to
jump backwards (or forwards), it actually walks the delays in two
phases, and multiplies by a function that goes down to zero at the
discontinuity point (the function happens to be sine/cosine for the two
phases, but it could be something else).

End result: it does shift the pitch, but it also has a delay due to how
it's done.

*And* to make it sound even more complex, it has a feedback thing, so
it can feed back its own pitch-shifted signal into the delay loop, and
you get another pitch shifting (with an extra delay). So you can kind
of think of it as a short echo with a pitch shift.

It tends to sound most natural - which isn't saying much - with a +1
octave shift, but it isn't limited to whole octaves.  You can shift the
pitch up by random fractions.  Play around with it.

 - 10-band EQ

This is the most complicated from an actual algorithmic standpoint, and
also has the fanciest display.

Each band goes +-20dB (so 0.1 .. 10x). At the extremes, it will tend
to distort the signal - all the math is done in 32-bit
single-precision float, I won't guarantee it's entirely stable or
smooth.

 - "USB"

This doesn't affect the sound, but it turns the USB audio interface
logic on and off, and you can pick whether you want the stereo signal
to be either all dry, all wet, or "left channel wet, right channel
dry".

It's a work-in-progress. It works, but not entirely reliably.
