#!/usr/bin/env python3
#
# Check that nothing running on the audio core leaves it - neither by
# calling out of it, nor by reading out of it.
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
# The second check is about *data*, and it exists because of flash
# writes.  Erasing or programming a flash sector means switching XIP off,
# and while it is off the whole 0x10000000 window reads back nonsense.
# Core 0 is stopped for the duration and does not care.  Core 1 keeps
# playing, so every byte it touches has to be in RAM - and marking a
# function __audio_func() moves its *code*, not the tables it reads.
#
# That distinction is invisible in the source and silent at runtime: a
# 'static const float table[]' added to an effect lands in .rodata, in
# flash, and everything works perfectly until the first save, when the
# audio core reads garbage for as long as the erase takes.  So find them
# here, where it is a build failure with a symbol name on it.
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

# Flash reads allowed from the audio sections. Keep this shorter, and be
# certain: an entry here is a promise that no flash write can ever be in
# flight while the audio core reads it.
ALLOWED_FLASH = {
    # (nothing - and adding one should feel like a big decision)
}

# The XIP window. Anything the audio core reads from in here is the bug.
XIP_BASE = 0x10000000
XIP_END = 0x20000000


def flash_symbols(elf, objdump):
    """Every symbol in the XIP window, as (start, end, name).

    Sizes come from 'nm -S', but not every symbol has one - so a symbol
    without a size runs until the next one starts.  Without that, a
    literal pointing at a sizeless table matches nothing and the check
    quietly passes: 'tunings' is exactly such a symbol.
    """
    nm = objdump.replace("objdump", "nm")
    res = subprocess.run([nm, "-S", "--numeric-sort", elf],
                         capture_output=True, text=True)
    if res.returncode:
        sys.exit(f"check-audio: {nm} failed:\n{res.stderr}")

    syms = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, name = int(parts[0], 16), int(parts[1], 16), parts[3]
        elif len(parts) == 3:
            addr, size, name = int(parts[0], 16), 0, parts[2]
        else:
            continue
        if XIP_BASE <= addr < XIP_END:
            syms.append([addr, size, name])

    out = []
    for i, (addr, size, name) in enumerate(syms):
        end = addr + size
        if not size:
            end = syms[i + 1][0] if i + 1 < len(syms) else addr + 1
        out.append((addr, end, name))
    return out


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

    flash = flash_symbols(elf, objdump)

    def flash_owner(addr):
        for start, end, name in flash:
            if start <= addr < end:
                return name
        return None

    func = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
    call = re.compile(r"\s(?:bl|blx)\s+[0-9a-f]+ <([^>+]+)")
    #
    # Two ways an address gets into a register.  Usually a literal pool
    # entry, which objdump prints both as the '.word' itself and as a
    # '; 0x...' comment on the load - either spelling will do, they are
    # the same address.  Otherwise a movw/movt pair builds it inline,
    # which never shows up as a literal at all.
    #
    word = re.compile(r"0x([0-9a-f]{8})\b")
    movw = re.compile(r"\bmovw\s+(\w+),\s*#(\d+)")
    movt = re.compile(r"\bmovt\s+(\w+),\s*#(\d+)")

    cur, bad, seen = None, set(), set()
    reads, halves = set(), {}
    for line in res.stdout.splitlines():
        m = func.match(line)
        if m:
            cur = m.group(1)
            halves = {}
            if cur in audio:
                seen.add(cur)
            continue
        if cur not in audio:
            continue

        m = call.search(line)
        if m:
            callee = m.group(1)
            if callee not in audio and callee not in ALLOWED:
                bad.add((cur, callee))

        m = movw.search(line)
        if m:
            halves[m.group(1)] = int(m.group(2))
        m = movt.search(line)
        if m and m.group(1) in halves:
            addr = halves[m.group(1)] | (int(m.group(2)) << 16)
            owner = flash_owner(addr)
            if owner and owner not in ALLOWED_FLASH:
                reads.add((cur, owner))

        for hexval in word.findall(line):
            owner = flash_owner(int(hexval, 16))
            if owner and owner not in ALLOWED_FLASH:
                reads.add((cur, owner))

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

    if reads:
        print(f"check-audio: {len(reads)} read(s) of flash from the audio "
              f"core:", file=sys.stderr)
        for reader, sym in sorted(reads):
            print(f"    {reader} -> {sym}", file=sys.stderr)
        print("\nThese are in .rodata, which is in flash, and the audio core "
              "keeps\nrunning while core 0 erases a flash sector with XIP "
              "switched off -\nso it would read nonsense for the duration.  "
              "Mark the data\n__not_in_flash(\"audio\") to move it into RAM.",
              file=sys.stderr)
        return 1

    print(f"check-audio: {len(seen)} functions on the audio core, "
          f"no calls out, no flash reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
