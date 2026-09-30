import copy
from math import *
from build123d import *
from ocp_viewer import *

# The board is 1.6mm thick, and the extra extrusion below
# it is the "random SMT components"
# And then we center it along the top of the board
board = extrude(RectangleRounded(48, 35, radius=3), -1.6)
board += extrude(RectangleRounded(45, 32, radius=3), -4);
board = Pos(0,-35/2,0)*board

# The USB connector is a mid-mount one, so it is unusually
# located: it's neither on top of the board nor below it.
# Instead the top of the connector is flush with the board
# top, which is our Z=0. So the Z height of the connector
# is the mid-point of the 3.5mm connector itself
usb_z = -3.5/2

usb = RectangleRounded(9, 3.5, radius=1.6)
usb = Pos(0,-35,usb_z)*Rot(90,0,0)*extrude(usb, -10)

board += usb

audio_jack = Cylinder(5,17)
audio_jack += Pos(-1.45,0,-11)*Box(16, 13, 22)
audio_jack += Pos(0,0,-22)*Cone(4,6,6)
audio_jack = Pos(0,0,6.5)*Rot(-90,0,0)*audio_jack

board += Pos(17,0,0)*audio_jack
board += Pos(-14,0,0)*audio_jack

LED = Box(5,3,3)
board += Pos(-10, -33, -2.6) * LED
board += Pos( 10, -33, -2.6) * LED

headphone_jack = Cylinder(2.5,4)
headphone_jack += Pos(0,0,-6)*Box(6,6,12)
headphone_jack = Pos(0,0,2.5)*Rot(-90,0,0)*headphone_jack

board += Pos(18,-35,-1.6)*Rot(180,0,0)*headphone_jack

#
# The two pieces of the enclosure: front/top and back/bottom
#
front = Pos(0,-35/2, 6.5)*fillet(Box(62,43,22).edges(), radius=3)
front = split(front, Pos(0,0,-5)*Rot(-21,0,0)*Plane.XY)

back_box = fillet(Box(58,38,22).edges(), radius=2)
back = split(Pos(0,-35/2-1, 2)*back_box,
             Pos(0,0,0)*Rot(-21,0,0)*Plane.XY, keep=Keep.BOTTOM)

# The socket in the front that the back sits in.
#
# Cut with a grown copy of the back rather than with the back itself,
# because subtracting the back leaves no clearance at all: the skirt's
# inner face is then exactly the back's outer face, the print turns that
# into an interference fit over the whole perimeter, and the two pieces
# cannot be parted again without breaking.
#
# Growing the box by 2*fit and its rounding by fit is what an outward
# offset of a filleted box is - the rounding stays tangent to both faces.
# offset() itself fails here ("an alternative kind may resolve this
# error") because it is being asked to offset the solid *after* the
# tilted cut, where that face meets the roundings. scale() is not the
# same thing: it would grow x, y and z by different amounts and move the
# geometry as well.
#
# Separating along z is what makes the vertical roundings free: they are
# a prism in that direction, so they key nothing. Lifting the front off
# is clear at every step; pulling it forward in y fouls immediately.
#
fit = 0.15

socket = fillet(Box(58 + 2*fit, 38 + 2*fit, 22 + 2*fit).edges(), radius=2 + fit)
socket = split(Pos(0,-35/2-1, 2)*socket,
               Pos(0,0,0)*Rot(-21,0,0)*Plane.XY, keep=Keep.BOTTOM)

front -= socket

#
# NOTE! Now we move the front/back to "board space", which is
# offset by 1.5mm in X. That way the audio jacks are centered
# on the front piece, and we can do all the rest using board
# coordinates.
#
front = Pos(1.5,0,0)*front
back = Pos(1.5,0,0)*back

# The 'board' is for testing fit and visualizing. This is the
# actual cutout used for the board.
#
# Note the slop: 0.5mm on each side for every component, so
# everything is 1mm bigger in all dimensions

board_cutout = Pos(0,-35/2)*Rectangle(49, 36)
cutout = Pos(0,0,-2) * extrude(board_cutout, 15)
cutout += Pos(0,-35/2)*extrude(Rectangle(46, 33), -5);

# Audio jack cutout. Same position, just bigger parts.
# except the cone is subsumed by just a longer body
audio_cutout = Cylinder(5.5,17) + Pos(-1.45,0,-13)*Box(17, 14, 26)
audio_cutout = Rot(-90,0,0) * audio_cutout

cutout += Pos(17, 0, 6.5) * audio_cutout
cutout += Pos(-14, 0, 6.5) * audio_cutout

headphone_cutout = Cylinder(3,10) + Pos(0,0,-12)*Box(7,6,24)
cutout += Pos(18, -35, -1.6-2.5) * Rot(90,0,0) * headphone_cutout

bottom_cutout = cutout + extrude(board_cutout, 13)
top_cutout = cutout + extrude(board_cutout, 1)

front -= top_cutout
back -= bottom_cutout

usb_cutout = RectangleRounded(12, 7, radius=3)
usb_cutout = Pos(0,-35,usb_z)*Rot(90,0,0)*extrude(usb_cutout, 10)

back -= usb_cutout

# The LEDs get the same 0.5mm slop as every other part
LED_cutout = Box(6,4,4)
back -= Pos(-10, -33, -2.6) * LED_cutout
back -= Pos( 10, -33, -2.6) * LED_cutout

#
# Now add the screw block between the audio jacks that connects
# the two. The back's half stops 0.5mm above the board: it prints
# on supports, and what they leave on its underside would otherwise
# bear on the board.
#
front += Pos(0,-11,7)*Box(15,30,14)
back += Pos(0,-11-15-5,5.5)*Box(15,9,10)

#
# And the M3 screwhole that connects them
#
screw = CounterBoreHole(radius=1.5, depth=22, counter_bore_radius=3.0, counter_bore_depth=4.0)
screw = Pos(0,-35, 6.5) * Rot(90,0,0) * screw
front -= screw
back -= screw

#
# This is what cuts the USB connector's notch through the ledge the
# board sits on. It has zero clearance on purpose: the connector is
# held by it.
#
back -= board

#
# The LED windows are a separate clear piece: a 2mm plug in the outer
# end of each LED hole, flush with the outside of the rear wall. 2mm is
# what the slicer turns into four extrusion lines. The hole is the same
# diameter, so the window and the back share a surface rather than
# overlap.
#
LED_hole = Rot(90,0,0)*Cylinder(2,10)
back_wall = back.bounding_box().min.Y
LED_pos = (Pos(-10, back_wall, -2.6), Pos(10, back_wall, -2.6))
LED_holes = LED_pos * LED_hole

LED_window = Rot(90,0,0)*Cylinder(2,2,align=(Align.CENTER, Align.CENTER, Align.MAX))
windows = Compound(LED_pos*LED_window)
back -= LED_holes

#
# White arrows on the lid over the audio jacks, pointing into the case
# at the input and out of it at the output. The same trick as the
# windows: a separate piece that fills its own pocket. It is sunk 2mm
# into the lid, which is what makes it thick enough to print, and
# stands 0.6mm proud, which is what makes it read as an arrow rather
# than as a decal.
#
def arrow(length=8, head=3, shaft=1.6, wide=4):
    l, s, w = length/2, shaft/2, wide/2
    return make_face(Polyline((-s,-l), (s,-l), (s,l-head), (w,l-head),
                              (0,l), (-w,l-head), (-s,l-head), close=True))

lid_top = front.bounding_box().max.Z
arrow_deep, arrow_proud = 2, 0.6

one_arrow = extrude(arrow(), arrow_deep + arrow_proud)
arrows = Pos(17, -4, lid_top - arrow_deep) * Rot(0,0,180) * one_arrow
arrows += Pos(-14, -4, lid_top - arrow_deep) * one_arrow

front -= arrows

#
# Name every piece: the slicer shows the names, and they are what you
# pick filament by. The board is only for the viewer and is not in
# the STEP.
#
# Each piece is a copy because a build123d Compound takes its children
# away from any Compound they were in before.
#
def piece(shape, name, colour):
    shape = copy.copy(shape)
    shape.label, shape.color = name, colour
    return shape

def case():
    return Compound(label="compact", children=[
        Compound(label="front", children=[
            piece(front, "front", Color("DarkRed", 0.5)),
            piece(arrows, "arrows", Color("White"))]),
        Compound(label="back", children=[
            piece(back, "back", Color("Grey", 0.5)),
            piece(windows, "windows", Color("LightBlue"))])])

show(Compound(children=[case(), piece(board, "board", Color("Green"))]))

export_step(case(), "compact.step")
