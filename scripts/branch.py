#!/usr/bin/env python3
#
# Which branch this was built on, for anything that has to tell two
# pedals apart.
#
# Three of them on one machine look identical in lsusb, and the moment
# one is running a side branch is exactly when it matters which.  A
# build off main says nothing at all, so the ordinary case stays
# untouched and only the interesting one is labelled.
#
# Nothing here is a fact the firmware can check, so it is not a
# capability and does not go through the probe: it is a build stamp,
# next to __DATE__ and __TIME__.
#
# A tarball, a tree with no git, a detached HEAD and main itself all
# come out the same - no branch - because none of them has a name worth
# printing.
#
# Called as: branch.py <output.h> <tree>
#
import re
import subprocess
import sys
from pathlib import Path


def branch(tree):
	try:
		r = subprocess.run(
			["git", "-C", tree, "symbolic-ref", "--short", "-q", "HEAD"],
			capture_output=True, text=True, timeout=5)
	except (OSError, subprocess.SubprocessError):
		return ""

	name = r.stdout.strip()
	if r.returncode or name == "main":
		return ""

	# It ends up inside a C string literal and a USB descriptor, and a
	# branch name may contain neither.  Anything git allows and this
	# does not becomes an underscore rather than being dropped, so two
	# branches cannot collapse into one name.
	return re.sub(r"[^A-Za-z0-9._/+-]", "_", name)


if __name__ == "__main__":
	out, tree = sys.argv[1], sys.argv[2]
	name = branch(tree)

	Path(out).parent.mkdir(parents=True, exist_ok=True)
	with open(out, "w") as f:
		if name:
			f.write('#define PEDAL_BRANCH "%s"\n' % name)
		else:
			f.write("// built on main, or on nothing with a name\n")
