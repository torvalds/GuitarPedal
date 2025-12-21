// This is not really 1590LB
// The board is 45x50mm with 5mm radius corners:
//     -22.5..+22.5 , -22.5..27.5
// because reasons (those being that it started out
// as 45x45 and then got expanded 5mm down in the y direction)
//
// Two 9mm pots with 20mm diameter knobs:
//     -12,-12 and +12,-12
//
// Audio jacks at 14,5 (input) and -14,17 (output)
//
// Stomp switch at 12,20
//
// LED at 0,23.5
//
// DC Jack at 0,-12.5, extending to y=-26 (3.5mm up off the board)
//
// Zero in Z dimension is top of board
$fn=100;

DZ=5.25;
DZe=5.251;
DZee=5.252;

module board()
{
    translate([0,-2.5])
	offset(r=5)
	    square([35,40],center=true);
}

module outside()
{
    offset(r=3.5) board();
}

module body()
{
    translate([0,0,-DZ])
	linear_extrude(12+DZe)
	    outside();
    translate([0,0,12])
	minkowski() {
	    linear_extrude(1)
		board();
	    sphere(r=3.5);
	}
}

module potcutout()
{
    translate([0,-2,0]) linear_extrude(11)
	square([12,15],center=true);
    translate([0,0,9])
	linear_extrude(10)
	    circle(d=21);
    cylinder(d1=21,d2=21,h=9.1);
}

module dcjack_cutout()
{
    translate([-5,12,-DZe])
	cube([10,15,11+DZee]);
    translate([-7,11,-DZe])
    cube([14,10,12]);
}

module audio_cutout()
{
    translate([-20,-8]) cube([28.5,16,12]);
    translate([6,0,6]) rotate([0,90,0])cylinder(d=10,h=11);
    translate([7,0,0]) cube([12,10,12],center=true);
}

module stomp_cutout()
{
    translate([-6.5,-7.25]) cube([13,14.5, 15]);
    translate([0,0,14]) cylinder(d=12.5,h=20);
}

module led_cutout()
{
    cylinder(d1=8,d2=6,h=20);
}

module cutout()
{
    translate([0,0,-DZe])
	linear_extrude(DZee)
	    board();
    translate([0,12.5,-11])
	cube([10,25,22],center=true);
    translate([-12,12])
	potcutout();
    translate([12,12])
	potcutout();
    dcjack_cutout();
    translate([14,-5])
	audio_cutout();
    translate([-14,-17]) rotate([0,0,180])
	audio_cutout();
    translate([12,-20])
	stomp_cutout();
    translate([0,-23.5])
	led_cutout();
}

difference() {
    body();
    cutout();
}

translate([60,0,-DZ-2]) {
    linear_extrude(2)
	outside();
    linear_extrude(DZ+2-1.6)
	difference() {
	    board();
	    square([38,100],center=true);
	}
    translate([-5,22.5])
        cube([10,3.5,DZ+2]);
    translate([22.5,-5])
        difference() {
            translate([0,-5]) cube([3.5,10,12]);
            translate([-1,0,DZ+2+6]) rotate([0,90,0])cylinder(d=10,h=11);
        }
    translate([-22.5,-17])
        difference() {
            translate([-3.5,-5]) cube([3.5,10,12]);
            translate([-4.5,0,DZ+2+6]) rotate([0,90,0])cylinder(d=10,h=11);
        }
}
