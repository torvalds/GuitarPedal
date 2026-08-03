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


def discover():
    """Every pedal on the machine, as [{codec, port, card, name}].

    The two halves of a pedal - a sequencer port and an ALSA card - are
    found by two different tools that have no idea they are looking at the
    same device.  What ties them together is the USB product string, which
    both of them report and which names the codec, so the codec is the key.

    Which is the whole reason the pedals had to be told apart before any
    of this could be written: with both of them called "Linus Pedal" there
    is nothing here to join on.
    """
    import audio

    cards = {}
    for number, name in audio.find_cards():
        m = re.search(r"TAC\d+", name)
        if m:
            cards[m.group(0)] = (number, name)

    found = []
    for p, name in ports():
        m = re.search(r"TAC\d+", name)
        if not m or m.group(0) not in cards:
            continue
        number, card_name = cards[m.group(0)]
        found.append({"codec": m.group(0), "port": p,
                      "card": number, "name": card_name})
    return sorted(found, key=lambda d: d["codec"])


def _smf(payload):
    body = payload[1:]
    ev = bytes([0x00, 0xF0, len(body)]) + body + bytes([0x00, 0xFF, 0x2F, 0x00])
    hdr = bytes([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0x60])
    return hdr + b"MTrk" + len(ev).to_bytes(4, "big") + ev


def send(p, *payload):
    tmp = tempfile.NamedTemporaryFile(suffix=".mid", delete=False)
    try:
        tmp.write(_smf(HEADER + bytes(payload) + bytes([0xF7])))
        tmp.close()
        r = subprocess.run(["aplaymidi", "-p", p, tmp.name],
                           capture_output=True, text=True)
    finally:
        os.unlink(tmp.name)
    if r.returncode:
        sys.exit(f"pedal: aplaymidi failed:\n{r.stderr}")
    # The pedal acts on these from its main loop at 25Hz, so back-to-back
    # writes need to be spaced or the later ones land in the same tick.
    time.sleep(0.06)


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
    return {"version": body[0], "in_dbfs": -body[1], "floor_dbfs": -body[2],
            "out_dbfs": -body[3], "gate": body[4], "load": body[5]}


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


def effect_count(map_h="../Software/build/effect_map.h"):
    """How many effects this firmware has, so 'the last one' has a number."""
    try:
        text = open(map_h).read()
    except OSError:
        return None
    m = re.search(r"#define EFFECT_COUNT (\d+)", text)
    return int(m.group(1)) if m else None
