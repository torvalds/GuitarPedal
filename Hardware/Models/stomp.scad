$fn=100;
epsilon=0.01;

// Datasheet at https://www.taydaelectronics.com/datasheets/files/A-4807.pdf
//
// But it doesn't actually match what I've measured with the calipers, so
// this has some different measurements.

// The threaded area: a M12x0.75 nut will thread over it, and
// the "11.8mm" diameter is an approximate outer thread size
module threaded()
{
	translate([0,0,13.6])
		cylinder(d=11.8, h=15);
}

// Top of the switch itself
module topcap()
{
	translate([0,0,13.6 + 14.9 + 5.1])
		cylinder(d=8.8, h=5.5);
}

// The "neck" of the cylinder between the threaded and top
// Only 5.1mm mm is exposed, the rest is hidden inside the threaded
// area and the top cap
module neck()
{
	translate([0, 0, 13.6 + 15 + 2.5])
		cylinder(d=7.5, h=10, center=true);
}

module pins()
{
	translate([-3.1, -3, 0]) cube([3, 1, 10], true);
	translate([-3.1, +3, 0]) cube([3, 1, 10], true);
}

module stomp_switch()
{
	color("silver") {
		threaded();
		topcap();
		neck();
		pins();
	}


	width=13;
	height=13.6;
	endpos = 11.5+3.1;
	bodyend = endpos-3;
	midcylinder = 25.5-3.1-11.5-width/2;

	// Slight protrusion
	color("silver")
	translate([0,0,height])
		cube([12,12,0.8],true);

	// Another odd protrusion
	color("#404040") {
		translate([-11.5-3.1, -1.9, 5.6])
			cube([10, 3.8, 7]);

		// Cylinder
		translate([midcylinder, 0])
			cylinder(d=width, h=height-1);

		translate([-bodyend, -width/2])
			cube([bodyend+midcylinder, width, height-1]);
	}

	// "top plate"
	color("burlywood") {
		translate([-endpos,-width/2, height-1])
			cube([endpos+midcylinder, width, 1]);
		translate([midcylinder,0, height-1])
			cylinder(d=width, h=1);
	}
}

stomp_switch();
