#!/usr/bin/env python3
#
# Does anything here write an effect id down?
#
# An effect id is a position in effects[], which is ordered by PRIORITY,
# so adding one effect renumbers every effect below it.  Adding a single
# effect at priority 55 moved [TESTTONE] from 16 to 17 and the settings
# pseudo-effect from 18 to 19, and three files in this directory had
# those numbers written down.
#
# WHY THIS NEEDS A CHECK AND NOT A CONVENTION
#
# The failure is silent by construction.  A stale id is still a valid
# id, so the SysEx write is accepted and sets a real pot on a real
# effect - just not the one that was meant.  A script pinning "USB L/R
# Out to None" pins a Cabinet pot instead and goes on printing a
# plausible baseline that is quietly wrong by a fraction of a percent; a
# script configuring one effect and measuring another goes on passing.
# Nothing errors, and nothing in the numbers looks like a mistake.
#
# Declaring the right answer somewhere central does not fix this on its
# own, because the callers that write a plain integer never ask.  That
# is why this walks the callers rather than trusting a convention.
#
# WHAT IT LOOKS FOR
#
# Two shapes, by walking the syntax rather than grepping the text - a
# grep for a number cannot tell an effect id from a pot number, and this
# has to be precise enough that nobody turns it off.
#
#   1. an integer literal in an argument position that is an effect id
#   2. an integer assigned to a name the generated map also declares,
#      which is how TESTTONE = 16 got written in the first place
#
# It runs in 'check' because it needs nothing but python and the map, and
# because the thing it guards is easiest to get wrong while adding an
# effect - which is when nobody is thinking about this directory.
#
import ast
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = os.path.join(HERE, "bench", "gen", "effect_map.h")

#
# Which argument of which function is an effect id.  A slice means "every
# argument from here on", which is what set_routing() takes.
#
ID_ARGS = {
    "set_pot": (1,),
    "set_mix": (1,),
    "wet_dry": (1,),
    "set_routing": slice(1, None),
}


def declared():
    """{SHORT_NAME: id} out of the generated map."""
    try:
        text = open(MAP).read()
    except OSError:
        return None
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"#define (\w+)_EFFECT_ID (\d+)", text)}


def called_name(node):
    """'set_pot' for pedal.set_pot(...) and for a bare set_pot(...)."""
    f = node.func
    if isinstance(f, ast.Attribute):
        return f.attr
    if isinstance(f, ast.Name):
        return f.id
    return None


def literal_int(node):
    return isinstance(node, ast.Constant) and isinstance(node.value, int) \
        and not isinstance(node.value, bool)


def scan(path, ids):
    """Every place this file writes an effect id down.

    pedal.py is exempt from the assignment rule and from that rule only:
    it holds the one id that is written down on purpose, pedal.CHAIN, and
    main() checks that one against the map directly.  Exempting the file
    rather than the name would let a second constant in beside it, so the
    call-site rule still applies here like anywhere else.
    """
    assigns_ok = os.path.basename(path) == "pedal.py"
    with open(path) as f:
        src = f.read()
    try:
        tree = ast.parse(src, path)
    except SyntaxError as e:
        return ["%s: will not parse: %s" % (os.path.basename(path), e)]

    bad = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            want = ID_ARGS.get(called_name(node))
            if want is None:
                continue
            args = node.args[want] if isinstance(want, slice) \
                else [node.args[i] for i in want if i < len(node.args)]
            for a in args:
                if literal_int(a):
                    bad.append("%s:%d: %s() given the literal %d as an "
                               "effect id"
                               % (os.path.basename(path), a.lineno,
                                  called_name(node), a.value))
        #
        # NAME = 4, where NAME is something the map declares.  This is
        # the shape the three stale ones actually had - the call site
        # looked fine, and the constant above it was the lie.
        #
        if isinstance(node, ast.Assign) and literal_int(node.value) \
           and not assigns_ok:
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id.upper() in ids:
                    bad.append("%s:%d: %s is an effect the map declares, "
                               "assigned the literal %d - ask "
                               "pedal.effect_id(\"%s\") instead"
                               % (os.path.basename(path), node.lineno,
                                  t.id, node.value.value, t.id.upper()))
    return bad


def main():
    ids = declared()
    if ids is None:
        print("check-effect-ids: no %s - run 'make bench' first"
              % os.path.relpath(MAP, HERE))
        return 1
    if not ids:
        print("check-effect-ids: %s declares no effect ids at all, which "
              "cannot be right" % os.path.relpath(MAP, HERE))
        return 1

    #
    # pedal.CHAIN is the one id written down on purpose, so it is checked
    # rather than exempted: the signal chain is priority 0 and first, and
    # if that ever stops being true this says so instead of the pedal
    # quietly gating the wrong effect.
    #
    sys.path.insert(0, HERE)
    import pedal

    bad = []
    if ids.get("CHAIN") != pedal.CHAIN:
        bad.append("pedal.py: CHAIN is %r and the map says %r"
                   % (pedal.CHAIN, ids.get("CHAIN")))

    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".py"):
            continue
        bad += scan(os.path.join(HERE, name), ids)

    if bad:
        print("check-effect-ids: %d effect id(s) written down:" % len(bad))
        for b in bad:
            print("    %s" % b)
        print("\n  An id is a position in a list ordered by PRIORITY, so"
              " adding an effect\n  moves it.  Ask pedal.effect_id(\"NAME\")"
              " or pedal.settings_effect().")
        return 1

    print("check-effect-ids: %d effects, no ids written down" % len(ids))
    return 0


sys.exit(main())
