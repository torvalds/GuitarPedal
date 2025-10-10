$fn=40;

red = [0.8,0.2,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

// The model is centered
pins = [ [ -5.3, -4.8], [0, -4.8], [5.3, -4.8],
	 [ -5.3,  0],   [0,  0],   [5.3,  0],
	 [ -5.3,  4.8], [0,  4.8], [5.3,  4.8] ];

module base()
{
    color(red) translate([0,0,8])
	cube([19.5, 17, 16], center=true);
}

module body()
{
    difference() {
	translate([0,0,16])
	    cylinder(13, d=12);
	translate([-1,5,16-epsilon])
	    cube([2,1,14]);
    }
}

module switch()
{
    translate([0,0,28])
	cylinder(6, d=8);
    translate([0,0,33.5])
	cylinder(5.5, d=10);
}

module stomp()
{
    color(red) base();
    color(silver) union() { body(); switch(); }

    // Pins
    color(silver) for (pos = pins)
	translate(pos) translate([0,0,-2]) cube([2,1,6], center=true);
}

stomp();
