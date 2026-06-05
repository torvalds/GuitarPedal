$fn=40;

black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];

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
}

module barrel()
{
    difference() {
	cylinder(9, d=9);
	translate([0,0,-1])
	    cylinder(11, d=6.35);
    }

}

module jack()
{
    translate([-6.5,0,5.5]) rotate([0,-90,0]) {
	cylinder(1, d=10.9);
	barrel();
    }
}

color(black) body();
color(silver) jack();

// Feet or locating pins..
color(black) translate([0,0,-1]) for (pos = feet)
	translate(pos) cylinder(2, d=2);

// Pins
color(silver) for (pos = pins)
	translate(pos) translate([0,0,-2]) cube([2,0.8,6], center=true);
