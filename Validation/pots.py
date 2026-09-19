#!/usr/bin/env python3
#
# Turn "-35 dB" into the 0..120 the firmware stores.
#
# Every analysis script needs this and the obvious way to do it is to
# write the arithmetic inline against the range in the header.  That is
# how three separate measurements got taken at the wrong settings in one
# afternoon: a range moved, the script did not, and nothing said so -
# the numbers came out plausible and wrong, which is the worst kind.
#
# So the curve and the range are asked for rather than written down, and
# then this checks itself.  The generated map carries each declared
# default twice - once as the engineering value and once as the raw
# 0..120 gen_effects.py turned it into - so converting one here and
# comparing against the other tests this file's arithmetic against the
# generator's, on every pot, every run.  A curve handled wrongly shows up
# immediately instead of as a strange measurement a week later.
#
# It deliberately does not implement every curve.  FREQUENCY is a cubic
# and SQUARED is its own thing; neither is needed yet, and guessing at
# them to be complete would be inventing an authority this file does not
# have.  Ask for one and it says so.
#
import math
import sys

import effectmap

#
# A BOOL is an ENUM the generator spells differently: it declares the two
# names, so reading it as one is not a guess.
#
DONE = ("LINEAR", "EXPONENTIAL", "FREQUENCY", "SQUARED", "ENUM", "BOOL")
NAMED = ("ENUM", "BOOL")

#
# The two curves that interpolate on a power of the knob's travel rather
# than on the travel itself, and which power each is.  Audio/util.h:
# frequency_pot() is cubic() and a SQUARED pot squares.
#
POWER = {"FREQUENCY": 3.0, "SQUARED": 2.0}


def _range(info):
    """(curve, low, high).

    The steering pots are declared by the firmware rather than by a POT:
    line, so they carry their choices and no range.  For an enum the two
    are the same statement.
    """
    curve = info["curve"]
    lo, hi = info.get("min"), info.get("max")
    if hi is None and curve in NAMED:
        lo, hi = 0.0, float(len(info["enum"] or []) - 1)
    return curve, lo, hi


def labels(effect):
    """An effect's pot labels, in the order the header declares them.

    Which is also SysEx pot order shifted by one, because pot 0 there is
    the mix.  Asking beats a table of constants per effect for the same
    reason effectmap.settings() beats counting: the header moves and the
    constants do not.
    """
    return effectmap.pot_labels(effect)


def defaults(effect):
    """{label: raw 0..120} as the generator computed each declared default.

    For putting a board into the state the bench starts from.  A test
    that sets only the pots it cares about is measuring those pots plus
    whatever the last person left behind.
    """
    return {lab: effectmap.pot_info(effect, lab)["defaultPot"]
            for lab in labels(effect)}


def to_pot(effect, label, value):
    """The 0..120 the firmware stores for an engineering value.

    An ENUM or a BOOL takes the name of one of its choices.  It also
    takes a position, because a command line is allowed to say
    `Shape=2`.

    A value the pot cannot reach is refused, whichever kind it is.
    Clamping it returns a setting that is in range, plausible and not
    what was asked for, which is the failure this whole file exists to
    stop: a sign slip or a dB-against-linear mix-up then measures
    something real at a setting nobody chose.  Rounding happens first,
    so a value a hair outside its own endpoint still lands on 0 or 120.
    """
    info = effectmap.pot_info(effect, label)
    curve, lo, hi = _range(info)

    if curve in NAMED:
        if isinstance(value, str):
            return effectmap.enum_value(effect, label, value)
        if not 0 <= value <= hi:
            raise ValueError("%s:%s has %d choices, so %r is not one"
                             % (effect, label, len(info["enum"] or []), value))
        return int(value)

    if curve == "LINEAR":
        p = (value - lo) / (hi - lo)
    elif curve == "EXPONENTIAL":
        p = math.log2(value / lo) / math.log2(hi / lo)
    elif curve in POWER:
        #
        # Audio/util.h puts the knob's own travel to a power and
        # interpolates on that, so the inverse takes the root.  A value
        # under the bottom of the range would be a fractional root of a
        # negative number, which is a complex number rather than an
        # error, so it is refused here instead.
        #
        ratio = (value - lo) / (hi - lo)
        if ratio < 0:
            raise ValueError("%s:%s is %s(%g %g), so %r is off the end of it"
                             % (effect, label, curve, lo, hi, value))
        p = ratio ** (1.0 / POWER[curve])
    else:
        raise NotImplementedError(
            f"{effect}:{label} is {curve}; pots.py only does {', '.join(DONE)}"
            f", and guessing at the rest would be inventing one")
    pot = round(p * 120)
    if not 0 <= pot <= 120:
        raise ValueError("%s:%s is %s(%g %g), so %r is off the end of it"
                         % (effect, label, curve, lo, hi, value))
    return pot


def value(effect, label, pot):
    """The other way: what a raw setting reads as on the knob.

    A sweep is written in raw steps, because that is the thing with a
    hundred and twenty-one of them and no rounding in it, and then has
    to say in its table what each step meant.  Doing that by hand is the
    same mistake as doing to_pot() by hand, from the same direction.

    An ENUM or a BOOL reads as the name of its choice, not as a number.
    """
    info = effectmap.pot_info(effect, label)
    curve, lo, hi = _range(info)

    if curve in NAMED:
        choices = info["enum"] or []
        if not 0 <= pot < len(choices):
            raise ValueError("%s:%s has %d choices, so %r is not one"
                             % (effect, label, len(choices), pot))
        return choices[int(pot)]

    p = pot / 120.0
    if curve == "LINEAR":
        return lo + p * (hi - lo)
    if curve == "EXPONENTIAL":
        return lo * (hi / lo) ** p
    if curve in POWER:
        return lo + p ** POWER[curve] * (hi - lo)
    raise NotImplementedError(
        f"{effect}:{label} is {curve}; pots.py only does {', '.join(DONE)}"
        f", and guessing at the rest would be inventing one")


def arg(effect, label, value):
    """...as the --pot argument the bench wants.

    Under the display name, which is the only one the bench answers to.
    """
    name = effectmap.display(effect)
    return ["--pot", f"{name}:{label}={to_pot(effect, label, value)}"]


def selfcheck():
    """Does this file's arithmetic agree with the generator's?

    Returns (disagreements, how many pots were checked).
    """
    bad, n = [], 0
    for _id, name, _short in effectmap.names():
        for label in labels(name):
            info = effectmap.pot_info(name, label)
            if info["curve"] not in DONE or info["default"] is None:
                continue
            want = info["defaultPot"]
            declared = info["default"]
            if info["curve"] in NAMED:
                declared = (info["enum"] or [])[int(declared)]
            n += 1
            got = to_pot(name, label, declared)
            if got != want:
                bad.append(f"{name}:{label} default {declared} -> {got}, "
                           f"generator says {want}")
    return bad, n


def main():
    try:
        problems, n = selfcheck()
    except effectmap.MapError as e:
        print("pots: %s" % e)
        return 1
    for p in problems:
        print("pots: MISMATCH " + p)
    print("pots: %d pots, %s" % (
        n, "all agree with the generator" if not problems
        else "DISAGREEMENTS ABOVE"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
