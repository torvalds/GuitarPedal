#!/usr/bin/env python3
import math
import sys
from pathlib import Path

def generate_sine(output_path, step_shift):
	steps = 1 << step_shift
	Path(output_path).parent.mkdir(parents=True, exist_ok=True)

	#
	# How far the chord falls short of the arc, as a coefficient.
	#
	# Linear interpolation between two table entries misses by
	# (h^2/2)*t*(1-t)*f'', with h the step in radians.  For a sinusoid
	# f'' is -f, so the miss is proportional to the value the chord just
	# produced and a caller can put it back with one multiply-add.  See
	# fastsincos() in audio/util.h.
	#
	# It is emitted here rather than written in the header because it is
	# a property of this table's spacing.  Hardcoding it beside the user
	# would leave a stale correction behind the first time step_shift
	# moved, and nothing would say so.
	#
	h = math.pi / (2 * steps)
	defect = h * h / 2

	with open(output_path, 'w') as f:
		f.write(f"#define QUARTER_SINE_STEP_SHIFT {step_shift}\n")
		f.write(f"#define QUARTER_SINE_DEFECT {defect:.8e}f\n")
		f.write("const float __not_in_flash(\"audio\") quarter_sin[] = {")

		for i in range(steps + 1):
			prefix = "\n\t" if i % 4 == 0 else " "
			val = math.sin(i * math.pi / steps / 2)
			f.write(f"{prefix}{val:+.8f}f,")

		f.write(f" {1.0:+.8f}f\n}};\n")

if __name__ == "__main__":
	generate_sine(sys.argv[1], int(sys.argv[2]))
