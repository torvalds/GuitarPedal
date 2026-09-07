#
# Which effect claims to be which netlist, and how to ask them the same
# question.
#
# A target is an effect, the netlist it claims to be, and the translation
# between the two - which is never the identity, because a pot is a
# number from 0 to 1 and a netlist parameter is a resistance fraction
# with a taper in front of it.
#
# It lives in its own file because more than one instrument needs it, and
# because two copies of a pot-to-parameter map is the shape of a whole
# afternoon spent measuring one setting against another.  compare-spice.py
# asks what the model does to a sine and to real playing, draw-rat.py
# draws the answer, and shoot-loop.py plays it: they disagree about
# everything except which knob means which parameter, so that is the part
# that is shared.
#
# The fields:
#
#   effect     the name the bench routes by
#   short      the [SHORT] name, which is what the pedal's SysEx takes -
#              the bench routes by display name and the firmware does
#              not, and neither is derivable from the other
#   netlist    the deck in spice/, without its extension
#   node       which node to probe
#   knobs      every pot the comparison sets, with the default it sits
#              at.  An int default marks an enum pot, which is how
#              pot_args() tells a position from a fraction.
#   spice      knobs -> the .param overrides that mean the same thing
#   settings   named points worth reporting one at a time
#   sweep      axes for a grid: a (lo, hi) pair is N points across that
#              range, a list is exactly those values
#   rival      an effect to measure beside this one, where the comparison
#              between two models is the point
#

#
# The clamp, numbered twice, because the two ends are counting different
# things and both are right.
#
# Effects/rat.h lists the three clamps by the ceiling they impose -
# Silicon 0.61 V, Stacked 1.20 V, LED 1.81 V - which is the order a knob
# should offer them in.  rat-helios.cir numbers the positions of the
# switch on the pedal, an on-off-on whose centre engages neither silicon
# pair and so leaves the permanently-wired LEDs alone: centre is 0, one
# 1N914 each way is 1, two in series is 2.
#
# So this is a fact about the Helios rather than an inconsistency to
# tidy away.  Renumbering the netlist would stop it describing the
# switch that gets set by hand on the bench; renumbering the enum would
# change what every saved scene's Mode byte means.
#
RAT_MODE = {0: 1, 1: 2, 2: 0}

TARGETS = {
    "rat": dict(
        effect="Rat Sketch", short="RAT", netlist="rat-helios", node="out",
        knobs={"Distortion": 0.45, "Filter": 0.5, "Sweep": 0.0,
               "Mode": 0, "Volume": 1.0},
        # the two rheostats are audio taper and the effect cubes them
        spice=lambda k: {"dist": k["Distortion"] ** 3,
                         "filter": k["Filter"] ** 3,
                         "sweep": k["Sweep"],
                         "mode": RAT_MODE[int(k["Mode"])]},
        settings=[{}, {"Distortion": 0.8}, {"Mode": 1}, {"Mode": 2},
                  {"Sweep": 0.5}, {"Filter": 0.9}],
        #
        # Distortion against the clamp, because those are the two that
        # decide the shape of an edge.  Filter is one pole after all of
        # it and moves a pulse's corner without changing what made it.
        #
        sweep={"Distortion": (0.05, 0.95), "Mode": [0, 1, 2]},
    ),
}


def target(name):
    """One target by name, with the candidates listed when it is not one."""
    try:
        return TARGETS[name]
    except KeyError:
        raise SystemExit("no target %r; have %s"
                         % (name, " ".join(sorted(TARGETS))))


def pot_args(t, knobs, effect=None):
    args = ["--pot", "Signal Chain:Gate=0", "--route", effect or t["effect"]]
    for name, v in knobs.items():
        #
        # An enum pot is a position and a continuous one is a fraction of
        # 120 steps.  Telling them apart by whether the default was
        # written as an int is not clever, but the alternative is parsing
        # the generated map and the callers already have an oracle to run.
        #
        raw = int(v) if isinstance(t["knobs"][name], int) else round(v * 120)
        args += ["--pot", "%s:%s=%d" % (effect or t["effect"], name, raw)]
    return args


def grid(t, n):
    """The sweep axes crossed with each other, as knob-override dicts.

    A (lo, hi) axis becomes n points across that range; a list is taken
    as it stands, because an enum has the positions it has and asking
    for five of three is not a question.
    """
    axes = []
    for name, spec in t.get("sweep", {}).items():
        if isinstance(spec, tuple):
            lo, hi = spec
            if n < 2:
                vals = [t["knobs"][name]]
            else:
                step = (hi - lo) / (n - 1)
                vals = [round(lo + i * step, 3) for i in range(n)]
        else:
            vals = list(spec)
        axes.append([(name, v) for v in vals])

    out = [{}]
    for axis in axes:
        out = [dict(row, **{name: v}) for row in out for name, v in axis]
    return out
