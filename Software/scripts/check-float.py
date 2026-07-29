#!/usr/bin/env python3
#
# Check that the firmware contains no double-precision arithmetic, and
# no calls into the SDK's math libraries.
#
# The FPU on this part is single precision only.  A 'double' that sneaks
# in doesn't fail to build - it quietly pulls in the AAPCS software
# helpers, which are hundreds of cycles where the hardware would have
# taken one.  That is why the audio code has its own pow2/log2 tables and
# fastsincos() instead of calling libm, and why -fsingle-precision-constant
# and -Wdouble-promotion are on.  This is the backstop for all of it: the
# helpers are named, so if one is in the image we can say which.
#
# The wrappers are the other half.  pico_float and pico_double interpose
# on sinf(), pow() and the rest with -Wl,--wrap, so a call to one of them
# is invisible by name: no link error, no warning, it just turns up in
# the binary.  We do all of that arithmetic with the generated tables
# instead, so one of these appearing means something started calling libm.
#
# Called as: check-float.py <elf> [nm]
#
import re
import subprocess
import sys

#
# The AAPCS double helpers.  Two shapes: everything operating on a double
# ('__aeabi_dadd', '__aeabi_cdcmple', '__aeabi_d2iz'), and the promotions
# into one ('__aeabi_i2d', '__aeabi_f2d').
#
DOUBLE = re.compile(r"^__aeabi_(c?d|(f|i|ui|l|ul)2d$)")

#
# The functions pico_float and pico_double interpose on.  Both the double
# name and the float one - pico_float wraps 'sinf', pico_double 'sin'.
#
MATH = """sqrt cos sin tan atan2 exp log ldexp copysign trunc floor ceil
	  round sincos asin acos atan sinh cosh tanh asinh acosh atanh
	  exp2 log2 exp10 log10 pow powint hypot cbrt fmod drem remainder
	  remquo expm1 log1p fma""".split()

WRAPPED = {"__wrap_" + m for m in MATH} | {"__wrap_" + m + "f" for m in MATH}


def main():
    elf = sys.argv[1]
    nm = (sys.argv[2] if len(sys.argv) > 2 and sys.argv[2]
          else "arm-none-eabi-nm")

    res = subprocess.run([nm, elf], capture_output=True, text=True)
    if res.returncode:
        sys.exit(f"check-float: {nm} failed:\n{res.stderr}")

    bad = []
    for line in res.stdout.splitlines():
        field = line.split()
        if len(field) < 2:
            continue
        name = field[-1]
        if DOUBLE.match(name):
            bad.append((name, "double-precision helper"))
        elif name in WRAPPED or name.startswith("__wrap___aeabi_"):
            bad.append((name, "libm call routed through pico_float/pico_double"))

    if bad:
        print(f"check-float: {len(bad)} symbol(s) that should not be here:",
              file=sys.stderr)
        for name, why in sorted(bad):
            print(f"    {name} - {why}", file=sys.stderr)
        print("\nLook for a bare constant without its 'f', an implicit "
              "promotion,\nor a math call that wants doing with a table "
              "instead.", file=sys.stderr)
        return 1

    print("check-float: no doubles, no libm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
