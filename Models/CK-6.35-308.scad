$fn=40;

// The model is centered on the front pin
// as per the data sheet
pins = [ [0, 0], [15, -2.5], [16, 2.5]];

feet = [ [ 0.3,3.9], [ 0.3,-3.9],
	 [11.9,3.9], [11.9,-3.9] ];

module body()
{
    translate([-6.5,-6.5])
	cube([14.5,13,11]);
    translate([8,-5.5,0.5])
	cube([10,11,10.5]);
    translate([-7.5,0,5.5]) rotate([0,90,0])
	cylinder(1, d=10.9);
    translate([-7.5,0,5.5]) rotate([0,-90,0])
	cylinder(9, d=9);
}

module hole()
{
    translate([4,0,5.5]) rotate([0,-90,0])
	cylinder(21, d=6.35);
}

difference() {
    body();
    hole();
}

// Feet or locating pins..
translate([0,0,-1]) for (pos = feet)
	translate(pos) cylinder(2, d=2);

// Pins
for (pos = pins)
	translate(pos) translate([0,0,-2]) cube([2,0.8,6], center=true);
