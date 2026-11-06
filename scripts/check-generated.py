#!/usr/bin/env python3
#
# Is the generated effect metadata still what Effects/ says it should be?
#
# gen_effects.py turns Effects/*.h into five files: effect_map.h and
# midi_schema.h for the firmware and the bench, effects.js for the web
# app, and MIDI_CC_MAP.md for people.  The ones in the source tree are
# not committed and the build rewrites them, so a copy left over from
# before an effect was added or a pot changed looks perfectly healthy
# and describes a pedal that no longer exists.  The app then draws
# controls for the old layout, and the failure is as quiet as a stale
# effect id: nothing errors, a knob is just wrong.
#
# This regenerates into a scratch directory and compares.  Any generated
# file that is present and differs is stale, and the message says which
# one and what regenerates it.  A file that is not there is not stale,
# only unbuilt, and is reported as skipped so that a clean checkout
# passes.
#
# Called as: check-generated.py [<repo-root>]
#
import filecmp
import subprocess
import sys
import tempfile
from pathlib import Path

# (name the generator writes, where copies live relative to the root)
GENERATED = [
    ("effect_map.h", ["build/effect_map.h",
                      "Validation/bench/gen/effect_map.h"]),
    ("midi_schema.h", ["build/midi_schema.h",
                       "Validation/bench/gen/midi_schema.h"]),
    ("effects.js", ["WebMIDI/effects.js"]),
    ("MIDI_CC_MAP.md", ["Documentation/MIDI_CC_MAP.md"]),
]


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1
                else Path(__file__).resolve().parent.parent)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        subprocess.run([sys.executable, str(root / "scripts/gen_effects.py"),
                        str(root / "Effects"),
                        str(tmp / "effect_map.h"), str(tmp / "effects.js"),
                        str(tmp / "MIDI_CC_MAP.md")], check=True)

        stale = []
        checked = 0
        for name, copies in GENERATED:
            fresh = tmp / name
            if not fresh.exists():
                continue
            for rel in copies:
                path = root / rel
                if not path.exists():
                    print(f"check-generated: {rel}: not built, skipped")
                    continue
                checked += 1
                if not filecmp.cmp(fresh, path, shallow=False):
                    stale.append(rel)

    for rel in stale:
        print(f"check-generated: {rel} is stale")
    if stale:
        print("check-generated: regenerate with 'cmake --build build' or "
              "'python3 scripts/gen_effects.py Effects/ <effect_map.h> "
              "WebMIDI/effects.js Documentation/MIDI_CC_MAP.md'")
        return 1

    print(f"check-generated: {checked} generated file(s) up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
