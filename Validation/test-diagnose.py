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
import shutil
import sys

#
# diagnose.py exits rather than raises when numpy or netfault are
# missing, so importing it would end this process at zero and look like
# a pass.
#
try:
    import diagnose
    import loop
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


def gain_of(rows, hz_index=0):
    return float(rows[0][hz_index])


def ladder_reads_the_gain_it_was_given():
    """measured_ladder must report a gain, not a level.

    spice_leg reports what the netlist does to its stimulus. If the
    measured side reported an absolute dBFS instead, the two would differ
    by whatever the converter and the drive happened to be, and the
    ranking would be on that.
    """
    print("measured_ladder()")
    want_db = -6.0
    g = 10.0 ** (want_db / 20.0)

    def fake_tone(card, hz, dbfs, seconds=0.5, tries=4, report=None):
        sent = loop.tone(hz, dbfs, int(seconds * loop.FS))
        return sent, sent * g

    real, loop.measure_tone = loop.measure_tone, fake_tone
    try:
        flat = [0.0] * len(diagnose.FREQS)
        rows = diagnose.measured_ladder(card=0, loop_db=flat, report=lambda *_: None)
    finally:
        loop.measure_tone = real

    record("shape is one row per rung",
           rows.shape == (len(diagnose.LEVELS), len(diagnose.FREQS)), str(rows.shape))
    record("reads the gain it was handed, not the level",
           abs(gain_of(rows) - want_db) < 0.05, "%.3f dB against %.1f" % (gain_of(rows), want_db))
    record("every rung and every point agrees",
           float(abs(rows - want_db).max()) < 0.05, "worst %.3f dB" % float(abs(rows - want_db).max()))

    def unity(card, hz, dbfs, seconds=0.5, tries=4, report=None):
        sent = loop.tone(hz, dbfs, int(seconds * loop.FS))
        return sent, sent

    real, loop.measure_tone = loop.measure_tone, unity
    try:
        cal = [3.0] * len(diagnose.FREQS)
        rows = diagnose.measured_ladder(card=0, loop_db=cal, report=lambda *_: None)
    finally:
        loop.measure_tone = real
    record("the loop's own response is taken out",
           abs(gain_of(rows) + 3.0) < 0.05, "%.3f dB against -3.0" % gain_of(rows))


DIVIDER = """* two 10k, so the answer is -6.02 dB at every frequency and level.
* tran_src replaces the Vin line, so the source has to be called that.
Vin in  0    DC 0 AC 1
R1  in  out  10k
R2  out 0    10k
.end
"""


def spice_leg_reports_the_same_gain():
    """The simulated side has to be the quantity the measured side is.

    A divider is the case with no argument about it: -6.02 dB, flat, and
    the same however hard it is driven.
    """
    print("spice_leg()")
    if shutil.which("ngspice") is None and shutil.which("ngspice_con") is None:
        record("agrees with a divider", True, "no ngspice, skipped")
        return

    sim = diagnose.spice_leg({"spice": lambda _k: {}, "node": "out"}, None)
    freqs = [200.0, 2000.0]
    for level in (-36.0, -12.0):
        db = sim(DIVIDER, freqs, level)
        record("a divider reads -6.02 dB at %+.0f dBFS" % level,
               float(abs(db + 6.0206).max()) < 0.1,
               " ".join("%.3f" % v for v in db))

    quiet, loud = sim(DIVIDER, freqs, -36.0), sim(DIVIDER, freqs, -12.0)
    record("and does not move with drive, being linear",
           float(abs(quiet - loud).max()) < 0.05,
           "worst %.4f dB" % float(abs(quiet - loud).max()))


def loop_response_lands_on_our_frequencies():
    """calibrate() answers on an FFT grid, not on FREQS."""
    print("loop_response()")
    import numpy as np

    f = np.linspace(30.0, 20000.0, 4096)
    h = 10.0 ** (np.full_like(f, -0.5) / 20.0)

    real, loop.calibrate = loop.calibrate, lambda card, seconds=1.0: (0.0, f, h)
    try:
        db = diagnose.loop_response(card=0, report=lambda *_: None)
    finally:
        loop.calibrate = real

    record("one value per frequency we ask about", len(db) == len(diagnose.FREQS),
           "%d of %d" % (len(db), len(diagnose.FREQS)))
    record("a flat loop reads flat", float(abs(db + 0.5).max()) < 1e-6,
           "worst %.6f dB" % float(abs(db + 0.5).max()))


def numbers_are_checked():
    """A sweep given on the command line is still a sweep."""
    print("numbers()")
    record("a list parses", diagnose.numbers("80,160,320", "freqs") == [80.0, 160.0, 320.0])
    record("spaces are not part of a number",
           diagnose.numbers(" -36 , -12 ", "levels") == [-36.0, -12.0])
    for bad, why in (("80,abc", "not a number"), ("", "empty"), (" , ", "only separators")):
        try:
            diagnose.numbers(bad, "freqs")
            record("refuses %r" % bad, ok=False, detail=why + ", it returned")
        except SystemExit:
            record("refuses %r" % bad, ok=True, detail=why)


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
    ladder_reads_the_gain_it_was_given()

    print()
    spice_leg_reports_the_same_gain()

    print()
    loop_response_lands_on_our_frequencies()

    print()
    numbers_are_checked()

    print()
    if FAILED:
        print("test-diagnose: %d FAILED" % len(FAILED))
        for f in FAILED:
            print("    %s" % f)
        return 1
    print("test-diagnose: ok")
    return 0


sys.exit(main())
