#!/usr/bin/env python3
#
# The Daier PBS-24B-2 stomp switch, translated from stomp.scad.
#
# Nobody sells a 3D model for this part - it is not in JLCPCB's library,
# EasyEDA has nothing, and KiCad's generator has no data for it - so this
# is the only one there is.  It is a rough solid, not a pretty one, and
# it exists so the switch stops being a hole in the 3D view.
#
# Datasheet: https://www.taydaelectronics.com/datasheets/files/A-4807.pdf
# The measurements below are Linus's calipers rather than that sheet,
# which does not match the part.
#
# The origin is the axis of the threaded barrel, in x and y both.  That
# is deliberate: a stomp switch faces the *top*, so it passes through the
# lid and both axes are constrained by the hole - see ISSUES 305.  z=0 is
# the underside of the body, so the pins hang below into the board.
#
# **Kept as three coloured parts rather than one fused solid.**  STEP
# carries colour per shape, and export_step writes it - but only if there
# is more than one shape to colour.  Fusing everything first, which the
# first version did, produces a valid model that KiCad draws in its
# default grey.  The grouping is the .scad's own: metal, body, plate.
#
from build123d import (Align, Axis, Box, Color, Compound, Cylinder, Plane,
                       Polyline, Pos, chamfer, export_step, fillet,
                       make_face, revolve)

BOTTOM = (Align.CENTER, Align.CENTER, Align.MIN)   # sit on z, centred in x/y

WIDTH = 13.0          # body across
HEIGHT = 13.6         # body up to the top plate
ENDPOS = 11.5 + 3.1
BODYEND = ENDPOS - 3
MIDCYL = 25.5 - 3.1 - 11.5 - WIDTH / 2            # +4.4, the body cylinder's axis
# The moulding has a generous radius - measured off a photo against the
# 13mm body width, nearer 1.2 than the 0.5 first guessed.  The turned
# metal is a different process and keeps its small break.
BODY_RADIUS = 1.2
LUG_RADIUS = 0.5                                   # too small a feature for 1.2
RADIUS = 0.5                                       # the turned metal edges
CHAMFER = 0.5                                      # the break on the barrel end

# The phenolic washer.  "burlywood" was close but too pale and too
# yellow; this is darker and redder, which is what the part looks
# like.  Roughly #8A4A33 - change the three numbers, not a name.
PHENOLIC = (0.54, 0.29, 0.20)

PITCH = 0.75          # M12x0.75, so 0.75mm per turn
DEPTH = 0.46          # external thread depth for that pitch
KEYWAY = (2.0, 1.0)   # the index slot: 2mm wide, 1mm deep
BORE = 8.0            # the plunger runs in this, neck is 7.5

#
# The thread is drawn as concentric grooves, not as a helix.
#
# A real helical sweep needs bd_warehouse (build123d dropped its thread
# classes at 0.11) and costs geometry proportional to the turn count -
# and this is 0.75mm pitch over 15mm, so twenty turns of it.  Rings cost
# sixty faces and are indistinguishable at any zoom anyone will use on a
# board render: the difference only shows looking straight down the axis.
#
_barrel = Pos(0, 0, HEIGHT) * Cylinder(11.8 / 2, 15, align=BOTTOM)
_groove = Plane.XZ * make_face(
    Polyline((6.1, 0), (11.8 / 2 - DEPTH, PITCH / 2), (6.1, PITCH), close=True).wire()
)
# stop short of the top so the barrel's rim survives to be chamfered below
for _i in range(19):
    _barrel -= Pos(0, 0, HEIGHT + _i * PITCH) * revolve(_groove, Axis.Z)

#
# The index slot, which stops the switch turning in its panel hole.  It
# runs the whole threaded length and cuts straight through the threads.
# On -x, the end the lug is on: confirmed against the part, not inferred
# from the photographs, which do not show it.
#
_kw, _kd = KEYWAY
_barrel -= Pos(-(11.8 / 2 - _kd / 2 + 0.6), 0, HEIGHT + 7.5) * Box(_kd + 1.2, _kw, 15)

#
# The barrel is a tube, not a rod: the neck slides inside it, and the
# gap around it is visible looking down at the switch.  Blind rather
# than through, because the bottom of the bore is never in view and a
# through hole would open into the flange.
#
_barrel -= Pos(0, 0, HEIGHT + 1.4 + 15 / 2) * Cylinder(BORE / 2, 15)

# --- the threaded barrel a M12x0.75 nut goes over, what is above it,
#     the pins below, and the thin flange where barrel meets plate
metal = (
    _barrel
    + Pos(0, 0, HEIGHT + 14.9 + 5.1) * Cylinder(8.8 / 2, 5.5, align=BOTTOM)
    # the neck is centred on its own height in the .scad, not based
    + Pos(0, 0, HEIGHT + 15 + 2.5) * Cylinder(7.5 / 2, 10)
    + Pos(-3.1, -3, 0) * Box(3, 1, 10)
    + Pos(-3.1, +3, 0) * Box(3, 1, 10)
    + Pos(0, 0, HEIGHT) * Box(12, 12, 0.8)
)

# --- the moulded body: a cylinder at one end, a box running back from
#     it, and the lug sticking out of the far end
body = (
    Pos(MIDCYL, 0, 0) * Cylinder(WIDTH / 2, HEIGHT - 1, align=BOTTOM)
    + Pos((-BODYEND + MIDCYL) / 2, 0, (HEIGHT - 1) / 2)
      * Box(BODYEND + MIDCYL, WIDTH, HEIGHT - 1)
    + Pos(-14.6 + 10 / 2, 0, 5.6 + 7 / 2) * Box(10, 3.8, 7)
)

#
# Rounded where it was moulded, selected by geometry so the choice
# survives a dimension changing.  At z=0 the body outline is four long
# edges; at the back face the two full-height ones are the outer corners
# and the shorter pair is where the lug meets it, an inside corner that
# stays sharp.  The lug's own exposed boundary is rounded too, except its
# top edges, which are covered by the plate.
#
_bottom = [e for e in body.edges().filter_by_position(Axis.Z, -0.001, 0.001)
           if e.length > 5]
_back = [e for e in body.edges().filter_by(Axis.Z)
                                .filter_by_position(Axis.X, -11.7, -11.5)
         if e.length > 10]
_lug = [e for e in body.edges()
        if e.center().X <= -11.6 and abs(e.center().Y) <= 2.0
        and e.center().Z <= 12.5]
body = fillet(_bottom + _back, BODY_RADIUS)
body = fillet([e for e in body.edges()
               if e.center().X <= -11.6 and abs(e.center().Y) <= 2.0
               and e.center().Z <= 12.5], LUG_RADIUS)

# --- the thin phenolic plate between body and flange
plate = (
    Pos((-ENDPOS + MIDCYL) / 2, 0, HEIGHT - 0.5) * Box(ENDPOS + MIDCYL, WIDTH, 1)
    + Pos(MIDCYL, 0, HEIGHT - 1) * Cylinder(WIDTH / 2, 1, align=BOTTOM)
)

#
# The turned metal parts have broken edges too: both ends of the top cap
# and the top of the threaded barrel where the neck comes through.  Named
# by the height they sit at, which is how the .scad places them.
#
_cap = [e for e in metal.edges()
        if (abs(e.center().Z - (HEIGHT + 14.9 + 5.1 + 5.5)) < 0.01 and e.length > 20)
        or (abs(e.center().Z - (HEIGHT + 14.9 + 5.1)) < 0.01 and e.length > 26)]
metal = fillet(_cap, RADIUS)

# The end of a threaded barrel is turned off square, not rounded - it is
# there so a nut starts cleanly - so this one is a chamfer.
_end = [e for e in metal.edges()
        if abs(e.center().Z - (HEIGHT + 15)) < 0.01 and e.length > 30]
metal = chamfer(_end, CHAMFER)

metal.color, metal.label = Color("silver"), "metal"
body.color, body.label = Color(0.25, 0.25, 0.25), "body"
plate.color, plate.label = Color(*PHENOLIC), "plate"

part = Compound(children=[metal, body, plate])
part.label = "PBS-24B-2"

if __name__ == "__main__":
    # Compound.volume under-reports here - it drops a child that has been
    # replaced by a fillet - so sum the children, which agrees with the
    # volume of the solids read back out of the exported STEP.
    print(f"  {len(part.children)} parts, {len(part.solids())} solids, "
          f"{len(part.faces())} faces, "
          f"volume {sum(c.volume for c in part.children):.1f} mm^3")
    for c in part.children:
        print(f"    {c.label:<6} {len(c.faces()):>3} faces  {c.volume:8.1f} mm^3")
    bb = part.bounding_box()
    for ax in "XYZ":
        lo, hi = getattr(bb.min, ax), getattr(bb.max, ax)
        print(f"  {ax.lower()}: {lo:+8.3f}..{hi:+8.3f}   span {hi - lo:7.3f}")
    export_step(part, "../symbols/PBS-24B-2.step")
    print("  wrote ../symbols/PBS-24B-2.step")
