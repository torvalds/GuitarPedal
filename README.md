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

Connectors and potentiometers are typically from Tayda Electronics, with
most of the actual common SMD components from kits or from Mouser.

For "random" SMD capacitors and resistors, get one of the kits.  I can
personally heartily recommend the Guanruixin kits from Amazon: there are
0805 and 1206 kits of both capacitors and resistors, and I love the
packaging and labeling.  Very good for organizing at a hobbyist level.

The AideTek kits are good too, but the Guanruixin ones are really
compact for those "get all the values just in case" situations.

Then just go to Mouser or Digikey and buy cut tape for the values you
use more of.  You'll run out of the common resistors in the kits, but
they are still worth having for the less usual ones and just for the
organizing.

And things that are just a bit more specialized - like the the C0G caps
in higher capacitances - you won't necessarily find in the kits.
