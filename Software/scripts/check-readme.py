#!/usr/bin/env python3
#
# Does the README still know which effects exist?
#
# The README's effect list is hand-written on purpose - it is an overview
# and wants prose, not a generated table - and hand-written is exactly
# why it drifted from eight entries to eight-of-seventeen without anyone
# noticing.  Nothing else in the tree has that problem, because
# gen_effects.py generates it.  This is the cheapest thing that would
# have caught it: the effect headers say which effects there are, the
# README bolds the name of each one it talks about, and those two sets
# should match.
#
# WHAT IT DOES NOT DO, deliberately.  It does not check that the prose is
# right, or current, or that a pot mentioned in it still exists.  An
# effect whose controls change completely still passes as long as its
# name is there.  Catching that would mean either generating the section,
# which is the thing we decided not to do, or an acknowledgement stamp
# somebody has to update, which is a second thing to forget.  Adding and
# removing effects is the case that actually went wrong, and it is the
# case this catches.
#
# It warns rather than fails.  The README is an overview and is allowed
# to lag slightly - a half-finished effect on a branch should not stop
# the firmware building.  If these warnings turn out to get ignored, the
# fix is to make the exit status 1 below and find out how annoying that
# is.
#
# Called as: check-readme.py <effects-dir> <readme>
#
import re
import sys
from pathlib import Path


def effects_in(effects_dir):
    """Every effect the firmware has, by display name.

    The name here is the one before the generator makes copies of it -
    tone.h is 'Tone', and the pedal ends up with 'Tone 1' and 'Tone 2' -
    because the README describes the effect and not each instance.
    """
    names = set()
    for path in sorted(Path(effects_dir).glob("*.h")):
        m = re.search(r"^// NAME:\s*(.+?)\s*\[(\w+)\]\s*$",
                      path.read_text(), re.M)
        if m:
            names.add(m.group(1))
    return names


def named_in(readme):
    """Every **bolded name** under the effects heading.

    Scoped to that section so that a mention of the tuner or the boost
    somewhere further up does not count as having described it.
    """
    text = Path(readme).read_text()
    m = re.search(r"^## Audio effects$(.*)", text, re.M | re.S)
    if not m:
        return None
    return set(re.findall(r"\*\*([^*]+)\*\*", m.group(1)))


def dead_links(readme):
    """Relative links in the effect list that point at nothing.

    An effect gets a link once it has a measured page under
    Documentation/effects, and a link on the front page of the
    repository that 404s is worse than no link at all.  Only relative
    ones: an http link is somebody else's to keep alive.
    """
    text = Path(readme).read_text()
    m = re.search(r"^## Audio effects$(.*)", text, re.M | re.S)
    if not m:
        return []
    root = Path(readme).resolve().parent
    return [t for t in re.findall(r"\]\(([^)#:]+)\)", m.group(1))
            if not (root / t).exists()]


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check-readme.py <effects-dir> <readme>")

    have = effects_in(sys.argv[1])
    if not have:
        sys.exit("check-readme: no effects found - wrong directory?")

    described = named_in(sys.argv[2])
    if described is None:
        print("check-readme: WARNING - no '## Audio effects' section in the "
              "README, so nothing was checked")
        return 0

    missing = sorted(have - described)
    extra = sorted(described - have)
    dead = dead_links(sys.argv[2])

    for target in dead:
        print(f"check-readme: WARNING - the README links to {target}, "
              f"which does not exist")

    if not missing and not extra:
        #
        # "headers" rather than "effects" because the two counts differ
        # and both are right: tone.h is one header and two effects.  The
        # README says seventeen, meaning routable ones.  Saying "effects"
        # here would look like one of them was wrong.
        #
        print(f"check-readme: {len(have)} effect headers, "
              f"all named in the README")
        return 0

    print("check-readme: WARNING - the README's effect list has drifted")
    for name in missing:
        print(f"    {name}: in the firmware, not in the README")
    for name in extra:
        print(f"    {name}: in the README, not in the firmware")
    print("    Software/effects/*.h is the source of truth; the README "
          "wants a line about each")
    return 0


if __name__ == "__main__":
    sys.exit(main())
