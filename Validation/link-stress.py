#!/usr/bin/env python3
"""
Load the link between a pedal and its radio, and check that nothing on it
gets lost, damaged or held up.

The link (nRF54/app/src/link.h) carries packets on streams, each paced by a
window of its own.  This starts the load test both firmwares carry
(nRF54/app/src/linktest.h): each side sends --streams test streams of
--count packets at the other, every packet checked on arrival, while the
radio prints --lines lines of debug text.  One stream can be made stuck: its
receiver holds its packets for --stuck-ms, so its window stays shut, and the
rest has to keep moving.  --ble also fetches the schema over Bluetooth
meanwhile, which is real MIDI on a stream of its own.

While the test runs, the radio's counters are asked for over and over.  That
is a command on the control stream, through the radio and back, so how long
each answer takes says whether commands are held up behind the load.

It passes when every test packet arrived exactly once and intact on both
sides, the stuck stream was held and then let go, the link counted no gaps,
nothing was refused, malformed or failed its CRC, and no command took longer
than --max-rtt-ms, and the radio overwrote nothing in its receive ring.
Either side prints each packet it drops on the pedal's debug port, as it
came off the wire.

	./link-stress.py --target DA54
	./link-stress.py --target DA54 --streams 4 --count 3000 --stuck 2 \\
		--stuck-ms 5000 --lines 2000 --ble "Pedal DA54"
"""

import argparse
import json
import os
import select
import statistics
import subprocess
import sys
import time

import pedal


class Raw:
    """The pedal's raw MIDI device, written and read directly."""

    def __init__(self, card):
        path = "/dev/snd/midiC%dD0" % card
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        except OSError as e:
            sys.exit("link-stress: cannot open %s (%s) - is the web app "
                     "holding the port?" % (path, e.strerror))
        self.buf = b""

    def send(self, *body):
        os.write(self.fd, bytes([0xF0, 0x7D, *body, 0xF7]))

    def reply(self, opcode, timeout):
        """The next F0 7D <opcode> ... F7, as its payload, or None."""
        end = time.monotonic() + timeout
        while True:
            start = self.buf.find(bytes([0xF0, 0x7D, opcode]))
            if start >= 0:
                stop = self.buf.find(b"\xf7", start)
                if stop >= 0:
                    body = self.buf[start + 3:stop]
                    self.buf = self.buf[stop + 1:]
                    return body
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    self.buf += os.read(self.fd, 65536)
                except BlockingIOError:
                    pass
            # Status CCs arrive all the time; keep only the tail
            if len(self.buf) > 1 << 20:
                self.buf = self.buf[-65536:]

    def drain(self):
        self.reply(0x7F, 0.05)
        self.buf = b""


def radio_stats(raw):
    """The radio's two counter replies, and how long they took."""
    t = time.monotonic()
    raw.send(0x14)
    s = raw.reply(0x15, 1.0)
    rtt = time.monotonic() - t
    links = raw.reply(0x1E, 0.5)
    try:
        return json.loads(s), json.loads(links) if links else {}, rtt
    except (TypeError, ValueError):
        return None, None, None


def counts(raw):
    raw.send(0x24)
    body = raw.reply(0x24, 1.0)
    try:
        return json.loads(body)
    except (TypeError, ValueError):
        return None


def seven(v):
    return [v & 0x7F, (v >> 7) & 0x7F]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", help="serial, label or board naming one pedal")
    ap.add_argument("--streams", type=int, default=4)
    ap.add_argument("--count", type=int, default=2000,
                    help="packets on each test stream, each way")
    ap.add_argument("--stuck", type=int, default=1,
                    help="the test stream whose receiver holds, -1 for none")
    ap.add_argument("--stuck-ms", type=int, default=3000)
    ap.add_argument("--lines", type=int, default=500,
                    help="debug lines the radio prints meanwhile")
    ap.add_argument("--ble", help="also fetch the schema over Bluetooth "
                    "from the pedal with this name")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--max-rtt-ms", type=float, default=200.0)
    args = ap.parse_args()

    d, why = pedal.sole(args.target)
    if not d:
        sys.exit("link-stress: %s" % why)
    raw = Raw(d["card"])
    raw.drain()

    before, _, _ = radio_stats(raw)
    if before is None:
        sys.exit("link-stress: the radio does not answer")
    first = counts(raw)
    if first is None:
        sys.exit("link-stress: the pedal has no load test")

    stuck = 0x7F if args.stuck < 0 else args.stuck
    raw.send(0x23, args.streams, *seven(args.count), stuck,
             *seven(args.stuck_ms), *seven(args.lines))
    started = time.monotonic()
    print("%s: %d streams of %d packets each way, stream %s stuck for "
          "%d ms, %d debug lines%s" % (
              d["label"], args.streams, args.count,
              "none" if args.stuck < 0 else args.stuck, args.stuck_ms,
              args.lines, ", schema over Bluetooth" if args.ble else ""))

    ble = None
    if args.ble:
        ble = subprocess.Popen(
            [sys.executable, os.path.join(os.path.dirname(__file__),
                                          "blemidi.py"),
             "--target", args.ble, "--wait", "30", "--no-counters"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    want = args.streams * args.count
    rtts, unanswered, last = [], 0, None
    while time.monotonic() - started < args.timeout:
        _, _, rtt = radio_stats(raw)
        if rtt is None:
            unanswered += 1
        else:
            rtts.append(rtt * 1000)
        last = counts(raw)
        if last and last["pedal"]["got"] >= want and \
                last["radio"]["got"] >= want:
            break
    took = time.monotonic() - started

    time.sleep(0.3)
    counts(raw)			# asks the radio once more
    time.sleep(0.3)
    last = counts(raw) or last
    after, links, _ = radio_stats(raw)

    ble_out = ""
    if ble:
        ble_out = ble.communicate(timeout=60)[0]

    ok = True

    def check(what, good):
        nonlocal ok
        print("  %-48s %s" % (what, "ok" if good else "FAIL"))
        ok = ok and good

    print("\nafter %.1f s" % took)
    for side in ("pedal", "radio"):
        c = last[side]
        print("  %s received %d of %d, bad %d, lost %d, repeated %d, "
              "held %d; sent %s" % (side, c["got"], want, c["bad"],
                                    c["lost"], c["early"], c["held"],
                                    c["sent"]))
    print("  pedal's side of the link: %s" % last["link"])
    keys = ("lg", "lr", "lb", "lc", "lt", "lw", "lo", "fl")
    print("  radio counters: %s" % {k: after.get(k) for k in keys})
    sent = last["link"]["tx_bytes"] - first["link"]["tx_bytes"]
    got = after["rb"] - before["rb"]
    print("  bytes from the pedal to the radio: %d sent, %d received by "
          "its UART, %d short (a few in flight is normal)" % (
              sent, got, sent - got))
    if rtts:
        print("  command round trips: %d, median %.1f ms, max %.1f ms, "
              "%d unanswered" % (len(rtts), statistics.median(rtts),
                                 max(rtts), unanswered))
    if ble:
        tail = [l for l in ble_out.splitlines() if "parses" in l or
                "notifications" in l]
        print("  Bluetooth: %s" % ("; ".join(tail) or "no schema"))

    print()
    for side in ("pedal", "radio"):
        c = last[side]
        check("%s got every packet" % side, c["got"] == want)
        check("%s found none bad, lost or repeated" % side,
              c["bad"] == c["lost"] == c["early"] == 0)
        if args.stuck >= 0:
            check("%s held the stuck stream" % side, c["held"] > 0)
    check("no gaps or malformed packets at the pedal",
          last["link"]["gaps"] == first["link"]["gaps"] and
          last["link"]["bad"] == first["link"]["bad"])
    check("no gaps or malformed packets at the radio",
          after["lg"] == before["lg"] and after["lb"] == before["lb"])
    check("no CRC failures at the pedal",
          last["link"]["crc"] == first["link"]["crc"])
    check("no CRC failures at the radio", after["lc"] == before["lc"])
    check("nothing refused",
          last["link"]["refused"] == first["link"]["refused"] and
          after["lr"] == before["lr"])
    check("nothing overwritten in the radio's receive ring",
          after["lo"] == before["lo"])
    check("every command answered within %.0f ms" % args.max_rtt_ms,
          bool(rtts) and not unanswered and max(rtts) <= args.max_rtt_ms)
    if ble:
        check("schema arrived over Bluetooth", "parses as JSON" in ble_out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
