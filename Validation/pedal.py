#
# Just enough SysEx to set the pedal up for a measurement.
#
# Deliberately its own thing rather than reaching for the interactive
# tools: those live outside the tree and are about working on the
# project, and a test that lives in the tree cannot depend on them being
# there.  What is needed here is also a different job - set a value,
# trigger a save - from "ask a question and print the answer".
#
# Everything goes through the ALSA sequencer rather than the raw MIDI
# device, because on a desktop the raw device is usually already held
# open by the sound server and the web app may want it too.  aplaymidi
# plays files, so a message has to become a one-event standard MIDI
# file; it is twenty-two bytes and less trouble than a dependency.
#
import os
import re
import subprocess
import sys
import tempfile
import time

HEADER = bytes([0xF0, 0x7D])

# Effect 0 is the signal chain, and its pots in SysEx numbering, where 0
# is the mix and 1-10 are the effect's own.
CHAIN = 0
CHAIN_GATE, CHAIN_TRIM, CHAIN_VOLUME = 1, 4, 5

# The settings pseudo-effect is last, and what is wanted from it is the
# USB output mode: 3 is Wet/Dry, which puts the processed signal on the
# left and the untouched input on the right.
SETTINGS_USB_OUT = 1
USB_OUT_WET_DRY = 3


def ports(match=""):
    """Every pedal's sequencer port, as [(port, name)].

    The port name comes from the USB product string, which names the codec
    - "TAC5242 Pedal" - so 'match' is how a caller says which pedal it
    means: "TAC5242", or "5112".  Empty means any of them.
    """
    try:
        out = subprocess.run(["aplaymidi", "-l"], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return []
    found = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\d+:\d+)\s+(.*\S)", line)
        if not m or "pedal" not in m.group(2).lower():
            continue
        if match.lower() not in m.group(2).lower():
            continue
        found.append((m.group(1), m.group(2)))
    return found


def port(match=""):
    """The sequencer port to play to, or None."""
    found = ports(match)
    return found[0][0] if found else None


#
# What a pedal looks like on the USB, and how the two halves of it are
# found again.
#
PEDAL_VID, PEDAL_PID = "ffff", "0003"

#
# ALSA hands a card-bound sequencer client a number by a fixed rule:
# sixteen global clients, then four per card.  So the client that belongs
# to card N is 16 + 4*N, and that is the missing link between the audio
# side and the MIDI side - the sequencer reports neither a card nor a
# serial, only a name.
#
# Used as a candidate rather than as gospel: the name at that client has
# to match the card's USB product string before it is believed.
#
SEQ_GLOBAL_CLIENTS = 16
SEQ_CLIENTS_PER_CARD = 4


def _usb_attr(card, name):
    """One attribute of the USB device behind an ALSA card."""
    dev = os.path.realpath("/sys/class/sound/card%d/device" % card)
    try:
        return open(os.path.join(dev, "..", name)).read().strip()
    except OSError:
        return None


def discover():
    """Every pedal on the machine, keyed by the one thing that is unique.

    A pedal is an ALSA card and a sequencer port, and nothing reports
    both.  This used to join them through the codec name in the product
    string, which worked exactly as long as no two boards had the same
    codec - and then a second TAC5242 arrived and the join started
    silently pairing one board's MIDI with another board's audio, which
    is worse than not finding it.

    So the join is through the USB serial now, which sysfs does report
    for the card, and the sequencer client is derived from the card
    number and then checked against the name before it is used.  Nothing
    here depends on what the pedal calls itself.

    Sorted by serial, so the order is stable across reboots and
    re-plugging in a way that card numbers are not.
    """
    found = []
    for path in sorted(os.listdir("/sys/class/sound")):
        m = re.fullmatch(r"card(\d+)", path)
        if not m:
            continue
        card = int(m.group(1))
        if (_usb_attr(card, "idVendor") != PEDAL_VID or
                _usb_attr(card, "idProduct") != PEDAL_PID):
            continue

        product = _usb_attr(card, "product") or "?"
        serial = _usb_attr(card, "serial") or "?"
        client = SEQ_GLOBAL_CLIENTS + SEQ_CLIENTS_PER_CARD * card

        # The name has to agree, or the rule above has stopped being true
        port, name = None, None
        for cand, cname in ports():
            if cand.split(":")[0] == str(client):
                port, name = cand, cname
                break
        if port and product not in name:
            port = None

        codec = (re.search(r"TAC\d+", product) or [None])
        codec = codec.group(0) if hasattr(codec, "group") else None

        found.append({
            "serial": serial,
            "product": product,
            "codec": codec,
            "card": card,
            "port": port,
            # Unique and short, for saying which board a number came from
            "label": "%s/%s" % (codec or product.split()[0], serial[-4:]),
        })
    return sorted(found, key=lambda d: d["serial"])


def find(match, among=None):
    """The one pedal matching 'match', or None if it is not exactly one.

    Matches a serial, a label or a product string, and refuses to guess:
    two boards of the same codec both match "TAC5242", and answering
    either of them is how a test ends up measuring the wrong board.
    """
    pedals = among if among is not None else discover()
    m = match.lower()
    hits = [d for d in pedals
            if m in d["serial"].lower() or m in d["label"].lower()
            or m in (d["product"] or "").lower()]
    return hits[0] if len(hits) == 1 else None


def dongle(match=""):
    """A sequencer port that is not a pedal - the USB-MIDI adapter.

    The hardware MIDI jacks go to the UART rather than to USB, so they
    are reachable only through something else plugged into them, and
    that something is not discoverable the way a pedal is: it has no
    serial we care about and no audio side to join to.  It is simply
    the MIDI port that is not one of ours.
    """
    try:
        out = subprocess.run(["aplaymidi", "-l"], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\d+:\d+)\s+(.*\S)", line)
        if not m or "pedal" in m.group(2).lower():
            continue
        if match.lower() in m.group(2).lower():
            return m.group(1)
    return None


def midi_listen(port, seconds=1.5, during=None):
    """Every byte that arrives on a port, as a list of ints.

    'during' is called once the listener is actually up, for whatever is
    supposed to provoke the bytes.  Sending first and listening after
    loses anything that arrives while amidi is still opening the device -
    which does not show on a stream that never stops, like the status
    CCs, and shows badly on a single note.  Same bargain as
    audio.capture()'s 'during'.
    """
    dev = rawmidi(port)
    if not dev:
        return []
    #
    # Stopped by the clock here rather than by amidi's own -t, which is
    # an *idle* timeout: it restarts on every byte, and a pedal streams
    # its status CCs continuously, so a listener with -t on one never
    # returns at all.  That is how the first attempt at this wedged the
    # device and left it busy for everything after it.
    #
    try:
        proc = subprocess.Popen(["amidi", "-p", dev, "-d"],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True)
    except FileNotFoundError:
        return []
    try:
        time.sleep(0.3)          # let it get the device open
        if during:
            during()
        time.sleep(seconds)
    finally:
        proc.terminate()
        out = proc.stdout.read()
        proc.wait()
    return [int(b, 16) for b in out.split()]


def midi_alive(port, seconds=1.5):
    """Is a pedal running, asked without USB.

    The pedal streams its status CCs - 102, 103 and 104 - out of the
    hardware MIDI jack whether or not anything is listening, so this
    needs nothing sent and no reply parsed.  It is the one question
    that can still be put to a pedal whose USB never came up: firmware
    running, or nothing running at all.  See issue 86.
    """
    seen = midi_listen(port, seconds)
    return any(seen[i] == 0xB0 and seen[i + 1] in (102, 103, 104)
               for i in range(len(seen) - 2))


def _smf(payloads):
    """One standard MIDI file carrying several messages, all at time zero."""
    ev = b""
    for payload in payloads:
        body = payload[1:]              # the F0 is the event tag
        ev += bytes([0x00, 0xF0, len(body)]) + body
    ev += bytes([0x00, 0xFF, 0x2F, 0x00])
    hdr = bytes([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0x60])
    return hdr + b"MTrk" + len(ev).to_bytes(4, "big") + ev


#
# The raw MIDI device behind a sequencer port, remembered once.
#
# ALSA numbers a card-bound sequencer client 16 + 4*card, which is the
# rule discover() uses to find a port from a card; this is the same rule
# run backwards.  Checked against the devices amidi can actually see
# rather than trusted, and anything that does not resolve simply falls
# back to the sequencer.
#
_rawmidi_cache = {}


def _rawmidi_devices():
    if None not in _rawmidi_cache:
        try:
            out = subprocess.run(["amidi", "-l"], capture_output=True,
                                 text=True, check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            out = ""
        _rawmidi_cache[None] = set(re.findall(r"hw:\d+,\d+,\d+", out))
    return _rawmidi_cache[None]


def rawmidi(port):
    """hw:C,D,S for a sequencer port, or None if it cannot be had."""
    if port in _rawmidi_cache:
        return _rawmidi_cache[port]

    dev = None
    try:
        card, rem = divmod(int(port.split(":")[0]) - SEQ_GLOBAL_CLIENTS,
                           SEQ_CLIENTS_PER_CARD)
        if card >= 0 and not rem:
            cand = "hw:%d,0,0" % card
            dev = cand if cand in _rawmidi_devices() else None
    except ValueError:
        dev = None
    _rawmidi_cache[port] = dev
    return dev


def _play(p, payloads):
    #
    # The raw device if it can be had, because aplaymidi costs two
    # seconds a call and amidi costs a millisecond.
    #
    # That is not a wake-up being paid: 'aplaymidi -l' talks to the same
    # daemon and returns instantly, and the two seconds is the same
    # 2.001s every time - it holds its sequencer queue open to let it
    # drain.  amidi writes the bytes and returns.
    #
    # The sequencer is still the fallback, and the reason it was the
    # default is still a real one: another program can hold the raw
    # device, and on a machine where something does, this finds out by
    # being refused rather than by being told in advance.
    #
    dev = rawmidi(p)
    if dev:
        hexed = " ".join("%02X" % b for m in payloads for b in m)
        r = subprocess.run(["amidi", "-p", dev, "-S", hexed],
                           capture_output=True, text=True)
        if not r.returncode:
            time.sleep(0.06)
            return

    tmp = tempfile.NamedTemporaryFile(suffix=".mid", delete=False)
    try:
        tmp.write(_smf(payloads))
        tmp.close()
        r = subprocess.run(["aplaymidi", "-p", p, tmp.name],
                           capture_output=True, text=True)
    finally:
        os.unlink(tmp.name)
    if r.returncode:
        sys.exit(f"pedal: aplaymidi failed:\n{r.stderr}")
    #
    # One settle for the whole file, rather than one per message.
    #
    # This used to be per message, on the grounds that the pedal acts on
    # them from its main loop at 25Hz and back-to-back writes would land
    # in the same tick.  Landing in the same tick turns out to be fine -
    # usb_midi_poll() drains every packet it can see in one pass and
    # hands each complete message to handle_sysex_payload() - and it was
    # costing a process spawn per parameter.  Checked by putting 6, 12,
    # 24, 48 and 96 writes in one file and reading every one of them back
    # out of a state dump: all landed, at every size.
    #
    time.sleep(0.06)


def send(p, *payload):
    _play(p, [HEADER + bytes(payload) + bytes([0xF7])])


def send_many(p, *messages):
    """Several SysEx messages down one invocation of aplaymidi.

    Each message is the payload without the F0 7D in front or the F7
    behind, the same as send() takes.
    """
    _play(p, [HEADER + bytes(m) + bytes([0xF7]) for m in messages])


def set_pots(p, *triples):
    """Several (effect, pot, value) at once."""
    send_many(p, *[(0x03, eff, pot, val) for eff, pot, val in triples])


def program_change(p, scene, channel=1):
    """Load a scene.  Not SysEx - a plain Program Change."""
    tmp = tempfile.NamedTemporaryFile(suffix=".mid", delete=False)
    try:
        ev = bytes([0x00, 0xC0 | (channel - 1), scene,
                    0x00, 0xFF, 0x2F, 0x00])
        hdr = bytes([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0x60])
        tmp.write(hdr + b"MTrk" + len(ev).to_bytes(4, "big") + ev)
        tmp.close()
        subprocess.run(["aplaymidi", "-p", p, tmp.name], check=True,
                       capture_output=True)
    finally:
        os.unlink(tmp.name)
    # Loading a scene re-inits every effect; give it a tick to settle
    # before anything measures what came out.
    time.sleep(0.5)


def enter_bootsel(p, channel=1):
    """CC 20 value 126 - the way in without touching the board.

    There is no picotool reset interface on this device, so this is the
    only way to get it into BOOTSEL from software.  MIDI_CC_MAP.md has
    the number frozen for exactly this reason.
    """
    tmp = tempfile.NamedTemporaryFile(suffix=".mid", delete=False)
    try:
        ev = bytes([0x00, 0xB0 | (channel - 1), 20, 126,
                    0x00, 0xFF, 0x2F, 0x00])
        hdr = bytes([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0x60])
        tmp.write(hdr + b"MTrk" + len(ev).to_bytes(4, "big") + ev)
        tmp.close()
        subprocess.run(["aplaymidi", "-p", p, tmp.name], check=True,
                       capture_output=True)
    finally:
        os.unlink(tmp.name)


def identity(p, in_port=None, wait=2.0):
    """The pedal's self-description, as a dict, or None.

    SysEx 0x0a, answered as JSON - build stamp, what answered on the i2c
    bus, and what the save area holds.
    """
    import json
    dump = subprocess.Popen(["aseqdump", "-p", in_port or p],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True)
    try:
        time.sleep(0.4)
        send(p, 0x0A)
        time.sleep(wait)
    finally:
        dump.terminate()
        text = dump.stdout.read()
        dump.wait()

    blob = bytes.fromhex("".join(
        re.findall(r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)", text)
    ).replace(" ", ""))
    i = blob.find(bytes([0xF0, 0x7D, 0x0A]))
    if i < 0:
        return None
    try:
        return json.loads(blob[i + 3:blob.find(0xF7, i)].decode())
    except (ValueError, UnicodeDecodeError):
        return None


def telemetry(p, in_port=None, wait=1.5):
    """What the pedal thinks its own levels are.

    SysEx 0x0b, answered as packed bytes: version, input peak, noise
    floor, output peak, gate, load.  The levels are -dBFS, one byte per
    dB, counting down from full scale.
    """
    dump = subprocess.Popen(["aseqdump", "-p", in_port or p],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True)
    try:
        time.sleep(0.4)
        send(p, 0x0B)
        time.sleep(wait)
    finally:
        dump.terminate()
        text = dump.stdout.read()
        dump.wait()

    blob = bytes.fromhex("".join(
        re.findall(r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)", text)
    ).replace(" ", ""))
    i = blob.find(bytes([0xF0, 0x7D, 0x0B]))
    if i < 0:
        return None
    body = blob[i + 3:blob.find(0xF7, i)]
    if len(body) < 6:
        return None
    out = {"version": body[0], "in_dbfs": -body[1], "floor_dbfs": -body[2],
           "out_dbfs": -body[3], "gate": body[4], "load": body[5]}
    #
    # From layout 2 the load carries an LSB after it, so it is really a
    # 14-bit number whose top seven bits sit where the old one did.
    # Taken by presence rather than by version, which is the rule the
    # whole layout is read by - see handleTelemetry() in the app.
    #
    if len(body) >= 7:
        out["load14"] = (body[5] << 7) | body[6]
    return out


def set_pot(p, effect, pot, value):
    """Pot 0 is the mix; 1-10 are the effect's own."""
    send(p, 0x03, effect, pot, value)


def set_routing(p, *effect_ids):
    send(p, 0x08, *effect_ids)


def save_scene(p, scene):
    send(p, 0x04, scene)


def wet_dry(p, settings_effect):
    """Put the processed signal and the raw input side by side."""
    set_pot(p, settings_effect, SETTINGS_USB_OUT, USB_OUT_WET_DRY)


def effect_count(map_h="../build/effect_map.h"):
    """How many effects this firmware has, so 'the last one' has a number."""
    try:
        text = open(map_h).read()
    except OSError:
        return None
    m = re.search(r"#define EFFECT_COUNT (\d+)", text)
    return int(m.group(1)) if m else None
