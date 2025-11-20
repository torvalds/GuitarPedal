$fn=40;

red = [0.8,0.2,0.2];
green = [0.2,0.8,0.2];
brown = [0.8,0.5,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

// FreeCad doesn't like the prettier model
// that rounds things with that minkowski
module body()
{
    translate([0,0,2])
//	minkowski() {
//	    cylinder(h=7, d=15);
//	    sphere(d=2);
//	}
	cylinder(h=9, d=17);
}

module shaft()
{
    translate([0,0,10])
	cylinder(5, d=6.8);
    translate([0,0,15])
	cylinder(10, d=6.35);
}

module pcb()
{
    translate([0, -6, 8.5])
	cube([15,12,1.2],center=true);
}

module leg(pos)
{
    translate([pos, -16.5, 0]) {
	cube([1, 0.2, 10], center=true);
	translate([0, 0, 4]) cube([3.6, 0.2, 8], center=true);
	translate([0, 6, 8]) cube([3.6, 12, 0.2], center=true);
    }
}

module pot_basic()
{
    color(silver) body();
    color(silver) shaft();
    color(brown) pcb();
    color(silver) {
	leg(-5); leg(0); leg(5);
    }
}

pot_basic();
