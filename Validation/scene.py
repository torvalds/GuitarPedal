#
# Building a scene payload, from the host.
#
# There is no way to set an effect's channel routing over MIDI - that
# wants a helper UI rather than a raw byte and is deferred - so a test
# that needs one writes the scene directly and plants it with picotool.
#
# The identities come out of the *built* effect_map.h rather than being
# recomputed here.  gen_effects.py hashes a canonical description of
# each effect, and reimplementing that hash in a second language is how
# the two would come to disagree about which effect is which, which is
# the one thing a stored scene must not be wrong about.
#
# The layout is the other half of the _Static_asserts in
# Software/scene.h.  Those exist because two of the offsets here are not
# visible in the C declaration: a scene_effect is 22 bytes of fields in
# a 24-byte slot, and the array of them is 4-aligned so it starts at 24
# rather than 22.  Both were wrong here first time round.
#
import hashlib
import re
import struct
import sys

SLOT_SIZE = 4096
TAIL_SIZE = 64
PAYLOAD_SIZE = SLOT_SIZE - TAIL_SIZE
MARKER = b"SAVEAREA"
CONTAINER_VERSION = 1

SCENE_VERSION = 3
SCENE_MAX_EFFECTS = 32
MAX_ROUTED = 14
MAX_RULES = 16

EFFECT_SIZE = 24
EFFECTS_AT = 24
SCENE_SIZE = EFFECTS_AT + SCENE_MAX_EFFECTS * EFFECT_SIZE + MAX_RULES * 6

XIP_BASE = 0x10000000
AREA_OFFSET = 0x1C0000

# do_effect_step()'s two 2-bit fields, and zero is what an effect did
# before there was a choice: read the left channel, write both.
IN_LEFT, IN_RIGHT = 0, 1
OUT_BOTH, OUT_LEFT, OUT_RIGHT, OUT_MERGE = 0, 1, 2, 3


def channels(ch_in=IN_LEFT, ch_out=OUT_BOTH):
    return ch_in | (ch_out << 2)


def effects_from_map(path="../Software/build/effect_map.h"):
    """Every effect in the built firmware, in id order."""
    text = open(path).read()
    out = []
    for m in re.finditer(
            r'\.name = "([^"]*)",\s*\n\s*\.short_name = "([^"]*)",\s*\n'
            r'\s*\.id_hash = (0x[0-9a-f]+),\s*\n\s*\.pot_hash = (0x[0-9a-f]+),',
            text):
        out.append({"name": m.group(1), "short": m.group(2),
                    "id": int(m.group(3), 16), "pot": int(m.group(4), 16),
                    "pots": [0] * 10, "mix": 120, "channels": 0, "merge": 120})
    if not out:
        sys.exit(f"scene: no effects in {path} - built?")

    # The schema defaults, so a scene says what the pedal would have said
    # rather than zeroing every knob it does not mention.
    for eff, block in zip(out, text.split(".pots = {")[1:]):
        vals = re.findall(r'EFFECT_POT\("[^"]*",[^,]*,[^,]*,\s*(\d+)', block)
        for i, v in enumerate(vals[:10]):
            eff["pots"][i] = int(v)
    return out


def by_name(effects, name):
    """By full name, because the copies share a short one.

    'Tone 1' and 'Tone 2' are both [TONE] - that is what COPIES: means -
    so the short name cannot pick between them and the display name can.
    """
    for e in effects:
        if e["name"].lower() == name.lower():
            return e
    known = ", ".join(e["name"] for e in effects)
    sys.exit(f"scene: no effect named '{name}'. Have: {known}")


def build(effects, routed):
    """The payload.  'routed' is a list of indices into 'effects'."""
    body = struct.pack("<HBB", SCENE_VERSION, len(effects), len(routed))
    body += bytes(routed) + bytes(MAX_ROUTED - len(routed))
    body += struct.pack("<B", 0) + bytes(3)
    body = body.ljust(EFFECTS_AT, b"\0")

    for e in effects:
        body += struct.pack("<II", e["id"], e["pot"])
        body += bytes(e["pots"])
        body += struct.pack("<BBB", e["mix"], e["channels"], e["merge"])
        body += bytes(EFFECT_SIZE - 21)
    body += bytes(EFFECT_SIZE * (SCENE_MAX_EFFECTS - len(effects)))
    body += bytes(6 * MAX_RULES)

    if len(body) != SCENE_SIZE:
        sys.exit(f"scene: built {len(body)} bytes, "
                 f"struct scene_payload is {SCENE_SIZE}")
    return body


def slot_image(payload, key, seq):
    """Wrap a payload in the container flash_store.h expects."""
    body = payload.ljust(PAYLOAD_SIZE, b"\0")
    body += MARKER + struct.pack("<IHH", seq, CONTAINER_VERSION, key)
    body += b"\0" * 16
    return body + hashlib.sha256(body).digest()


def slot_address(n):
    return XIP_BASE + AREA_OFFSET + n * SLOT_SIZE
