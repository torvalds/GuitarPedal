$fn=40;

black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];

// The model is centered on the front
// as per the data sheet
//
// And when I say "as per the data sheet",
// I mean that I've found three different
// copies with slightly different values.
//
// Oh well.
pins = [ [11.3, -2.3], [3.2, 2.3], [6.2, 2.3], [10.2, 2.3] ];

feet = [ [ 1.6, 0], [ 8.6, 0] ];

module body()
{
    difference() {
	union() {
	    translate([0,-3])
		cube([12.1, 6.1, 5]);
	    translate([0,0,2.5]) rotate([0,-90,0])
		cylinder(2,d=5);
	}
	translate([8,0,2.5]) rotate([0,-90,0])
	    cylinder(12,d=3.6);
    }
}

module pj320a()
{
    color(black) {
	body();

	// Feet or locating pins..
	translate([0,0,-0.5]) for (pos = feet)
	    translate(pos) cylinder(1, d=0.75);
    }

    color(silver) {
	// Pins
	for (pos = pins)
	    translate(pos) translate([0,0,1]) cube([1,0.4,6], center=true);
    }
}

pj320a();
