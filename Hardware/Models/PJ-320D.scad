$fn=40;

black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];

// The model is centered on the front
// as per the data sheet, but with the
// cylinder sticking out 2mm, so 0,0
// is where the cube body starts
// (datasheet numbers are relative to
// the 2mm tip of the cylinder)

pins = [ [11.6,-3], [3.2, 3], [6.2, 3], [10.2, 3] ];

module body()
{
	difference() {
		union() {
			translate([0,-3,0]) cube([12.1, 6, 5]);
			translate([0,0,2.5]) rotate([0,-90,0]) cylinder(h=4,d=5, center=true);
			translate([1.7,0,0]) cylinder(d=0.8,h=2,center=true);
			translate([8.7,0,0]) cylinder(d=0.8,h=2,center=true);
		}
		translate([8,0,2.5]) rotate([0,-90,0])
			cylinder(12,d=3.6);
	}
}

module pj320d()
{
	color(black)
		body();

	// Pads
	color(silver) for (pos = pins)
		translate(pos) translate([0,0,0.2]) cube([1,2,0.5], center=true);
}

pj320d();
