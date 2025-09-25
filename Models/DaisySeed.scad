$fn=40;

red = [0.8,0.2,0.2];
green = [0.2,0.8,0.2];
black = [0.2,0.2,0.2];
silver = [0.75,0.75,0.75];
epsilon = 0.001;

posx = [ -24.13 : 2.54 : 24.13 ];

module board()
{
    translate([0,0,12.8])
	cube([51, 18, 1.6], center=true);
}

module legs()
{
    for (x = posx) {
	translate([x,-7.62,6])
		cube([0.8,0.8,17], center=true);
	translate([x,7.62,6])
		cube([0.8,0.8,17], center=true);
    }
}

module standoffs()
{
    translate([0,-7.62,6])
	cube([51,1.27,11],center=true);
    translate([0,7.62,6])
	cube([51,1.27,11],center=true);
}

module usb_conn();
{
    translate([-24,0,14.4])
	cube([5.5, 8, 3], center=true);
}

module chips()
{
    translate([-14,0,16.4])
	cube([7,3,6], center=true);
    translate([-4,0,13.4])
	cube([10,9,2], center=true);
    translate([8,0,13.4])
	cube([8,12,2], center=true);
    translate([18,0,13.4])
	cube([7,5,2], center=true);
}

module daisy_seed()
{
    color(green) board();
    color(silver) legs();
    color(black) standoffs();
    color(silver) usb_conn();
    color(black) chips();
}

daisy_seed();
