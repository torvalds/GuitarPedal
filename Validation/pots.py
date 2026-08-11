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
# So this reads the POT: line rather than being told, and then checks
# itself.  gen_effects.py has already converted each declared default
# into the raw 0..120 that goes in effect_map.h, so converting the
# header's default here and comparing against that is a test of this
# file's arithmetic against the generator's, on every pot, every run.
# If a curve is handled wrongly the mismatch shows up immediately
# instead of as a strange measurement a week later.
#
# It deliberately does not implement every curve.  FREQUENCY is a cubic
# and SQUARED is its own thing; neither is needed yet, and guessing at
# them to be complete would be inventing an authority this file does not
# have.  Ask for one and it says so.
#
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EFFECTS = HERE.parent / "Effects"
MAP = HERE / "bench" / "gen" / "effect_map.h"

POT_RE = re.compile(
    r'//[ \t]*POT:[ \t]*"([^"]+)"[ \t]+(LINEAR|EXPONENTIAL|FREQUENCY|SQUARED|RAW|ENUM)'
    r'(?:\(([^)]+)\))?(?:[ \t]*=[ \t]*(\S+))?')


def _declared():
    """Every POT: line in the tree, by effect display name and label."""
    out = {}
    for path in sorted(EFFECTS.glob("*.h")):
        text = path.read_text()
        m = re.search(r"^// NAME:\s*(.+?)\s*\[(\w+)\]\s*$", text, re.M)
        if not m:
            continue
        for label, curve, rng, default in POT_RE.findall(text):
            lo, hi = (None, None)
            if rng:
                parts = rng.split()
                if len(parts) == 2:
                    try:
                        lo, hi = float(parts[0]), float(parts[1])
                    except ValueError:
                        pass
            out[(m.group(1), label)] = (curve, lo, hi, default)
    return out


def _raw_defaults():
    """What the generator turned each declared default into."""
    out = {}
    text = MAP.read_text() if MAP.exists() else ""
    for name, body in re.findall(r'\.name = "([^"]*)",.*?\.pots = \{(.*?)\n\t\}',
                                 text, re.S):
        base = re.sub(r" \d+$", "", name)          # "Tone 1" -> "Tone"
        for label, _unit, dv in re.findall(
                r'EFFECT_POT\("([^"]*)",\s*([^,]*),\s*(\d+)', body):
            out.setdefault((base, label), int(dv))
    return out


def to_pot(effect, label, value):
    """The 0..120 the firmware stores for an engineering value."""
    spec = _DECLARED.get((effect, label))
    if spec is None:
        raise KeyError(f"no POT: line for {effect}:{label}")
    curve, lo, hi, _ = spec
    if curve == "LINEAR":
        p = (value - lo) / (hi - lo)
    elif curve == "EXPONENTIAL":
        p = math.log2(value / lo) / math.log2(hi / lo)
    else:
        raise NotImplementedError(
            f"{effect}:{label} is {curve}; pots.py only does LINEAR and "
            f"EXPONENTIAL, and guessing at the rest would be inventing one")
    return max(0, min(120, round(p * 120)))


def value(effect, label, pot):
    """The other way: what a raw 0..120 setting reads as on the knob.

    A sweep is written in raw steps, because that is the thing with a
    hundred and twenty-one of them and no rounding in it, and then has
    to say in its table what each step meant.  Doing that by hand is the
    same mistake as doing to_pot() by hand, from the same direction.
    """
    spec = _DECLARED.get((effect, label))
    if spec is None:
        raise KeyError(f"no POT: line for {effect}:{label}")
    curve, lo, hi, _ = spec
    p = pot / 120.0
    if curve == "LINEAR":
        return lo + p * (hi - lo)
    if curve == "EXPONENTIAL":
        return lo * (hi / lo) ** p
    raise NotImplementedError(
        f"{effect}:{label} is {curve}; pots.py only does LINEAR and "
        f"EXPONENTIAL, and guessing at the rest would be inventing one")


def arg(effect, label, value):
    """...as the --pot argument the bench wants."""
    return ["--pot", f"{effect}:{label}={to_pot(effect, label, value)}"]


def selfcheck():
    """Does this file's arithmetic agree with the generator's?

    Returns the list of disagreements, empty when all is well.
    """
    bad, raws = [], _raw_defaults()
    for (effect, label), (curve, lo, hi, default) in sorted(_DECLARED.items()):
        if curve not in ("LINEAR", "EXPONENTIAL") or default is None:
            continue
        want = raws.get((effect, label))
        if want is None:
            continue
        try:
            got = to_pot(effect, label, float(default))
        except (ValueError, TypeError):
            continue
        if got != want:
            bad.append(f"{effect}:{label} default {default} -> {got}, "
                       f"generator says {want}")
    return bad


_DECLARED = _declared()

if __name__ == "__main__":
    problems = selfcheck()
    for p in problems:
        print("pots: MISMATCH " + p)
    n = sum(1 for v in _DECLARED.values() if v[0] in ("LINEAR", "EXPONENTIAL"))
    print(f"pots: {n} linear/exponential pots, "
          f"{'all agree with the generator' if not problems else 'DISAGREEMENTS ABOVE'}")
    sys.exit(1 if problems else 0)
