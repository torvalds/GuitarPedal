$fn=40;

black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];

module pin(pos, size)
{
    translate(pos) color(silver) cube(concat(size,8), center=true);
}

// The model is centered on the back pin
pin([  0,  0],   [2.8, 0.6]);
pin([4.8, -3],   [0.6, 2.4]);
pin([  0, -5.8], [2.4, 0.6]);
	
module body()
{
    translate([-4.5, -13.6, 0])
	cube([9, 14.5, 3.2]);
    translate([-4.5, -13.6, 0])
	cube([9, 3, 10.7]);
    translate([-4.5, -13.6, 0])
	cube([9, 12, 6.5]);
    translate([0, -13.6, 6.5]) rotate([-90,0,0])
	cylinder(13.6, d=8.4);
}

module hole()
{
    translate([0, -14, 6.5]) rotate([-90,0,0])
	cylinder(7, d=5.5);
}

module housing()
{
    difference() {
	body();
	hole();
    }
}

color(black) housing();
translate([0, -13.6, 6.5]) rotate([-90,0,0])
	color(silver) cylinder(5.3, d=2.1);
