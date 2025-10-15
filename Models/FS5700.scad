$fn=40;

red = [0.8,0.2,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

// The model is centered
pins = [ [ -4, -2.5], [0, -2.5], [4, -2.5],
         [ -4,  2.5], [0,  2.5], [4,  2.5] ];

module base()
{
    color(red) translate([0,0,6.5])
	cube([13.4, 12, 13], center=true);
}

module body()
{
    difference() {
	translate([0,0,13])
	    cylinder(12, d=12);
	translate([-6,-1,13])
	    cube([1,2,12+epsilon]);
    }
}

module switch()
{
    translate([0,0,25])
	cylinder(3, d=8);
    translate([0,0,27.5])
	cylinder(5.8, d=10);
}

module fs5700()
{
    color(red) base();
    color(silver) body();
    color(silver) switch();

    // Pins
    color(silver) for (pos = pins)
	translate(pos) translate([0,0,-2]) cube([1,1,6], center=true);
}

fs5700();
