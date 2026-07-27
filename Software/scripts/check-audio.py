#!/usr/bin/env python3
#
# Check that nothing running on the audio core calls out of it.
#
# Everything marked with __audio_func() is compiled into its own
# '.time_critical.audio_<name>' section. Anything such a function calls
# has to be marked too - if it isn't, either it wants marking, or we have
# accidentally pulled a newlib or soft-float routine into the
# hard-realtime path.
#
# The linker merges all of those input sections into .data, so the
# section names only survive in the map file. That is fine: the macro
# builds the section name out of the function name, so the map is enough
# to recover the list.
#
# Called as: check-audio.py <map> <elf> [objdump]
#
import re
import subprocess
import sys

# Calls allowed out of the audio sections. Keep this short, and say why.
ALLOWED = {
    # (nothing yet - the audio path is entirely self-contained)
}


def main():
    mapfile, elf = sys.argv[1], sys.argv[2]
    objdump = (sys.argv[3] if len(sys.argv) > 3 and sys.argv[3]
               else "arm-none-eabi-objdump")

    with open(mapfile) as f:
        audio = set(re.findall(r"^ \.time_critical\.audio_(\S+)$",
                               f.read(), re.M))
    if not audio:
        sys.exit("check-audio: no __audio_func() symbols in the map - "
                 "stale build?")

    res = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True)
    if res.returncode:
        sys.exit(f"check-audio: {objdump} failed:\n{res.stderr}")

    func = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
    call = re.compile(r"\s(?:bl|blx)\s+[0-9a-f]+ <([^>+]+)")
    cur, bad, seen = None, set(), set()
    for line in res.stdout.splitlines():
        m = func.match(line)
        if m:
            cur = m.group(1)
            if cur in audio:
                seen.add(cur)
            continue
        m = call.search(line)
        if m and cur in audio:
            callee = m.group(1)
            if callee not in audio and callee not in ALLOWED:
                bad.add((cur, callee))

    # A marked function that vanished was fully inlined into its callers,
    # which is fine - they are marked too.
    if bad:
        print(f"check-audio: {len(bad)} call(s) leaving the audio core:",
              file=sys.stderr)
        for caller, callee in sorted(bad):
            print(f"    {caller} -> {callee}", file=sys.stderr)
        print("\nMark the callee with __audio_func(), or if it really has to "
              "live\nout of line, add it to ALLOWED in this script with a "
              "reason.", file=sys.stderr)
        return 1

    print(f"check-audio: {len(seen)} functions on the audio core, no calls out")
    return 0


if __name__ == "__main__":
    sys.exit(main())
