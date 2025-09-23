$fn=40;

red = [0.8,0.2,0.2];
green = [0.2,0.8,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

module body()
{
    translate([0,-0.5,6.5])
	cube([9.5, 11, 7], center=true);
}

module shaft()
{
    translate([0,0,10])
	cylinder(5, d=6.8);
    translate([0,0,15])
	cylinder(10, d=6.35);
}

module leg(w1, w2)
{
    translate([0,0,2.5])
	cube([w1, 0.3, 5], center=true);

    if (w2 != 0)
	translate([0,0,0])
	    cube([w2, 0.3, 7.5], center=true);
}

color(green) body();
color(silver) shaft();

// Legs and supports
color(silver) {
    translate([-2.54, -7.5]) leg(1.5, 0.8);
    translate([ 0,    -7.5]) leg(1.5, 0.8);
    translate([ 2.54, -7.5]) leg(1.5, 0.8);

    translate([-4.8, 0]) rotate(90) leg(3, 1);
    translate([ 4.8, 0]) rotate(90) leg(3, 1);

    translate([0,-4.55]) leg(3, 0);
    translate([0, 4.55]) leg(3, 0);
}
