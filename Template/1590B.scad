$fn=50;

pots = [ for (x = [-20,0,20]) for (y = [10,35]) [ x,y ] ];

holes = [
	for (pos = pots) [ 7.7, pos ],	// Pots
	[ 12.5, [ 0,-44] ],		// Stomp
	[ 6, [-14,-44] ],		// LED
];

//
// There are probably better ways to do this, but..
//
board_corners = [
    [ 21, 53 ], [ 21, 51], [ 25, 51], [ 25, 47], [ 27, 47 ],
    [ 27,-47 ], [ 25,-47], [ 25,-51], [ 21,-51], [ 21,-53 ],
    [-21,-53 ], [-21,-51], [-25,-51], [-25,-47], [-27,-47 ],
    [-27, 47 ], [-25, 47], [-25, 51], [-21, 51], [-21, 53 ]];

board_arc_cutots = [[25,51],[25,-51],[-25,-51],[-25,51]];

module board_outline()
{
	difference() {
		polygon(board_corners);
		for (pos = board_arc_cutots)
			translate(pos) circle(r=4);
	}
}

module tabs()
{
	translate([0,55]) square(10, center=true);
	translate([0,-55]) square(10, center=true);
	translate([30,0]) square(10, center=true);
	translate([-30,0]) square(10, center=true);
}

module top_drill_guide()
{
    difference() {
	linear_extrude(4) {
		union() {
			board_outline();
			tabs();
		}
	}
	for (hole = holes)
		translate(hole[1])
			cylinder(10, d=hole[0], center=true);
    }
}

// This is tight on purpose, so that it self-centers
// There is a slight taper from the top to the bottom
// of the 1590B enclosure
module outside_outline()
{
	difference() {
		union() {
			square([65,100], center=true);
			square([40,115], center=true);
		};
		square([60,110],center=true);
	}
}

module enclosure_side_holes()
{
	translate([30,-26,10])
		rotate([0,90,0])
			cylinder(10,d=10,center=true);
	translate([-30,-26,10])
		rotate([0,90,0])
			cylinder(10,d=10,center=true);
	translate([0,55,10])
		cube([9.5, 12.5, 12.5], center=true);
}

module drill_guide()
{
	difference() {
		linear_extrude(26.5+4)
			outside_outline();
		enclosure_side_holes();
	}
	translate([0,0,26.5]) {
		top_drill_guide();
	}
}

drill_guide();
