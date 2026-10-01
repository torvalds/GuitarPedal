#!/usr/bin/env python3
#
# Have the pedal learn its hum cuts, and say what it did.
#
#   ./hum-learn.py                    over USB, target -95 dBFS
#   ./hum-learn.py --target 85        target -85 dBFS: it is given in -dBFS
#   ./hum-learn.py --clear            remove the cuts
#   ./hum-learn.py --ble D7:8C:...    over Bluetooth
#
# The same learn the Hum Filter's Target pot starts, without the app:
# the pedal measures the hum and cuts each mains line above the target,
# updating the cuts as it goes, with the output on throughout.  Keep the
# strings quiet while it runs, which is about five seconds; what is
# played is left out of the measurement, and makes it take longer.
# SysEx 21; the layout is in Firmware/hum-learn.h.
#
# With the Hum Filter switched on, its Target pot replaces any other
# target as soon as it sees one, so the target printed is the one the
# pedal reports.
#
import argparse
import sys

import pedal


def decode(body):
    """(mains, updates, learning, target, ignored, cuts) after F0 7D 21."""
    if len(body) < 7 or body[0] != 4:
        raise ValueError("unknown layout")
    mains, n, updates, learning = body[1], body[2], body[3], body[4]
    target, ignored = -body[5], body[6] / 10.0
    cuts = []
    for i in range(n):
        c = body[7 + 7 * i:14 + 7 * i]
        cuts.append({"h": c[0], "depth": ((c[1] << 7) | c[2]) / 4.0,
                     "width": ((c[3] << 7) | c[4]) / 10.0,
                     "before": -c[5], "after": -c[6]})
    return mains, updates, learning, target, ignored, cuts


def finished(bodies):
    """The last report that is not a learn announcing itself."""
    done = [b for b in bodies if len(b) > 4 and not b[4]]
    return done[-1] if done else None


def all_replies(blob):
    out, at = [], 0
    while True:
        at = blob.find(b"\xf0\x7d\x21", at)
        if at < 0:
            return out
        end = blob.find(b"\xf7", at)
        if end < 0:
            return out
        out.append(blob[at + 3:end])
        at = end


def over_usb(arg, wait):
    d, why = pedal.sole()
    if not d:
        sys.exit("no pedal on USB: %s" % why)
    p = d["port"]
    dev = pedal.rawmidi(p)
    cmd = (["amidi", "-p", dev, "-a", "-c", "-r", "/dev/stdout"] if dev
           else ["aseqdump", "-p", p])
    if not dev:
        sys.exit("the raw MIDI device is held; close the web app")
    blob = pedal._listen(cmd, lambda: pedal.send(p, 0x21, arg), wait)
    return finished(all_replies(blob))


def over_ble(target, arg, wait):
    import blemidi
    from gi.repository import GLib
    link = blemidi.Link("hci0", target)
    path = link.find()
    if not path:
        sys.exit("%s is not advertising" % target)
    link.open(path)
    GLib.timeout_add(500, lambda: link.write(
        blemidi.sysex_request(0x21, arg)) or False)
    link.run(wait)
    return finished([bytes(m[3:-1]) for m in link.decoder.messages
                     if len(m) > 3 and m[0] == 0xF0 and m[1] == 0x7D
                     and m[2] == 0x21])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ble", metavar="ADDRESS")
    ap.add_argument("--target", type=int, default=95,
                    help="-dBFS every line is cut to (95)")
    ap.add_argument("--clear", action="store_true")
    args = ap.parse_args()

    arg = 0 if args.clear else args.target
    wait = 5.0 if args.clear else 40.0
    body = (over_ble(args.ble, arg, wait) if args.ble
            else over_usb(arg, wait))
    if not body:
        sys.exit("no answer")

    mains, updates, _learning, target, ignored, cuts = decode(body)
    if args.clear:
        print("cuts cleared")
        return 0
    if not mains:
        print("no mains hum found above the noise")
        return 0
    print("%d Hz mains, %d cut(s), %d update(s), target %d dBFS"
          % (mains, len(cuts), updates, target))
    if target != -args.target:
        print("  (asked for -%d; the Target pot set it)" % args.target)
    if ignored:
        print("ignored %.1f s of playing" % ignored)
    for c in sorted(cuts, key=lambda c: c["h"]):
        print("  %5d Hz  cut %5.1f dB, %4.1f Hz wide   line %4d -> %4d dBFS"
              % (c["h"] * mains, c["depth"], c["width"], c["before"],
                 c["after"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
