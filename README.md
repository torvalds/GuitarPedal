## Random guitar pedal board design

### Background

This is a personal toy project that has gone through several phases, but
the common theme has been that it makes absolutely no sense outside of
the very specific niche of "Linus is trying to learn random things about
electronics".

So keep that in mind: there is very little point to any of this to
anybody else.  Don't expect some great useful guitar pedal experience.

I call it my "LEGO for adults" hobby, because this got started when I
wanted to extend my traditional after-Christmas activity (which was
receiving and building _actual_ LEGO kits, which has been a thing for me
since I was a wee tyke) with something else.

So for Christmas 2024, I got a new soldering iron and randomly started
doing guitar pedal kits.  And so over the next month or two, I built at
least two dozen kits, and had to literally look for victims to give them
away to because I had no use for them myself.

> [!NOTE]
> Of all the kits I built, the ones I enjoyed the most were the Aion FX
> ones, and if you are looking for a kit build of traditional analog
> guitar pedals, I can heartily recommend them.
>
> The documentation, the customer service, the components, and the
> enclosures were all top notch. See ["Aion FX"](https://aionfx.com/)

Anyway, after building a lot of these traditional analog guitar pedal
kits I decided I really wanted to actually understand what they did,
because I really had very little experience with any analog circuits.

While I've done some very limited electronics most of my life, almost
all of it has been related to computers, so it's been either digital
logic or power supplies for them.

Also, I was looking for a different kind of soldering experience where
there was less snipping of legs of through-hole components.  I actually
like soldering SMT components, but that doesn't tend to be what those
guitar pedal kits do.

I had done some very limited PCB design with kicad a few years ago, so I
decided to just start learning more about analog circuits.  And then it
kind of grew from that.

### Electrical design

This is the "fourth generation" of my guitar pedal design journey, and
is a new repository because the goal of the learning experience has
evolved.

What started out being about the analog circuits (and the power rails:
those were always a big thing) got to the point where I realized I
really want to do a mixed signal design: understanding what the circuits
do is one thing, re-creating some analog design from the 70s when you
don't actually care about the sound is another thing entirely.

Also, on the actual analog signal side, I started out using op-amps, but
as I was attempting to learn how things actually worked, I had switched
over to a "discrete components only" model, and this continues that
trend (except for the whole digital side, of course).

> [!NOTE]
> To me "discrete components" does include more optimized packages:
> things like dual diodes or matched transistors, but not more complex
> circuits like a op-amp (or a 555 timer or D Flip-flop or other classic
> logic IC)

Also, because I don't typically *listen* to the end result, but look at
it with a signal generator and an oscilloscope, I've grown to detest
power supply noise.

Not knowing what I was doing, quite a lot of my circuits have been very
noisy indeed, and have coupled in noise from the power supply into the
signal chain, and you can really see that on an oscilloscope even when
it's not always audible.

Even in op-amp designs, where the op-amp itself has a very high PSRR and
isn't mixing power supply noise into the signal, my biasing circuits
were often not great, and so the op-amp would see not just the signal
but the power supply noise coming in through the DC biasing.

And every time I tried a dual power rail (so that I could just keep the
signal ground-referenced), the noise from the switching ended up just
always noticeable, and the extra complexity was annoying when a lot of
effects then didn't have any real use for the dual rail.

Filtering obviously helps, but this is just a long-winded explanation
for why I ended up really appreciating the "bias to ground" JFET model
for the signal input side, and the common drain follower in particular.

That works with a single JFET (the MMBF5103 worked well for me), but my
favorite design so far is a dual-JFET LS844 with the second matched JFET
used as a current sink.  It has basically infinite input impedance (and
could be DC coupled, although I do the coupling capacitor with resistor
to ground) and gives a good output signal somewhere roughly in the
middle of the single-supply 9V rail.

See [LS844 Application note](https://www.linearsystems.com/_files/ugd/7e8069_52b1022fbded45fab609459acb337629.pdf)

Why do I mention this in particular? Mainly because it's a great example
of how completely *insane* my designs are.  That LS844 is used as a
voltage follower with a noticeable DC offset, and that single dual-JFET
SOT-23-6 component is more expensive - and harder to find - than a
simple op-amp would be.

Just to put that into perspective: you can buy LM358's at mouser for
about $0.07 each in reasonable quantities (ie a hundred).  Sure, that's
not the greatest op-amp ever and you'd have to have a 5V regulator, so
maybe you're better off at twice the cost, and go for a TL082 or a TL072
instead.  Or go for a BJT input one and be even cheaper.

The LS844? Harder to find, and *much* more expensive.  If they are in
stock at all, you'll find them for $2.50 when you buy ten or more.  And
I'm using it as a questionable replacement for a single input of one of
those dirt-cheap op-amps.

Put another way: go elsewhere for sane designs.  This is not the place.

But it does work quite well. See some notes on the signal path testing
[here](Documentation/Passthrough/Notes.md)


### Physical design

I started out with small designs that I decided would fit best into a
1590A enclosure (basically the smallest regular guitar pedal form
factor), because it was cute.  The purely physical layout limitations
were actually interesting, and since I was doing SMT components and
simple circuits, the circuit sizes were never even remotely an issue.

However, as I decided that I don't want to play around with silly analog
audio effects from the 60's and 70's any more, that 1590A enclosure got
increasingly painful.  Putting an Electrosmith Daisy Seed inside of them
is possible, but only if you don't do a proper stomp-switch and limit
yourself to two pots.  And yes, I did that.

Do I want to use the pre-built Daisy Seed? Maybe - and maybe not.  This
repository does include the beginnings of a "what if I did my own
version of a MCU and CODEC", because it's an interesting learning
experience too.  And I started that because then I'd be able to fit
things better in a 1590A.

But then my meds kicked in, and I've finally given up on the 1590A.
It's cute.  The mechanical challenges were interesting.  But they've
gone from "interesting" to "overly limiting".

So now I'm instead doing a much more reasonable enclosure.  So 1590B it
is.  That just simplifies things a lot and the end result is a lot
saner.

### Components

I've used JLCPCB, PCBWAY and OSH Park for PCB manufacturing.  The end
results are all good, pick the one you're most comfortable with.  I've
found that at least for me, JLCPCB gives the fastest turnaround time,
but I suspect it heavily depends on where you live.

In the past, I've done some PCB assembly services too, and PCBWAY did a
good job.  With this project where the manual soldering has been part of
the whole experience, I've just done bare boards so far.

I've toyed with the idea of doing some assembly service jobs in case I
decide to go for more fiddly components, but that's really for a future
"what if I do the digital side too".  I'm not there yet.

Connectors and potentiometers are typically from Tayda Electronics, with
most of the actual common SMD components from kits or from Mouser.

Some typical parts from Tayda:
 - [Mono Audio Jack A-6976](https://www.taydaelectronics.com/6-35-mm-1-4-righ-angle-mono-female-connector-thread-lock-panel-mount.html)
 - [2.1mm DC Barrel Jack A-4118](https://www.taydaelectronics.com/dc-power-jack-2-1mm-barrel-type-pcb-mount.html)
 - [10k Linear 9mm Pot A-1847](https://www.taydaelectronics.com/10k-ohm-linear-taper-potentiometer-round-shaft-pcb-9mm.html)
 - [DPDT Compact Stomp Foot Switch A-1884](https://www.taydaelectronics.com/dpdt-compact-stomp-foot-pedal-switch-momentary-pcb.html)
 - [Right Angle IDC Header A-2943](https://www.taydaelectronics.com/10-pin-box-header-connector-2-54mm-right-angle.html)

For "random" SMD capacitors and resistors, get one of the kits.  I can
personally heartily recommend the Guanruixin kits from Amazon: there are
0805 and 1206 kits of both capacitors and resistors, and I love the
packaging and labeling.  Very good for organizing at a hobbyist level.

Just the storage case makes them worth getting:

 - [Guanruixin 0805 Capacitor Kit](https://www.amazon.com/Guanruixin-Capacitor-1pF-47uF-Capacitance-Compliant/dp/B0B3JV5PMT)
 - [Guanruixin 0805 Resistor Kit](https://www.amazon.com/Guanruixin-Resistor-Assortment-Tolerance-Compliant/dp/B0B3JVDMZ1)
 - .. any other sizes you want

There are other kits out there, but it's nice to have a good compact
case with a selection of various values.  If you are like me, you'll run
out of the common resistor values, and then I buy cut tape from Mouser
and just fill the specific compartments in that kit case.

Which brings us to Mouser and DigiKey and the like: not just for
refills, but for anything that is slightly more specialized.  Like that
LS844, but really most of the not-completely-standard SMD components.

My exact SMD component choices have been pretty random, and a number of
them have been influenced by footprint rather than technical merit.  I
tend to like things like dual transistor SOT-23-6 packages, and if you
look at my MOSFET choices I think you'll find that the primary choice
was packaging and a high enough V<sub>GSS</sub>.

Put another way: I'm not claiming my parts choices necessarily make
sense.  They've worked for me - often in the sense of "that other part
was just too fiddly to solder, so I've replaced it with this other part
that works for me"

> [!NOTE]
> If you actually know what you are doing, and you looked at the
> schematic and went "Linus is clearly way over his head, and that is
> just _stupid_", whether it comes to parts choices or to just the
> circuit in general, please let me know.
>
> In particular, don't feel like it would be impolite to tell me I'm
> incompetent and doing stupid things. I absolutely know I'm not
> competent and would love to hear any criticism. Some of the best
> teaching moments have been when I haven't understood something, and
> somebody piped up to tell me I should do Xyz.
>
> I'm going to leave the link to the
> ["Tremolo doubling as a metronome"](https://github.com/torvalds/1590A/issues/4)
> issue from the 1590A pedal project, because that was a case of
> somebody (@gralco) coming in and very politely telling me I was doing
> stupid things.
>
> Pushing me to do simulations in KiCad completely changed the game.
> So don't be shy to tell me my circuits suck. Because that's literally
> why I do this!
