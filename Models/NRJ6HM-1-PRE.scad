$fn=40;

red = [0.8,0.2,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

// The model is centered on the barrel
pins = [ [-17.45,7.23],  [ -11.1,7.23 ], [-4.75,7.23], 
	 [-17.45,-4.2], [-11.1,-4.2], [-4.75,-4.2 ] ];

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
    translate([0,0,6]) rotate([0,90,0]) {
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
