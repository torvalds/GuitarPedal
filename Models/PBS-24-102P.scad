$fn=40;

red = [0.8,0.2,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

// The model is centered
pins = [ [ -7.1, -4.65], [0, -4.65], [7.1, -4.65] ];

module base()
{
    color(red) translate([0,0,9])
	cube([17.5, 14.3, 18], center=true);
}

module body()
{
    difference() {
	translate([0,0,18])
	    cylinder(12, d=12);
	translate([-6,-1,18])
	    cube([1,2,12+epsilon]);
    }
}

module switch()
{
    translate([0,0,30])
	cylinder(6, d=8);
    translate([0,0,35.5])
	cylinder(5.8, d=10);
}

module pbs24()
{
    color(red) base();
    color(silver) union() { body(); switch(); }

    // Pins
    color(silver) for (pos = pins)
	translate(pos) translate([0,0,-2]) cube([1,1,6], center=true);
}

pbs24();
