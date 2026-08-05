#!/usr/bin/env python3
#
# The same boot hang, chased without a hand on the USB plug.
#
#   ./test-reboot.py --target 7989 --cycles 200 --json reboot-D.json
#
# test-boot.py measures the real thing - a cold power cycle - and costs a
# person twenty unplugs to do it.  This measures something adjacent and
# costs nothing, so it can run for hundreds of cycles while nobody is
# watching.  What it gives up is worth being explicit about.
#
# **A watchdog reboot would test nothing.**  hardware_watchdog/watchdog.c
# sets PSM_WDSEL to "everything apart from ROSC and XOSC", so the crystal
# is still running and still warm on the other side of the reset.
# xosc_init() then finds STABLE already asserted, pll_init() gets a
# reference that settled minutes ago, and it locks.  Every time.  That is
# the most likely reading of the eleven clean flash-and-reboot cycles in
# the issue log: not a firmware that worked, but a test that could not
# fail.  A cold crystal is the whole point.
#
# **A BOOTSEL round-trip is not the same thing**, and there is one piece
# of evidence for it: 7989 hung on exactly this path - CC 20 into the
# bootrom, then picotool reboot back out - while variant D was being
# flashed.  The bootrom reconfigures clocks for its own USB, so the
# machine our firmware wakes up on is not the one a watchdog leaves.
#
# So this is a cheap probe with a known blind spot, not a replacement for
# the plug.  A clean run here does NOT mean the hang is fixed; a failure
# here is real.  Asymmetric, and worth remembering which way round.
#
# It stops at the first hang, because a board that never reaches main()
# has no USB and no watchdog and there is nothing left to ask it - only a
# power cycle recovers it.  "How many reboots until it failed" is the
# measurement, and several runs make a distribution.
#
import argparse
import json
import os
import subprocess
import sys
import time

import pedal

PICOTOOL = "picotool"

BOOTSEL_TIMEOUT = 15.0
RETURN_TIMEOUT = 20.0
POLL = 0.25


def in_bootsel(serial):
    """Is *this* board in the bootloader?  See test-boot.py for why the
    serial is not optional - a bare `picotool info` answers for any
    RP-series device on the machine.  The bootrom reports the flash chip
    id as its USB serial and the firmware uses the same value, so one
    string works on both sides of a reboot."""
    try:
        return subprocess.run([PICOTOOL, "info", "--ser", serial],
                              capture_output=True).returncode == 0
    except FileNotFoundError:
        return False


def board_now(serial):
    """The board's current entry, or None - card and port both move."""
    for d in pedal.discover():
        if d["serial"] == serial:
            return d
    return None


def wait_for(pred, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        got = pred()
        if got:
            return got
        time.sleep(POLL)
    return None


def one_reboot(serial):
    """BOOTSEL and back.  Returns an outcome string."""
    d = board_now(serial)
    if d is None or not d["port"]:
        return "vanished"

    pedal.enter_bootsel(d["port"])
    if not wait_for(lambda: in_bootsel(serial), BOOTSEL_TIMEOUT):
        #
        # It took the CC and did something other than arrive.  Not the
        # hang being hunted - that one happens on the way *back* - but
        # not a success either, and it needs a human just the same.
        #
        return "no-bootsel"

    subprocess.run([PICOTOOL, "reboot"], capture_output=True)

    #
    # A timeout is honest here in a way it was not in test-boot.py: the
    # reboot was issued by this process, so "how long since it was told
    # to come back" is known exactly, with no operator in the middle.
    #
    if wait_for(lambda: board_now(serial), RETURN_TIMEOUT):
        return "up"
    return "hung"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", default="", help="which board")
    ap.add_argument("--cycles", type=int, default=100)
    ap.add_argument("--json", help="write results here, updated as they go")
    ap.add_argument("--note", default="", help="what is being tested")
    args = ap.parse_args()

    board = pedal.find(args.target) if args.target else None
    if board is None:
        found = pedal.discover()
        if args.target or len(found) != 1:
            print("test-reboot: SKIPPED - %r is not exactly one of: %s" % (
                args.target, ", ".join(d["label"] for d in found) or "nothing"))
            return 0
        board = found[0]

    ident = pedal.identity(board["port"]) if board["port"] else None
    build = (ident or {}).get("build")

    print("test-reboot: %s  serial %s" % (board["label"], board["serial"]))
    print("             build %s" % (build or "unknown"))
    if args.note:
        print("             %s" % args.note)
    print("             up to %d BOOTSEL round-trips, stopping at the first "
          "hang" % args.cycles)
    print("             (a clean run does not mean fixed - see the header)")
    print()

    results = {"board": board["label"], "serial": board["serial"],
               "build": build, "note": args.note, "method": "bootsel-roundtrip",
               "cycles_asked": args.cycles, "results": []}

    def save():
        if not args.json:
            return
        tmp = args.json + ".tmp"
        with open(tmp, "w") as f:
            json.dump(results, f, indent=1)
            f.write("\n")
        os.replace(tmp, args.json)

    outcome = "up"
    n = 0
    try:
        for n in range(1, args.cycles + 1):
            t0 = time.monotonic()
            outcome = one_reboot(board["serial"])
            results["results"].append({"cycle": n, "outcome": outcome,
                                       "seconds": round(time.monotonic() - t0, 1)})
            save()
            print("  %4d/%d  %s" % (n, args.cycles, outcome), flush=True)
            if outcome != "up":
                break
    except KeyboardInterrupt:
        print("\ntest-reboot: interrupted")

    print()
    if outcome == "up":
        print("  %d of %d clean, no hang seen" % (n, args.cycles))
        print("  Remember what that does and does not say: a BOOTSEL")
        print("  round-trip may not give the crystal a cold start.")
    elif outcome == "hung":
        print("  HUNG after %d clean reboots" % (n - 1))
        print("  No USB and no watchdog - it needs a power cycle.")
        print("  Look at the LED first: lit means it got past the crystal.")
    else:
        print("  stopped: %s after %d clean reboots" % (outcome, n - 1))

    results["stopped_after"] = n
    results["outcome"] = outcome
    save()
    if args.json:
        print("  written to %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
