#include <math.h>
#include <stdio.h>

#define SAMPLES_PER_SEC 52083.33333f

typedef unsigned int uint;
#include "util.h"

int main(int argc, char **argv)
{
	double error = 0;

	for (float f = 0; f < 1.0; f+=0.01) {
		struct sincos my = fastsincos(f);
		double esin = fabs(my.sin - sin(2*M_PI*f));
		double ecos = fabs(my.cos - cos(2*M_PI*f));
		error = fmax(esin,error);
		error = fmax(ecos,error);
	}
	printf("Max error %.8f (%.1f digits)\n", error, -log10(error));
}
