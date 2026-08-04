#!/usr/bin/env python3
#
# The hardware MIDI jacks, exercised through a USB-MIDI adapter.
#
# MIDI_HW defaults on, so the UART path ships in every build, and until
# now nothing had touched it since it was written - issue 48 is already a
# known defect in that parser.  A cheap adapter on the 3.5mm TRS jacks
# turns it into something that can be run.
#
# The other half is that it is a control channel which does not go
# through USB.  Everything the pedal accepts over USB it accepts here,
# because uart_midi_poll() hands its packets to the same
# handle_midi_packet() - including CC 20 value 126, which is BOOTSEL.
# And it streams its status CCs out of the jack whether or not anything
# is listening, so "is the firmware running" can be asked of a pedal
# whose USB never came up.  That is the question issue 86 keeps needing
# and cannot otherwise put.
#
#   ./test-midi.py [--trials N]
#
# NOT GATING.  It always exits 0.  The adapter is a cheap CH345, the
# path has shown intermittent failures that have never been pinned down,
# and a flaky check wired into a gate teaches you to ignore the gate.
# What is wanted from it now is a rate: run it often, watch the number,
# and let more data say whether the flakiness is the dongle waking up,
# the parser, or nothing at all.
#
import argparse
import sys
import time

import pedal


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=1.2)
    args = ap.parse_args()

    dong = pedal.dongle()
    if not dong:
        print("test-midi: SKIPPED - no MIDI adapter on the sequencer")
        return 0

    pedals = pedal.discover()
    if not pedals:
        print("test-midi: SKIPPED - no pedals")
        return 0

    print("test-midi: adapter on %s (%s)" % (dong, pedal.rawmidi(dong)))

    #
    # Which pedal the jacks belong to, by trying it rather than being
    # told.  A note-on the pedal does not consume comes back out of both
    # its MIDI thru paths, so whichever pedal echoes it to USB is the one
    # the adapter is wired to.
    #
    target = None
    for d in pedals:
        if _saw_note(pedal.midi_listen(d["port"], args.seconds,
                                       during=lambda: _note(dong))):
            target = d
            break
    if not target:
        print("test-midi: SKIPPED - the adapter does not reach any pedal")
        return 0

    print("test-midi: wired to %s" % target["label"])

    alive = rx = tx = act = 0
    for _ in range(args.trials):
        if pedal.midi_alive(dong, args.seconds):
            alive += 1

        # adapter -> pedal IN -> thru -> pedal USB
        if _saw_note(pedal.midi_listen(target["port"], args.seconds,
                                       during=lambda: _note(dong))):
            rx += 1

        # pedal USB -> thru -> pedal OUT -> adapter
        if _saw_note(pedal.midi_listen(dong, args.seconds,
                                       during=lambda: _note(target["port"]))):
            tx += 1

        # and whether it *acts* on what arrives, which needs no thru at
        # all: CC 7 is the master volume, read back from the state it
        # reports over USB.
        if _volume_follows(dong, target):
            act += 1

    n = args.trials
    for name, hit in (("status CCs arriving", alive),
                      ("adapter -> pedal IN", rx),
                      ("pedal OUT -> adapter", tx),
                      ("pedal acts on CC 7", act)):
        print("  %-22s %d/%d" % (name, hit, n))

    worst = min(alive, rx, tx, act)
    print("test-midi: %s" % ("all four clean" if worst == n else
                             "%d of %d trials clean on the worst of them"
                             % (worst, n)))
    return 0


def _note(port):
    pedal._play(port, [bytes([0x90, 60, 100])])


def _saw_note(seen):
    return any(seen[i] == 0x90 and seen[i + 1] == 60
               for i in range(len(seen) - 2))


def _volume_follows(dong, target):
    """Set the master volume over the jacks, read it back over USB."""
    for cc, want in ((64, 60), (127, 120)):
        pedal._play(dong, [bytes([0xB0, 7, cc])])
        time.sleep(0.3)
        got = _chain_volume(target)
        if got is None or abs(got - want) > 2:
            return False
    return True


def _chain_volume(d):
    """Pot 5 of effect 0, out of a state dump."""
    import re
    import subprocess

    proc = subprocess.Popen(["aseqdump", "-p", d["port"]],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        time.sleep(0.4)
        pedal.send(d["port"], 0x05)
        time.sleep(2.0)
    finally:
        proc.terminate()
        text = proc.stdout.read()
        proc.wait()
    blob = bytes.fromhex("".join(
        re.findall(r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)",
                   text)).replace(" ", ""))
    #
    # One message per effect, carrying pairs: F0 7D 03 <eff> [<pot>
    # <val>] ... F7.  This used to look for 'F0 7D 03 00 <pot>' as a
    # prefix, which worked only while every pot was its own message - the
    # dump now puts the mix first, so the wanted pot is somewhere in the
    # middle of effect 0's message rather than at the front of its own.
    #
    want = bytes([0xF0, 0x7D, 0x03, 0x00])
    i = blob.rfind(want)
    if i < 0:
        return None
    end = blob.find(0xF7, i)
    if end < 0:
        return None
    for at in range(i + 4, end - 1, 2):
        if blob[at] == pedal.CHAIN_VOLUME:
            return blob[at + 1]
    return None


if __name__ == "__main__":
    sys.exit(main())
