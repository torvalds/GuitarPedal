$fn=50;
use <../Models/WR-DC-2.1.scad>
use <../Models/Pot-9mm.scad>
use <../Models/NRJ6HM-1-PRE.scad>
use <../Models/PBS-24-102P.scad>
use <../Models/DaisySeed.scad>

red = [0.8,0.2,0.2];
green = [0.3,0.7,0.3];

potx = [-20,0,20];
poty = [15,40];

pots = [ for (x = potx) for (y = poty) [ x,y ] ];

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
	translate([30,-26]) {
		cube([9.5,9.5,20],center=true);
		translate([0,0,10.5]) rotate([0,90,0])
			cylinder(10,d=9.5,center=true);
	}
	translate([-30,-26]) {
		cube([9.5,9.5,20],center=true);
		translate([0,0,10.5]) rotate([0,90,0])
			cylinder(10,d=9.5,center=true);
	}
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

module pot_board_outline()
{
	translate([0,27])
		minkowski() {
			square([50,32],center=true);
			circle(d=5);
		}
}

module pot_board()
{
	color(green) linear_extrude(1.6)
		pot_board_outline();

	// The "Low Profile" version sits effectively
	// inside the board
	translate([0,0,-1.4]) {
		for (x = potx) {
			translate([x,40]) pot();
			translate([x,15]) rotate([0,0,180]) pot();
		}
	}

	// Fake some "legs" for the board for now
	for (pos = [[-20,20], [20,20], [-20,30], [20,30]])
		translate(concat(pos,-13))
			cylinder(14,d=4);
}

module led()
{
	cylinder(23, d=5);
	color(red) translate([0,0,23]) cylinder(5, d=5);
}

module board()
{
	// PCB
	color(green) translate([0,0,-1.8]) linear_extrude(1.6)
		board_outline();

	// DC Jack
	translate([0,43]) rotate([0,0,180])
		dc_jack();

	// Potentiometer board
	translate([0,0,12])
		pot_board();

	// Audio Jacks
	translate([27,-26])
		nrj6hm();
	translate([-27,-26]) rotate([0,0,180])
		nrj6hm();

	// Stomp switch
	translate([0,-44])
		pbs24();

	// LED
	translate([-14,-44,-1])
		led();

	// Daisy Seed
	translate([0,-2,0])
		daisy_seed();
}

// Zero this to see them nested
apart=40;

translate([-apart, 0, 0]) union() { drill_guide(); }

translate([apart,0,4]) union() { board(); }
