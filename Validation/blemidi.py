#!/usr/bin/env python3
#
# Talk to the pedal over Bluetooth, without the web app.
#
#   ./blemidi.py                      ask for the schema and say what arrived
#   ./blemidi.py --wait 25            allow longer for it
#   ./blemidi.py --send 0x05          ask for something else
#   ./blemidi.py --adapter hci1       a different controller
#
# The app was the only Bluetooth client for a while, which made every
# failure ambiguous: a schema that does not arrive could be the radio, the
# pedal, the host's Bluetooth stack or the app's own decoder, and there was
# no second opinion to be had.  This is the second opinion.  It is also
# what a regression test would be built on, since it reports counts rather
# than drawing anything.
#
# It is not a MIDI port: nothing here registers with ALSA. It does use the
# pedal's USB MIDI port once, at the end, to ask the radio what its send
# path did - which is the other half of any answer about a transfer, and
# the reason a failure here is attributable at all.  --no-counters skips
# that if something else is holding the port.
#
# WHAT IT NEEDS.  BlueZ, over D-Bus - python3-dbus and PyGObject, both of
# which are packaged.  And a bond: the pedal's characteristic requires LE
# Secure Connections, so pair while the pedal's pairing window is open -
# nothing can be read or subscribed to without one.  If a run reports
# nothing arriving, see WHAT BITES below before suspecting the radio.
#
# WHAT BITES
#
#   - Connecting fails roughly one try in five with
#     le-connection-abort-by-local.  That is the host giving up, not the
#     radio; it is retried below.
#
#   - A device found by scanning is transient.  BlueZ drops the object
#     shortly after discovery stops, so the scan has to be part of a run
#     rather than a separate step.
#
#   - Holding the debug port with probe-rs while measuring costs the link
#     6 to 20 disconnects a run.  Program the radio, then let go.
#
import argparse
import json
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

import pedal

#
# The BLE MIDI service and its one characteristic, from MMA/AMEI RP-052 -
# Apple's Bluetooth MIDI protocol, adopted as a standard.  Zephyr does not
# ship it, so nRF54/app/src/midi.c is the other end and WebMIDI/ble-midi.js
# is the same decoding again in the browser.
#
MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"

SYSEX_HEADER = (0xF0, 0x7D)


# --------------------------------------------------------------------------
# RP-052, which is the part worth sharing
# --------------------------------------------------------------------------
#
# Three rules shape it and two are easy to get wrong.  Every packet starts
# with a header byte.  Every MIDI status byte is preceded by a timestamp
# byte.  A SysEx continuation packet is a header byte followed straight by
# data, with no timestamp, and that absence is the only thing saying it is
# a continuation.
#
# ble-midi.js implements this a second time for the browser.  They are two
# implementations of one format and nothing checks that they agree, which
# is a real duplication and not a shared decoder.
#

_FIXED = {0xF0: 0, 0xF1: 2, 0xF2: 3, 0xF3: 2}


def midi_message_length(status):
    """How many bytes this status byte's message is, or 0 for SysEx."""
    if status < 0x80:
        return 0
    if status >= 0xF8:
        return 1
    if status < 0xF0:
        return 2 if 0xC0 <= status < 0xE0 else 3
    return _FIXED.get(status, 1)


class Decoder:
    """RP-052 packets in, whole MIDI messages out.

    Whole messages, because that is what Web MIDI delivers and what the
    app is written against: a SysEx arrives as one message however many
    packets carried it.  An unterminated one is left in `partial`, which
    is what a truncated transfer looks like.
    """

    def __init__(self):
        self.messages = []
        self.partial = []
        self._want = 0
        self._status = 0

    def byte(self, b):
        if b >= 0xF8:
            self.messages.append(bytes([b]))       # real-time, stands alone
            return
        if b >= 0x80:
            if b == 0xF7:
                if self._want == 0 and self.partial:
                    self.partial.append(b)
                    self.messages.append(bytes(self.partial))
                self.partial, self._want = [], 0
                return
            self.partial = [b]
            self._want = midi_message_length(b)
            self._status = b if b < 0xF0 else 0
            if self._want == 1:
                self.messages.append(bytes(self.partial))
                self.partial, self._want = [], 0
            return

        if self._want == 0 and not self.partial:
            if not self._status:
                return                             # orphan data byte
            self.partial = [self._status]          # running status
            self._want = midi_message_length(self._status)
        self.partial.append(b)
        if self._want and len(self.partial) >= self._want:
            self.messages.append(bytes(self.partial))
            self.partial, self._want = [], 0

    def packet(self, data):
        """One notification."""
        if len(data) < 2 or (data[0] & 0xC0) != 0x80:
            return
        i = 1
        while i < len(data):
            if not data[i] & 0x80:
                self.byte(data[i])                 # SysEx continuation data
                i += 1
                continue
            i += 1                                 # the timestamp itself
            if i >= len(data):
                return
            if data[i] & 0x80:
                self.byte(data[i])
                i += 1
            elif self._status:
                self.byte(self._status)
            while i < len(data) and not data[i] & 0x80:
                self.byte(data[i])
                i += 1


def encode(stream, limit=20):
    """A MIDI byte stream to RP-052 packets.

    `limit` is the negotiated ATT MTU less three, and 20 is what the
    default MTU of 23 gives.  Web Bluetooth does not expose the MTU, so
    the app cannot do better and neither does this; what matters is the
    other direction, which the radio sizes and which uses all of it.

    Running status is never generated - it saves one byte and costs the
    reader an ambiguity.
    """
    packets, pkt, sysex = [], [], False
    stamp = int(time.monotonic() * 1000) & 0x1FFF

    def start():
        return [0x80 | (stamp >> 7)]

    for b in stream:
        need = 1 if (sysex and b < 0x80) else 2
        if not pkt:
            pkt = start()
        elif len(pkt) + need > limit:
            packets.append(bytes(pkt))
            pkt = start()
        if b == 0xF0:
            sysex = True
        if b >= 0x80:
            pkt.append(0x80 | (stamp & 0x7F))
            if b == 0xF7:
                sysex = False
        pkt.append(b)
    if len(pkt) > 1:
        packets.append(bytes(pkt))
    return packets


def sysex_request(*body):
    """The pedal's `F0 7D <body...> F7`, as packets.

    More than one byte because several of the pedal's commands take an
    argument - `06 01` opens the pairing window where `06` only asks.
    """
    return encode(bytes(SYSEX_HEADER) + bytes(body) + b"\xF7")


# --------------------------------------------------------------------------
# The link, over BlueZ
# --------------------------------------------------------------------------

BLUEZ = "org.bluez"
PROPS = "org.freedesktop.DBus.Properties"


class Link:
    def __init__(self, adapter="hci0", target="pedal", quiet=False):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SystemBus()
        self.adapter = adapter
        self.target = target
        self.quiet = quiet
        self.decoder = Decoder()
        self.notifications = 0
        self.bytes = 0
        self.duplicates = 0
        self._last = None
        self._last_at = 0.0
        self.sizes = {}
        self.first = self.last = None
        self._om = dbus.Interface(self.bus.get_object(BLUEZ, "/"),
                                 "org.freedesktop.DBus.ObjectManager")

    def _say(self, *a):
        if not self.quiet:
            print(*a, flush=True)

    def _objects(self):
        return self._om.GetManagedObjects()

    def _iface(self, path, name):
        return dbus.Interface(self.bus.get_object(BLUEZ, path), name)

    def _prop(self, path, iface, name):
        return dbus.Interface(self.bus.get_object(BLUEZ, path), PROPS) \
                   .Get(iface, name)

    def find(self, seconds=15):
        """The device path, scanning for it if BlueZ has forgotten it.

        By name rather than by address, because the address is per board.
        A pedal's radio is called "Pedal" and the last four hex digits of
        the board's unique id, "Pedal 13A9", so "pedal" finds any of them
        and the whole name finds that board, in either case.
        """
        prefix = "/org/bluez/%s/" % self.adapter

        def look():
            for path, ifaces in self._objects().items():
                dev = ifaces.get("org.bluez.Device1")
                if not dev or not path.startswith(prefix):
                    continue
                name = str(dev.get("Name", "")).lower()
                want = self.target.lower()
                if want in (name, str(dev.get("Address", "")).lower()) \
                   or name.startswith(want + " "):
                    return path
            return None

        found = look()
        if found:
            return found

        self._say("scanning for %r" % self.target)
        adapter = self._iface("/org/bluez/" + self.adapter,
                              "org.bluez.Adapter1")
        try:
            adapter.SetDiscoveryFilter({"Transport": "le"})
            adapter.StartDiscovery()
        except dbus.DBusException as e:
            self._say("  discovery:", e.get_dbus_name())
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            found = look()
            if found:
                break
            time.sleep(0.2)
        try:
            adapter.StopDiscovery()
        except dbus.DBusException:
            pass
        return found

    def open(self, path, tries=6):
        dev = self._iface(path, "org.bluez.Device1")

        #
        # Disconnect first, always.  A StartNotify() on a link BlueZ
        # already believes is subscribed writes no Client Characteristic
        # Configuration descriptor, so a run that reuses the previous
        # connection can measure a radio that nothing ever subscribed to -
        # which looks exactly like the radio failing to notify.
        #
        if bool(self._prop(path, "org.bluez.Device1", "Connected")):
            self._say("dropping the previous connection")
            try:
                dev.Disconnect()
            except dbus.DBusException as e:
                self._say("  disconnect:", e.get_dbus_message())
            time.sleep(1.5)

        for attempt in range(tries):
            if bool(self._prop(path, "org.bluez.Device1", "Connected")):
                break
            try:
                dev.Connect()
                break
            except dbus.DBusException as e:
                self._say("  connect %d: %s" % (attempt + 1,
                                                e.get_dbus_message()))
                time.sleep(1.5)
        else:
            raise RuntimeError("could not connect to %s" % self.target)

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if bool(self._prop(path, "org.bluez.Device1", "ServicesResolved")):
                break
            time.sleep(0.1)

        for cpath, ifaces in self._objects().items():
            c = ifaces.get("org.bluez.GattCharacteristic1")
            if c and cpath.startswith(path) and str(c.get("UUID")) == MIDI_CHAR:
                self.char = self._iface(cpath, "org.bluez.GattCharacteristic1")
                self.bus.add_signal_receiver(self._value, path=cpath,
                                             dbus_interface=PROPS,
                                             signal_name="PropertiesChanged")
                #
                # Stop before starting.  BlueZ keeps notification state
                # per device and a StartNotify() it believes is already
                # in force writes no Client Characteristic Configuration
                # descriptor at all - so the radio is never asked, every
                # notify it tries is refused with -EINVAL, and the run
                # looks like a radio that will not send.  Disconnecting
                # first is not enough on its own.
                #
                try:
                    self.char.StopNotify()
                except dbus.DBusException:
                    pass
                #
                # A refusal here is a real answer, not a crash.
                #
                # The characteristic requires a bond made with LE Secure
                # Connections, so a client that has not paired is told so
                # when it subscribes - which is the point of asking at the
                # descriptor as well as at the value.  Before that, an
                # unpaired client subscribed happily and then waited for
                # notifications that were never allowed.
                #
                try:
                    self.char.StartNotify()
                except dbus.DBusException as e:
                    raise RuntimeError(
                        "the pedal refused the subscription (%s).  It wants a"
                        " bond made with Secure Connections: open the pairing"
                        " window over USB with 'F0 7D 06 01' and pair first."
                        % e.get_dbus_message()) from None
                try:
                    mtu = int(self._prop(cpath,
                                         "org.bluez.GattCharacteristic1",
                                         "MTU"))
                    self._say("connected, ATT MTU %d" % mtu)
                except dbus.DBusException:
                    self._say("connected, ATT MTU not exposed")
                return
        raise RuntimeError("no BLE MIDI characteristic - is this the pedal?")

    def _value(self, iface, changed, invalidated, path=None):
        if "Value" not in changed:
            return
        data = bytes(bytearray(changed["Value"]))
        now = time.monotonic()

        #
        # Every notification arrives here twice.
        #
        # Not from registering twice: dbus-python reports one receiver and
        # one match rule for this path, and the radio counts one
        # notification for the two that arrive.  So it is in the host and
        # the cause is not understood.
        #
        # Left unhandled it silently doubles everything - a schema
        # reassembles at twice its length with a chunk repeated, and looks
        # like a pedal corrupting its own output.
        #
        # A repeat inside three milliseconds is the duplicate; the pedal
        # repeats its status a few times a second and never twice in a
        # millisecond, so a real repetition is not caught by this.  The
        # count is reported either way, because an instrument that hides
        # what it discarded is how this went unnoticed.
        #
        if data == self._last and now - self._last_at < 0.003:
            self.duplicates += 1
            return
        self._last = data
        self._last_at = now
        if self.first is None:
            self.first = now
        self.last = now
        self.notifications += 1
        self.bytes += len(data)
        self.sizes[len(data)] = self.sizes.get(len(data), 0) + 1
        self.decoder.packet(data)

    def write(self, packets):
        """Write without a response, which is what the characteristic is."""
        for p in packets:
            self.char.WriteValue(
                dbus.Array([dbus.Byte(b) for b in p], signature="y"),
                {"type": dbus.String("command")})

    def run(self, seconds):
        loop = GLib.MainLoop()
        GLib.timeout_add(int(seconds * 1000), lambda: loop.quit())
        loop.run()


#
# A message that arrived is not a message that is right.
#
# An F0 at the front and an F7 at the back is all "complete" used to mean
# here, and a schema with bytes missing from the middle has both - it
# reassembles, it terminates, and it is broken.  The pedal's replies are
# JSON, so say whether it parses and where it stops if it does not.
#
def check_message(m):
    if len(m) < 4 or m[0] != 0xF0 or m[1] != 0x7D:
        return
    body = m[3:-1]
    if m[2] not in (0x02, 0x0B, 0x0F, 0x15, 0x1E):  # the ones that are JSON
        return
    try:
        json.loads(body.decode("ascii"))
        print("    parses as JSON, %d bytes of body" % len(body))
    except (ValueError, UnicodeDecodeError) as e:
        at = getattr(e, "pos", None)
        print("    CORRUPT: %s" % e)
        if at is not None and 0 < at <= len(body):
            lo = max(0, at - 24)
            print("    at %d of %d: ...%r<HERE>%r..."
                  % (at, len(body), body[lo:at].decode("ascii", "replace"),
                     body[at:at + 24].decode("ascii", "replace")))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adapter", default="hci0", help="controller (hci0)")
    ap.add_argument("--target", default="pedal",
                    help="advertised name or address; 'pedal' is any"
                         " pedal, 'Pedal 13A9' is that one (pedal)")
    ap.add_argument("--send", default="0x01",
                    help="SysEx body to send, comma separated"
                         " (0x01 asks for the schema; 0x06,0x01 opens the"
                         " pairing window)")
    ap.add_argument("--wait", type=float, default=15.0,
                    help="seconds to listen (15)")
    ap.add_argument("--no-counters", action="store_true",
                    help="do not ask the radio what it did afterwards")
    args = ap.parse_args()

    link = Link(args.adapter, args.target)
    path = link.find()
    if not path:
        sys.exit("%r never advertised on %s" % (args.target, args.adapter))
    link.open(path)

    body = [int(b, 0) for b in args.send.split(",")]
    GLib.timeout_add(500, lambda: link.write(sysex_request(*body)) or False)
    link.run(args.wait)

    span = (link.last - link.first) if link.first else 0
    print("%d notifications, %d bytes, over %.2fs (%.0f B/s)"
          % (link.notifications, link.bytes, span,
             link.bytes / span if span else 0))
    print("packet sizes:", dict(sorted(link.sizes.items())))
    if link.duplicates:
        print("duplicate deliveries discarded: %d" % link.duplicates)
    for m in link.decoder.messages:
        print("  message: %d bytes, %s..." % (len(m), m[:6].hex(" ")))
        check_message(m)
    if link.decoder.partial:
        print("  UNTERMINATED: %d bytes" % len(link.decoder.partial))

    #
    # And what the radio thought of it, over USB.  F0 7D 14 asks and
    # F0 7D 15 answers; the pedal forwards both without reading either.
    #
    #   f  bytes in from the pedal      nc  nobody was connected
    #   o  bytes handed to the stack    ns  no transmit buffer free
    #   p  packets                      fl  the stack refused, e its error
    #   r  the pedal's backlog there    s   the pedal is being held off
    #
    # 'f' far ahead of 'o' is the radio being given more than it sent.
    #
    if not args.no_counters:
        #
        # The same board on USB, by the id digits its Bluetooth name
        # ends in, so that several pedals on USB do not stop this.
        #
        name = args.target.split()
        d, why = pedal.sole(name[1] if len(name) == 2 and
                            name[0].lower() == "pedal" else None)
        if not d:
            print("radio counters: no pedal on USB (%s)" % why)
        else:
            blob = pedal.listen_sysex(d["port"], 0x14, 2.0)
            for opcode in (0x15, 0x1e):
                body = pedal.sysex_payload(blob, opcode)
                print("radio:", body.decode() if body else "no answer")

    return 0 if link.decoder.messages and not link.decoder.partial else 1


if __name__ == "__main__":
    sys.exit(main())
