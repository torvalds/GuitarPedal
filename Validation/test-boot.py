#!/usr/bin/env python3
#
# Cold power cycles, counted by something other than a person's memory.
#
#   ./test-boot.py --target 7989 --cycles 20 --json boot-A.json
#
# One board fails to reach main() on roughly a quarter of cold starts and
# the others never do.  The numbers that established that - 5 in 20 on
# one board, 0 in 20 on another - were counted by hand and written into
# an issue as prose, which is fine exactly once.  Comparing a second
# firmware against them needs the counting to survive in a form that can
# be diffed, and needs both runs to agree on what they were counting.
#
# So this is a tally sheet, not an automation: a cold cycle is a human
# pulling a USB plug, and there is no switched hub on this bench.  What
# it does do is take everything except the plug out of the operator's
# hands - it notices the unplug, notices the replug, and only asks a
# question when the board does not come back.
#
# The three answers it can end in, and why they are the interesting ones:
#
#	up		enumerated; it booted
#	bootsel		the boot watchdog fired, so main() was reached
#	lit		LED on, no USB - past the crystal, hung after it
#	dark		LED off - never got past the crystal
#
# 'lit' and 'dark' are the split that boot_lamp_after_xosc() exists to
# make, and they are the only two this cannot see for itself: whether a
# board that never enumerated has its LED on is a question only an eye
# can answer.
#
# Skipped rather than failed when the board is not there, in the same
# spirit as the rest of the suite.
#
import argparse
import json
import os
import select
import subprocess
import sys
import time

import pedal

PICOTOOL = "picotool"

POLL = 0.25

PROMPT = "unplug, replug - Enter only if it does NOT come back: "

#
# Erase to end of line.  The status line that replaces this one is
# shorter than it, and without this the tail of the longer line stays on
# screen and grafts itself onto the end of the shorter one - which first
# showed up as a tally reading "up=12/7989 ...".
#
CLEAR = "\033[K"


def in_bootsel(serial):
    """Is *this* board sitting in the bootloader?

    picotool talks to the bootrom's USB interface and to nothing else, so
    a zero return is a board in BOOTSEL rather than one running firmware.
    The serial matters though, and asking without it is a bug that has
    already cost one results file: a bare `picotool info` answers for
    *any* RP-series device on the machine, so flashing a second board
    during a run logs eight phantom BOOTSELs onto the board being tested.

    The bootrom reports the flash chip id as its USB serial and the
    firmware uses the same value, so one string identifies the board on
    both sides of a reboot - checked, rather than assumed:

	firmware serial   7B6F1824A2B5177F
	picotool chipid   0x7b6f1824a2b5177f
    """
    try:
        return subprocess.run([PICOTOOL, "info", "--ser", serial],
                              capture_output=True).returncode == 0
    except FileNotFoundError:
        return False


def present(serial):
    """Is the board with this serial enumerated right now?"""
    return any(d["serial"] == serial for d in pedal.discover())


def wait_while(pred, timeout):
    """Poll until pred() goes false, or the timeout runs out.

    Returns True if it went false in time.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not pred():
            return True
        time.sleep(POLL)
    return False


def classify(serial):
    """One cycle, from the unplug to a verdict.

    A timeout used to decide this, and it was wrong in the one direction
    that matters.  A board that hangs at boot never pulls up D+, so the
    host sees *nothing* on the replug - it cannot tell "not plugged back
    in yet" from "plugged back in and dead".  The operator can, instantly
    and from across the room, because the LED is right there; and having
    seen it, their hand is back on the plug long before any timeout runs
    out.  So the second cycle's success got recorded as the first cycle's
    outcome, one failure disappeared per occurrence, and the tally
    drifted toward "up" - which is the direction that would have made a
    variant look like a fix.

    So the operator says.  A board that comes back is still detected
    without a keystroke, because that is the case the host *can* see; a
    board that does not is announced with one, and only then is the LED
    asked about.  Nothing is inferred from the clock any more.
    """
    #
    # The unplug has no timeout on purpose.  The operator sets the pace
    # and may well be doing twenty of these with a cup of tea; the only
    # thing that would come of hurrying them is a miscounted cycle.
    #
    while present(serial):
        time.sleep(POLL)

    print("\r  %s" % PROMPT, end="", flush=True)
    while True:
        if present(serial):
            return "up", None
        if in_bootsel(serial):
            return "bootsel", None
        #
        # Poll the bus and the keyboard in the same breath.  select() on
        # stdin is what keeps the happy path keystroke-free: the board
        # coming back wins the race on its own, and Enter only ever means
        # "it is back in and it is not coming up".
        #
        if select.select([sys.stdin], [], [], POLL)[0]:
            sys.stdin.readline()
            break

    print()
    print("    replugged and no USB - look at the LED")
    while True:
        answer = input("    [d]ark  [l]it  [b]ootsel  [u]p after all ? ")
        answer = answer.strip().lower()[:1]
        if answer in ("d", "l", "b", "u"):
            return {"d": "dark", "l": "lit",
                    "b": "bootsel", "u": "up"}[answer], "late"


def verdict(tally, cycles):
    """The stopping rule, stated before the numbers existed.

    A ~25% failure rate over 20 trials can detect a failure mode going
    away and cannot detect it merely getting better: 0 failures has a
    0.3% chance under the old rate, but 2 failures is consistent with
    both a halved rate and with luck.  So there are three outcomes and
    the middle one is 'do it again', which is the honest answer.
    """
    bad = cycles - tally.get("up", 0)
    if bad == 0:
        return "changed - 0 of %d failed (p = 0.75^%d under the old rate)" % (
            cycles, cycles)
    if bad >= 3:
        return "unchanged - %d of %d failed" % (bad, cycles)
    return "ambiguous - %d of %d failed; needs another %d" % (bad, cycles,
                                                              cycles)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", default="",
                    help="serial, label or product of the board to cycle")
    ap.add_argument("--cycles", type=int, default=20,
                    help="how many cold power cycles (default 20)")
    ap.add_argument("--json", help="write results here, updated as they arrive")
    ap.add_argument("--note", default="",
                    help="what is being tested, e.g. 'variant B, VCO 1536'")
    args = ap.parse_args()

    #
    # There has to be a person here.  A failure is only visible as an
    # LED, and the "it did not come back" signal is a keypress - and on
    # a stdin that is not a terminal, select() reports EOF as readable
    # immediately and every cycle would record an instant failure.  That
    # is a full run of plausible-looking numbers with nothing behind
    # them, so refuse rather than produce it.
    #
    if not sys.stdin.isatty():
        print("test-boot: SKIPPED - needs a terminal; there is a person in "
              "this loop")
        return 0

    #
    # find() answers None rather than guessing between two boards of the
    # same codec, which is the behaviour this wants: cycling the wrong
    # pedal for twenty minutes produces a number that looks exactly like
    # a real one.
    #
    # Being told a target that is not on the bus is not an error, though,
    # and this learned that the hard way: the board this exists to test
    # is a board that intermittently is not there, and the operator's
    # first instinct is to reach for the plug before starting anything.
    # Both of those arrive here as "not exactly one" and both used to be
    # an immediate skip, which is the least useful moment to give up.
    # So a named target is waited for.
    #
    # ... but only when it is absent.  find() folds "nothing matched"
    # and "several matched" into the same None, and only the first of
    # those is worth waiting for: no amount of waiting turns two
    # TAC5242s into one.  Asking find() about each board in turn is how
    # the two are told apart without restating its matching rule here.
    board = pedal.find(args.target) if args.target else None
    if board is None and args.target:
        found = pedal.discover()
        hits = [d for d in found if pedal.find(args.target, among=[d])]
        if len(hits) > 1:
            print("test-boot: SKIPPED - %r matches %s" % (
                args.target, ", ".join(d["label"] for d in hits)))
            return 0

        print("test-boot: waiting for %r - plug it in, or press ^C" %
              args.target)
        try:
            while board is None:
                time.sleep(POLL)
                board = pedal.find(args.target)
        except KeyboardInterrupt:
            print("\ntest-boot: SKIPPED - %r never appeared" % args.target)
            return 0
        print("test-boot: found it")

    if board is None:
        found = pedal.discover()
        if not found:
            print("test-boot: SKIPPED - no pedal on the USB")
            return 0
        if len(found) > 1:
            print("test-boot: SKIPPED - %d pedals; say which with --target" % (
                len(found),))
            print("           %s" % ", ".join(d["label"] for d in found))
            return 0
        board = found[0]

    #
    # The build stamp, read once and recorded beside the tally.  The
    # thing this experiment is least able to survive is comparing two
    # runs of the same image while believing they were different ones,
    # and the operator's memory of which build is flashed is exactly the
    # part that will not hold up over three rounds.
    #
    ident = pedal.identity(board["port"]) if board["port"] else None
    build = (ident or {}).get("build")
    if build is None:
        print("test-boot: could not read a build stamp from %s" %
              board["label"])
        print("           carrying on, but the result will not say which "
              "image it came from")

    print("test-boot: %s  serial %s" % (board["label"], board["serial"]))
    print("           build %s" % (build or "unknown"))
    if args.note:
        print("           %s" % args.note)
    print("           %d cold cycles - unplug and replug when asked" %
          args.cycles)
    print()

    results = {
        "board": board["label"],
        "serial": board["serial"],
        "build": build,
        "note": args.note,
        "cycles_asked": args.cycles,
        "results": [],
    }

    def save():
        if not args.json:
            return
        #
        # Rewritten every cycle rather than once at the end: twenty
        # unplugs is long enough that an interrupted run is a real
        # possibility, and half a tally is worth much more than none.
        #
        tmp = args.json + ".tmp"
        with open(tmp, "w") as f:
            json.dump(results, f, indent=1)
            f.write("\n")
        os.replace(tmp, args.json)

    tally = {}
    try:
        for n in range(1, args.cycles + 1):
            print("  %2d/%d  unplug %s ...%s" % (n, args.cycles,
                                                 board["label"], CLEAR),
                  end="", flush=True)
            outcome, flag = classify(board["serial"])
            tally[outcome] = tally.get(outcome, 0) + 1
            results["results"].append({"cycle": n, "outcome": outcome,
                                       "late": flag == "late"})
            save()
            print("\r  %2d/%d  %-8s %s%s" % (
                n, args.cycles, outcome,
                " ".join("%s=%d" % kv for kv in sorted(tally.items())),
                CLEAR))
    except KeyboardInterrupt:
        print("\n\ntest-boot: interrupted after %d of %d" % (
            len(results["results"]), args.cycles))

    done = len(results["results"])
    if not done:
        return 0

    print()
    for k in ("up", "bootsel", "lit", "dark"):
        if tally.get(k):
            print("  %-8s %d" % (k, tally[k]))
    results["tally"] = tally
    results["verdict"] = verdict(tally, done)
    save()
    print()
    print("  %s" % results["verdict"])
    if args.json:
        print("  written to %s" % args.json)

    #
    # Always zero.  This measures a board rather than gating a build, and
    # a nonzero exit would put it in the way of anything that ever wires
    # the suite into a gate.
    #
    return 0


if __name__ == "__main__":
    sys.exit(main())
