#include <math.h>
#include <stdio.h>

#define STEP_SHIFT 8
#define STEPS (1 << STEP_SHIFT)

int main(int argc, char **argv)
{
	printf("#define QUARTER_SINE_STEP_SHIFT %d\n", STEP_SHIFT);
	printf("const float quarter_sin[] = {");

	for (int i = 0; i < STEPS+1; i++) {
		printf("%s%+.8ff,",
			!(i & 3) ? "\n\t" : " ",
			sin(i*M_PI/STEPS/2));
	}

	printf(" %+.8ff\n};\n", 1.0);
}
