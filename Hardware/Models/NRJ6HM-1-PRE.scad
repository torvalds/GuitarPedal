$fn=40;

red = [0.8,0.2,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

// The model is centered on the barrel
//
// The pins are the footprint's, with y negated because OpenSCAD is y-up
// and KiCad is y-down.  They were measured with calipers originally and
// were out by 50um on the column pitch and 115um on the row spacing;
// these are the corrected numbers, taken from the part's own data via
// EasyEDA - 6.400 pitch, 11.200 rows.
pins = [ [-17.5,7.115],  [ -11.1,7.115 ], [-4.7,7.115],
	 [-17.5,-4.085], [-11.1,-4.085], [-4.7,-4.085 ] ];

module body()
{
    union() {
	translate([-22,-6.43,0])
	    cube([22,15.75,13]);
	translate([-25,0,6]) {
	    rotate([0,90,0]) {
		cylinder(3,d1=8,d2=9);
	    }
	}
    }
}

module barrel()
{
    translate([0,0,6.5]) rotate([0,90,0]) {
	difference() {
	    cylinder(8,d=9);
	    cylinder(9,d=6.35);
	}
    }
}

module nrj6hm()
{
    color(black) body();
    color(silver)
	barrel();
    // Pins
    color(silver) for (pos = pins)
	translate(pos) translate([0,0,-2]) cube([1,1,6], center=true);
}

nrj6hm();
