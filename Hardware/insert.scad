$fn=100;
epsilon=0.01;

screen_pos=[-8,30];
rotary_pos=[[19,38],[19,13]];
stomp_pos=[
	[[-16,-40],[0,0,-40]],
	[[16,-40],[0,0,40]]
];

module rotary_cutout()
{
	cube([12.5, 16, 30], true);
}

module stomp_cutout(pos)
{
	translate(pos[0]) rotate(pos[1]) {
		cylinder(d=16,h=20,center=true);
		translate([0,9]) cube([13,26,6],true);
		translate([0,-4]) cylinder(d=13,h=6,center=true);
	}
}

module stomp_led_cutout()
{
	cylinder(d=12.5,h=50);
	translate([0,15,0])
		cylinder(d=5.5,h=50);
}

corners=[[-27,-55],[-27,55],[27,-55],[27,55]];

module holder_outline()
{
	difference() {
		square([58,114],true);

		// Scrow posts for corners
		for (i=corners) translate(i) circle(r=6);

		// stomp switch connector
		translate([0,-15])
			circle(d=20);

		// stomp switches
		for (i=stomp_pos) translate(i[0])
			translate([0,15]) circle(d=8);
	}
}

module screen_cutouts()
{
	cylinder(d=20,h=30,center=true);
	translate([0,21.6]) cube([12, 4, 30], true);
}

module holder()
{
	difference() {
		linear_extrude(6.5) holder_outline();
		translate(screen_pos) translate([0,0,6.5]) {
			cube([34.2, 47.2,2.9*2], true);
			cube([30, 34, 4+2.9*2], true);
			screen_cutouts();
		}
		for (i=rotary_pos) translate(i)
			rotary_cutout();
		for (i=stomp_pos)
			stomp_cutout(i);
	}
}

holder();
