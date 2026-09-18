#!/usr/bin/env python3
#
# What number is this name?
#
# An effect id is a position in effects[], ordered by PRIORITY, so adding
# one effect renumbers every effect below it.  A stale id is still a
# valid id: the write lands on a real pot of a real effect, the firmware
# accepts it, and nothing says a word.  So nothing here writes a number
# down, and this is the one place that turns a name into one.
#
# WHAT IT READS
#
# midi_schema.h, which gen_effects.py writes beside effect_map.h.  It is
# a C string holding JSON, and it carries what the C header only holds as
# code: the curve, the range and the enum names of every pot.  That is
# why this and not effect_map.h, and it is what lets pots.py do
# arithmetic without parsing Effects/*.h for a second opinion.
#
# It is also byte for byte what the pedal serves on SysEx 0x01 -> 0x02.
# Reading it off the board instead of off the build is then a change of
# transport rather than a rewrite, which matters because the build
# describes the commit and the question is usually about the board.
#
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

#
# The firmware build first: it is the one that matches a board you just
# flashed.  The bench copy is the fallback because `make bench` writes it
# without needing a cross toolchain.
#
SCHEMA_PATHS = (
    os.path.join(HERE, "..", "build", "midi_schema.h"),
    os.path.join(HERE, "bench", "gen", "midi_schema.h"),
)

MIX = 0                 # SysEx pot 0 is the mix; 1..10 are the effect's own


class MapError(Exception):
    pass


class NoMap(MapError):
    """Nothing generated one yet."""


class NoSuchName(MapError):
    """The map has no such effect or pot, or more than one."""


def parse(text):
    """A schema, from the generated header or from the pedal's own reply.

    The file wraps it in a C string and the pedal sends it bare, which is
    the only difference between the two.  Firmware old enough to send a
    list rather than an object is read as one, the way the app reads it.
    """
    m = re.search(r'= "(.*)";\s*$', text, re.S)
    if m:
        text = m.group(1).encode().decode("unicode_escape")
    obj = json.loads(text)
    return obj if isinstance(obj, dict) else {"steering": {"pots": []},
                                              "effects": obj}


def _read(path):
    """The schema in a midi_schema.h, or None if it is not there."""
    try:
        text = open(path).read()
    except OSError:
        return None
    try:
        return parse(text)
    except ValueError:
        raise MapError("%s is not a generated schema" % path)


_cache = {}
_using = []


def use(obj):
    """Resolve against this map rather than against the build's.

    Process-wide and deliberately blunt: a script drives one pedal and
    wants every lookup in it to mean that pedal.  Returns what was in
    force, so a caller that needs the build's map back can put it back.
    """
    was = _using[0] if _using else None
    _using[:] = [obj] if obj is not None else []
    return was


def schema(path=None):
    """The generated schema, as the pedal would serve it.

    Both copies are read when no path is given.  They come from one
    generator and disagree only when one of the two builds is stale,
    which is worth saying out loud - it is invisible otherwise, and it
    decides which of two maps a measurement was taken against.
    """
    if _using and path is None:
        return _using[0]
    if path in _cache:
        return _cache[path]

    if path is not None:
        got = _read(path)
        if got is None:
            raise NoMap("no schema at %s" % path)
        _cache[path] = got
        return got

    found = [(p, _read(p)) for p in SCHEMA_PATHS]
    found = [(p, d) for p, d in found if d is not None]
    if not found:
        raise NoMap("no generated schema - run 'make', or 'make bench' in "
                    "Validation/")
    if len(found) > 1 and found[0][1] != found[1][1]:
        raise MapError(
            "%s and %s disagree - one of the two builds is stale"
            % (os.path.relpath(found[0][0]), os.path.relpath(found[1][0])))

    _cache[path] = found[0][1]
    return found[0][1]


def _unpad(s):
    """'  ATTN' is a label, and the padding is presentation."""
    return s.strip().lower()


def _effects(path=None):
    return schema(path)["effects"]


#
# Three spellings reach one effect, because three already exist: the
# display name, the short name in brackets in the header, and the C
# spelling a copy gets.  Copies share a short name - 'Tone 1' and
# 'Tone 2' are both [TONE] - and the generator resolves that by giving
# the first copy the plain name and numbering the rest from two, so
# 'TONE' is Tone 1 exactly as TONE_EFFECT_ID is.  Display names are
# unique because the generator numbers those too.
#
def _by_name(name, path=None):
    want = _unpad(name)
    display, short, cident = [], [], []
    seen = {}
    for e in _effects(path):
        s = e["shortName"]
        n = seen[s] = seen.get(s, 0) + 1
        ident = s if n == 1 else "%s%d" % (s, n)
        for key, bucket in ((e["name"], display), (s, short), (ident, cident)):
            if _unpad(key) == want:
                bucket.append(e)

    for bucket in (display, cident, short):
        if len(bucket) == 1:
            return bucket[0]
        if len(bucket) > 1:
            raise NoSuchName(
                "%r is %d effects - %s" %
                (name, len(bucket), ", ".join(e["name"] for e in bucket)))
    raise NoSuchName("no effect called %r" % name)


def effect(name, path=None):
    """The effect id, from any of the three spellings of its name."""
    return _by_name(name, path)["id"]


def display(name, path=None):
    """The display name, which is what bench and pots.py are keyed on."""
    return _by_name(name, path)["name"]


def settings(path=None):
    """The settings pseudo-effect, which is not the last one."""
    return effect("Settings", path)


def names(path=None):
    """[(id, display name, short name)], in effects[] order."""
    return [(e["id"], e["name"], e["shortName"]) for e in _effects(path)]


def pot_labels(name, path=None):
    """An effect's own pot labels, in the order it declares them."""
    return [p["name"].strip() for p in _by_name(name, path)["pots"]]


def _find_pot(name, label, path=None):
    """(SysEx index, the pot's schema entry)."""
    e = _by_name(name, path)
    want = _unpad(label)
    for i, p in enumerate(e["pots"]):
        if _unpad(p["name"]) == want:
            return i + 1, p
    #
    # The steering pots belong to every steerable effect rather than to
    # any one of them, and they carry their own index.
    #
    if e["steerable"]:
        for p in schema(path)["steering"]["pots"]:
            if _unpad(p["name"]) == want:
                return p["index"], p
    raise NoSuchName("%s has no pot called %r - it has %s"
                     % (e["name"], label,
                        ", ".join(repr(p["name"].strip()) for p in e["pots"])))


def pot(name, label, path=None):
    """The pot number the SysEx takes, where 0 is the mix."""
    return _find_pot(name, label, path)[0]


def pots(name, *labels, path=None):
    """Several of one effect's pot numbers, in the order asked for."""
    return tuple(pot(name, label, path) for label in labels)


def pot_info(name, label, path=None):
    """Curve, range, enum names, and both spellings of the default."""
    return _find_pot(name, label, path)[1]


def enum_value(name, label, value, path=None):
    """The raw value an ENUM pot's name stands for."""
    p = _find_pot(name, label, path)[1]
    choices = p["enum"] or []
    want = _unpad(value)
    for i, c in enumerate(choices):
        if _unpad(c) == want:
            return i
    raise NoSuchName("%s:%s has no value %r - it has %s"
                     % (display(name, path), p["name"].strip(), value,
                        ", ".join(repr(c) for c in choices) or "no enum"))


#
# Everything above resolves a name.  This asks whether the map it
# resolved against is self-consistent, so that a broken generator shows
# up here rather than as a measurement of the wrong effect.
#
def selfcheck(path=None):
    bad = []
    ids = [e["id"] for e in _effects(path)]
    if ids != list(range(len(ids))):
        bad.append("ids are not 0..%d in order" % (len(ids) - 1))

    seen = set()
    for e in _effects(path):
        if e["name"] in seen:
            bad.append("two effects are called %r" % e["name"])
        seen.add(e["name"])

        for i, p in enumerate(e["pots"]):
            if pot(e["name"], p["name"], path) != i + 1:
                bad.append("%s:%s does not resolve to %d"
                           % (e["name"], p["name"].strip(), i + 1))
            if p["curve"] == "ENUM":
                if not p["enum"]:
                    bad.append("%s:%s is an ENUM with no values"
                               % (e["name"], p["name"].strip()))
                elif p["max"] != len(p["enum"]) - 1:
                    bad.append("%s:%s has %d values and a max of %g"
                               % (e["name"], p["name"].strip(),
                                  len(p["enum"]), p["max"]))
    return bad


def main():
    try:
        s = schema()
    except MapError as e:
        print("effectmap: %s" % e)
        return 1

    bad = selfcheck()
    for b in bad:
        print("effectmap: %s" % b)
    if bad:
        return 1

    print("effectmap: %d effects, %d pots, all reachable by name"
          % (len(s["effects"]), sum(len(e["pots"]) for e in s["effects"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
