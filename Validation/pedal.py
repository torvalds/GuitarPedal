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
import select
import subprocess
import sys
import tempfile
import time

import effectmap
import pots

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")

HEADER = bytes([0xF0, 0x7D])

# The signal chain is priority 0 and therefore effect 0.  It is the one
# id written down on purpose, and check-effect-ids.py checks it against
# the map rather than exempting it.
CHAIN = 0


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

        #
        # The product string is "<board> <codec> Pedal": the board name is
        # compile-time because it is the pin map, and the codec is what
        # probe_hardware() found.  Only the first word is a fact about
        # which build this is - what the probe said is in the identity
        # reply, and capabilities() is where to ask.
        #
        board = product.split()[0]

        found.append({
            "serial": serial,
            "product": product,
            "board": board,
            "card": card,
            "port": port,
            # Unique and short, for saying which board a number came from
            "label": "%s/%s" % (board, serial[-4:]),
        })
    return sorted(found, key=lambda d: d["serial"])


def find(match, among=None):
    """The one pedal matching 'match', or None if it is not exactly one.

    Matches a serial, a label or a product string, and refuses to guess:
    two boards of one revision both match their board name, and
    answering either is how a test measures the board nobody asked about.
    """
    pedals = among if among is not None else discover()
    m = match.lower()
    hits = [d for d in pedals
            if m in d["serial"].lower() or m in d["label"].lower()
            or m in (d["product"] or "").lower()]
    return hits[0] if len(hits) == 1 else None


def capabilities(d, ident=None):
    """What a board can do, out of its own identity reply.

    Read by presence: a board old enough not to carry a key does not
    have the thing it names, and a version number to compare against is
    what the telemetry block got wrong.

    The codec description comes from the product string rather than from
    the reply, because every board sets it and firmware old enough to
    omit "codec" from the reply still says it there.  What it means is
    per family - "mono"/"stereo" on the boards with an audio card,
    "DC-coupled"/"AC-coupled" on minimal - so 'stereo' is None on a board
    that did not say either way rather than a guess.
    """
    ident = pedal_identity(d) if ident is None else ident
    found = (ident or {}).get("found", {})

    # "<board> <desc> Pedal"
    words = (d.get("product") or "").split()
    desc = " ".join(words[1:-1]) if len(words) > 2 else None
    desc = found.get("codec", desc)

    return {
        "board": d.get("board"),
        "build": (ident or {}).get("build"),
        "codec": desc,
        "stereo": {"stereo": True, "mono": False}.get(desc),
        "dc_coupled": None if desc is None else desc == "DC-coupled",
        "midi_hw": (ident or {}).get("midi_hw"),
        "controls": (ident or {}).get("controls", []),
        "scenes": (ident or {}).get("scenes"),
    }


def pedal_identity(d):
    return identity(d["port"]) if d.get("port") else None


def matches(target, among=None):
    """Every pedal 'target' names.

    find() collapses to "exactly one or nothing", and the difference
    between nothing matched and several matched is worth having: one of
    them is worth waiting for and the other never will be.
    """
    pedals = among if among is not None else discover()
    return [d for d in pedals if find(target, among=[d])]


def sole(target=None, among=None):
    """The one pedal to drive, as (device, why not) - one is always None.

    Refusing to guess is the default, for the reason find() gives.  What
    to do about a refusal stays with the caller, because a check target
    skips with 0 and a tool stops with a message, and the difference
    between those is not this function's to decide.
    """
    found = among if among is not None else discover()
    if not found:
        return None, "no pedal on the USB"

    here = ", ".join(d["label"] for d in found)
    if target:
        hits = matches(target, found)
        if len(hits) == 1:
            return _unheld(hits[0])
        return None, ("%r names %s of the %d pedals here: %s"
                      % (target, len(hits) or "none", len(found), here))
    if len(found) > 1:
        return None, ("%d pedals and no target; this wants exactly one: %s"
                      % (len(found), here))
    return _unheld(found[0])


def _unheld(d):
    """(board, None), or (None, who has its port) - see port_holder().

    Checked here because this is where every script picks a board, and
    because the alternative is each of them discovering it as "the pedal
    did not answer" several seconds later.
    """
    held = port_holder(d["port"]) if d.get("port") else None
    if held and held["kind"] == "raw":
        return None, "%s: %s" % (d["label"], held["why"])
    return d, None


def dongles(match=""):
    """Every sequencer port that is not a pedal, in order.

    An adapter can have more than one DIN pair - the MIDIMATE has two,
    48:0 and 48:1 - and which of them a pedal is wired to is a fact
    about the bench rather than about the adapter.  So a caller that
    wants to find the pedal tries them.
    """
    try:
        out = subprocess.run(["aplaymidi", "-l"], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return []
    found = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\d+:\d+)\s+(.*\S)", line)
        if not m or "pedal" in m.group(2).lower():
            continue
        if match.lower() in m.group(2).lower():
            found.append(m.group(1))
    return found


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
        return list(_listen(["amidi", "-p", dev, "-a", "-c",
                             "-r", "/dev/stdout"], during, seconds,
                            settle=0.3))
    except FileNotFoundError:
        return []


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
    """hw:C,D,S for a sequencer port, or None if it cannot be had.

    The client number gives the card by the 16 + 4*N rule, and the port
    number after the colon is the subdevice on it - 'ESI MIDIMATE eX
    MIDI 2' is 48:1 and hw:8,0,1.  A pedal has one port and it is always
    zero, which is why this used to read the client and ignore the rest,
    and why a two-port adapter came back as its own first port whichever
    half was asked for.
    """
    if port in _rawmidi_cache:
        return _rawmidi_cache[port]

    dev = None
    try:
        client, _, sub = port.partition(":")
        card, rem = divmod(int(client) - SEQ_GLOBAL_CLIENTS,
                           SEQ_CLIENTS_PER_CARD)
        if card >= 0 and not rem:
            cand = "hw:%d,0,%d" % (card, int(sub or 0))
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


#
# CC 20 is bypass on 127 and BOOTSEL on 126, so the two live together and
# the one-off byte between them is worth having in one place.
#
def _cc(p, number, value, channel=1):
    tmp = tempfile.NamedTemporaryFile(suffix=".mid", delete=False)
    try:
        ev = bytes([0x00, 0xB0 | (channel - 1), number, value,
                    0x00, 0xFF, 0x2F, 0x00])
        hdr = bytes([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0x60])
        tmp.write(hdr + b"MTrk" + len(ev).to_bytes(4, "big") + ev)
        tmp.close()
        subprocess.run(["aplaymidi", "-p", p, tmp.name], check=True,
                       capture_output=True)
    finally:
        os.unlink(tmp.name)


def set_bypass(p, on, channel=1):
    """CC 20: 127 enables the chain, 0 bypasses it.

    Worth sending explicitly at the top of any measurement rather than
    assuming.  A bypassed pedal crossfades the whole chain away, so a
    routed effect looks dead and a patch cable from the output back to
    the input closes a unity feedback loop that reads as a wild
    frequency-dependent gain - two symptoms that both look like firmware
    bugs and are not.  The state is on the wire in the SysEx dump but as
    a *channel* message, which every parser in here steps over; that is
    issue 253, and this is the way round it until it is fixed.

    The tell, if it is ever in doubt: with the chain bypassed, muting
    Signal Chain's Volume changes nothing, because bypass discards
    volume along with everything else.

    aplaymidi rather than the raw device, because the raw device is
    taken whenever the web app is open and this has to work anyway.
    """
    _cc(p, 20, 127 if on else 0, channel)
    time.sleep(0.2)


def enter_bootsel(p, channel=1):
    """CC 20 value 126 - the way in without touching the board.

    There is no picotool reset interface on this device, so this is the
    only way to get it into BOOTSEL from software.  MIDI_CC_MAP.md has
    the number frozen for exactly this reason.
    """
    _cc(p, 20, 126, channel)


def port_holder(p):
    """What else has this pedal's MIDI input open, or None.

    "The pedal did not answer" is more often this than anything wrong
    with the pedal, and it is one cheap question rather than a guess:
    ALSA gives a rawmidi input one owner, and /proc names the process.

    How that owner got there decides whether anything here can still
    hear a reply, so the two are told apart by what it has open.  A
    sequencer client - the web app in a tab - holds the raw device
    underneath itself and the sequencer still fans replies out to
    everyone subscribed.  Another raw reader takes the bytes before the
    sequencer sees them, and then nothing here will hear anything at
    all, however long it waits.
    """
    dev = rawmidi(p)
    if not dev:
        return None
    _hw, card, _device, sub = re.split(r"[:,]", dev)
    try:
        text = open("/proc/asound/card%s/midi0" % card).read()
    except OSError:
        return None

    #
    # A section per subdevice, and the owner belongs to one of them: an
    # adapter with two DIN pairs has an "Input 0" and an "Input 1", and
    # taking the first owner in the file would call both of them busy
    # whichever one was open.
    #
    m = re.search(r"^Input %s\b(.*?)(?=^\S|\Z)" % re.escape(sub),
                  text, re.M | re.S)
    if not m:
        return None
    m = re.search(r"Owner PID\s*:\s*(\d+)", m.group(1))
    if not m or int(m.group(1)) == os.getpid():
        return None
    pid = int(m.group(1))

    try:
        name = open("/proc/%d/comm" % pid).read().strip()
    except OSError:
        name = "pid %d" % pid

    kind = "seq"
    try:
        for fd in os.listdir("/proc/%d/fd" % pid):
            try:
                if os.readlink("/proc/%d/fd/%s" % (pid, fd)).startswith(
                        "/dev/snd/midi"):
                    kind = "raw"
                    break
            except OSError:
                pass
    except OSError:
        pass

    return {"pid": pid, "name": name, "kind": kind,
            "why": "%s (pid %d) has the raw MIDI device open, so it is "
                   "taking the pedal's replies before anything else can "
                   "see them" % (name, pid) if kind == "raw" else
                   "%s (pid %d) has the port open through the sequencer"
                   % (name, pid)}


def listen_sysex(p, opcode, wait, in_port=None):
    """Send one request and hand back every byte that came back.

    The reply is read *while* it arrives.  A dumper writes three
    characters per byte, so a long reply is more than a pipe will hold,
    and a pipe nobody is reading stops the dumper dead: it blocks on the
    write and takes nothing more off the MIDI device.  Collecting only
    after the dumper has been killed therefore caps every capture at one
    pipeful, and what did arrive looks exactly like the pedal giving up
    partway through.

    The raw device first, because the sequencer's pool drops a long
    reply, and the sequencer second.  A sequencer client - the web app
    in a tab - holds the raw device open underneath itself, so the fast
    route is refused and the slow one still works, which is the case the
    fallback is for.

    Another *raw* reader is a different thing and nothing here helps: it
    takes the bytes before the sequencer sees them, so both routes come
    back empty and the pedal looks silent while it is answering
    perfectly well.  That is what port_holder() is asked about above.
    """
    held = port_holder(p) if in_port is None else None
    if held and held["kind"] == "raw":
        #
        # Waiting here is waiting for a reply somebody else is already
        # taking, for however long the caller asked for.
        #
        return b""

    dev = rawmidi(p) if in_port is None and not held else None
    if dev:
        #
        # -r rather than -d: the hex dump is three characters a byte and
        # is meant to be read by a person, and this reads it straight
        # back.  -a -c so that active sensing and clock arrive instead of
        # being filtered out here, because a byte nobody ever sees is a
        # byte nobody can notice the pedal sending.
        #
        got = _listen(["amidi", "-p", dev, "-a", "-c", "-r", "/dev/stdout"],
                      lambda: send(p, opcode), wait)
        if got:
            return got

    text = _listen(["aseqdump", "-p", in_port or p],
                   lambda: send(p, opcode), wait)
    return bytes.fromhex("".join(re.findall(
        r"System exclusive\s+((?:[0-9A-Fa-f]{2} ?)+)",
        text.decode("ascii", "replace"))).replace(" ", ""))


def sysex_payload(blob, opcode):
    """The body of an 'F0 7D <opcode> ... F7' in a captured stream.

    The last one, because a request from before this one still arriving
    leaves a partial reply in front of the real one, and half a message
    parses rather than failing.

    Real-time bytes are dropped here rather than upstream.  MIDI lets
    them appear between any two bytes of a longer message, including
    inside a SysEx, so a payload taken as a byte range has to allow for
    it - and this pedal has never been seen to send one, which is worth
    being able to find out rather than arranging not to see.
    """
    at = blob.rfind(bytes([0xF0, 0x7D, opcode]))
    if at < 0:
        return None
    end = blob.find(0xF7, at)
    if end < 0:
        return None
    return bytes(b for b in blob[at + 3:end] if not 0xF8 <= b <= 0xFE)


def _listen(cmd, provoke, wait, settle=0.4):
    """Run a dumper, provoke the reply, and read its output as it comes.

    'provoke' is called once the dumper is actually up.  Provoking first
    and listening after loses whatever arrives while it is still opening
    the device, which does not show on a stream that never stops - like
    the status CCs - and shows badly on a single note.
    """
    dump = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    chunks = []

    def collect(seconds):
        fd = dump.stdout.fileno()
        until = time.monotonic() + seconds
        while True:
            left = until - time.monotonic()
            if left <= 0 or not select.select([fd], [], [], left)[0]:
                return
            b = os.read(fd, 1 << 16)
            if not b:
                return              # the dumper is gone
            chunks.append(b)

    try:
        collect(settle)             # let the dumper get the device open
        if provoke:
            provoke()
        collect(wait)
    finally:
        dump.terminate()
        collect(0.2)                # and whatever is still in the pipe
        dump.wait()
    return b"".join(chunks)


def identity(p, in_port=None, wait=2.0):
    """The pedal's self-description, as a dict, or None.

    SysEx 0x0a, answered as JSON - build stamp, what answered on the i2c
    bus, and what the save area holds.
    """
    import json
    body = sysex_payload(listen_sysex(p, 0x0A, wait, in_port=in_port), 0x0A)
    if body is None:
        return None
    try:
        return json.loads(body.decode())
    except (ValueError, UnicodeDecodeError):
        return None


def schema(p, wait=6.0):
    """The effect map the board is actually running, or None.

    SysEx 0x01 asking, 0x02 answering, and the answer is the same JSON
    the build writes into midi_schema.h - so effectmap reads it either
    way and only the transport differs.  Which matters because the build
    describes a commit and this describes the board in front of you.
    """
    body = sysex_payload(listen_sysex(p, 0x01, wait), 0x02)
    if body is None:
        return None
    try:
        return effectmap.parse(body.decode())
    except (ValueError, UnicodeDecodeError):
        return None


def telemetry(p, in_port=None, wait=1.5):
    """What the pedal thinks its own levels are.

    SysEx 0x0b, answered as packed bytes: version, input peak, noise
    floor, output peak, gate, load.  The levels are -dBFS, one byte per
    dB, counting down from full scale.
    """
    body = sysex_payload(listen_sysex(p, 0x0B, wait, in_port=in_port), 0x0B)
    if body is None or len(body) < 6:
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
    # ...and from layout 3, where the treadle is, raw and unscaled.
    if len(body) >= 9:
        out["treadle"] = (body[7] << 7) | body[8]
    if len(body) >= 13:
        out["treadle_lo"] = (body[9] << 7) | body[10]
        out["treadle_hi"] = (body[11] << 7) | body[12]
    return out


def set_pot(p, effect, pot, value):
    """Pot 0 is the mix; 1-10 are the effect's own."""
    send(p, 0x03, effect, pot, value)


def set_named(p, effect, label, value):
    """One pot, named all the way down.

    The effect, the pot and - for an enum - the setting, each spelled the
    way the header spells it, so nothing between here and the board has a
    position written down in it.
    """
    set_pot(p, effectmap.effect(effect), effectmap.pot(effect, label),
            pots.to_pot(effect, label, value))


def set_routing(p, *effect_ids):
    send(p, 0x08, *effect_ids)


def save_scene(p, scene):
    send(p, 0x04, scene)


def wet_dry(p, settings_effect):
    """Put the processed signal and the raw input side by side.

    Wet/Dry is the processed signal on the left and the untouched input
    on the right, in the same frame.
    """
    set_pot(p, settings_effect, effectmap.pot("Settings", "USB L/R Out"),
            pots.to_pot("Settings", "USB L/R Out", "Wet/Dry"))


class Stale(Exception):
    """The board is running firmware this tree cannot vouch for."""


def elf_build(board):
    """The build stamp in this tree's elf for that board, or None.

    The firmware builds "{\"build\":\"" __DATE__ " " __TIME__ "\"" into
    the reply to SysEx 0x0a, so the same literal is sitting in the binary
    and the two can simply be compared.

    Named for the board, because every artifact here is: there is no
    pedal.elf, and comparing a minimal board against unified's stamp
    refuses a board that is fine and accepts one that is not.
    """
    try:
        blob = open(os.path.join(BUILD, "pedal-%s.elf" % board), "rb").read()
    except OSError:
        return None
    m = re.search(rb'\{"build":"([^"]*)"', blob)
    return m.group(1).decode() if m else None


def use_map(d, strict=False):
    """Point the lookups at the map this board is running, and say which.

    Two questions with one answer.  A board that can hand over its own
    schema settles it: what it says is what it is.  A board that cannot -
    no MIDI port, or firmware older than the schema request - leaves the
    build's map as the only one there is, and that is the right map only
    if the board is running this tree.

    'strict' is the difference between a test and a tool.  A test refuses
    firmware it cannot vouch for, because an effect id that has moved
    lands on a real pot of a real effect and says nothing.  A tool says
    what it is working from and carries on.
    """
    running = schema(d["port"]) if d.get("port") else None
    if running:
        effectmap.use(running)
        return "effects from the pedal"

    effectmap.use(None)
    want = elf_build(d["board"])
    got = (identity(d["port"]) or {}).get("build") if d.get("port") else None
    if want and got and want == got:
        return "effects from ../build, which is what %s is running" % d["label"]

    why = ("%s is running %r and this tree builds %r"
           % (d["label"], got or "nothing it would say", want or "nothing"))
    if strict:
        raise Stale(why + " - run 'make flash'")
    return "effects from ../build, unverified: " + why
