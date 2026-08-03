#!/usr/bin/env python3
#
# Check that the firmware image does not reach into the save area.
#
# The save area is a fixed region at the top of flash, addressed
# arithmetically rather than by a linker symbol, because the whole point
# is that it survives being reflashed - it is not part of the image and
# the image does not know how big it is.
#
# Which means nothing connects the two.  The image grows from the bottom
# and the save area sits at the top, and if they ever meet, the failure
# is that flashing the firmware silently destroys somebody's scenes, or
# that saving silently destroys the firmware.  There is a megabyte and a
# half between them today; this is here so that the day that stops being
# true is a build failure and not a mystery.
#
# Called as: check-flash.py <elf> <flash_size> <area_offset> <nm>
#
import subprocess
import sys

XIP_BASE = 0x10000000


def main():
    elf = sys.argv[1]
    flash_size = int(sys.argv[2], 0)
    area_offset = int(sys.argv[3], 0)
    nm = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else "arm-none-eabi-nm"

    res = subprocess.run([nm, elf], capture_output=True, text=True)
    if res.returncode:
        sys.exit(f"check-flash: {nm} failed:\n{res.stderr}")

    end = None
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "__flash_binary_end":
            end = int(parts[0], 16)
    if end is None:
        sys.exit("check-flash: no __flash_binary_end in the image - "
                 "a NO_FLASH build, or a stale one?")

    area = XIP_BASE + area_offset
    used = end - XIP_BASE
    if end > area:
        print(f"check-flash: the image reaches 0x{end:08x}, which is "
              f"0x{end - area:x} bytes into the save area at 0x{area:08x}.",
              file=sys.stderr)
        print("\nEither the firmware has to get smaller, or the save area has "
              "to\nmove up and lose slots - and moving it up means every "
              "pedal in\nexistence loses whatever was saved in the slots that "
              "went away.", file=sys.stderr)
        return 1

    print(f"check-flash: {used // 1024}kB of {flash_size // 1024}kB used, "
          f"{(area - end) // 1024}kB clear of the save area")
    return 0


if __name__ == "__main__":
    sys.exit(main())
