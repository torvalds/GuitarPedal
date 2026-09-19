#!/usr/bin/env python3
#
# Does diagnose.py refuse a --exclude it did not understand?
#
# --exclude exists to stop a part being blamed for a fault it cannot
# have.  A name that matches nothing achieves none of that, and the
# failure is silent: the part stays a candidate, the run prints it as
# excluded anyway, and the wrong answer arrives after the dictionary has
# taken its half hour.  So a name that is not in the netlist is an error
# before any of that is spent.
#
# Needs no pedal and no ngspice.
#
import sys

#
# diagnose.py exits rather than raises when numpy or netfault are
# missing, so importing it would end this process at zero and look like
# a pass.
#
try:
    import diagnose
except SystemExit:
    print("test-diagnose: SKIPPED - diagnose.py has no numpy or no netfault")
    sys.exit(0)

FAILED = []


def record(name, ok, detail=""):
    print("  %-52s %s%s" % (name, "ok" if ok else "FAIL",
                            "  " + detail if detail else ""))
    if not ok:
        FAILED.append(name)


PARTS = {"R1": {}, "R2": {}, "C1": {}, "RPD": {}}


def main():
    print("blameable()")
    record("no --exclude leaves every part",
           diagnose.blameable(PARTS, "") == ["C1", "R1", "R2", "RPD"])
    record("a named part is dropped",
           diagnose.blameable(PARTS, "RPD") == ["C1", "R1", "R2"])
    record("spaces around the commas are not part of the name",
           diagnose.blameable(PARTS, " RPD , R1 ") == ["C1", "R2"])
    record("an empty field is not a name",
           diagnose.blameable(PARTS, "RPD,,") == ["C1", "R1", "R2"])

    print()
    print("what it will not accept")
    for arg, why in (("Rsrc", "a part from another netlist"),
                     ("rpd", "the right part in the wrong case"),
                     ("R1,R99", "one good name and one bad")):
        try:
            diagnose.blameable(PARTS, arg)
            record("refuses %r" % arg, False, "it returned")
        except SystemExit as e:
            record("refuses %r" % arg, True, "%s: %s" % (why, str(e)[:44]))

    print()
    print("and it says so before anything is spent")
    try:
        diagnose.blameable(PARTS, "R1,C1,R2,RPD")
        record("excluding everything is an error too", False, "it returned")
    except SystemExit:
        record("excluding everything is an error too", True)

    print()
    if FAILED:
        print("test-diagnose: %d FAILED" % len(FAILED))
        for f in FAILED:
            print("    %s" % f)
        return 1
    print("test-diagnose: ok")
    return 0


sys.exit(main())
