build: gensin.h
	cmake --build build --target blink

flash:
	picotool load build/blink.elf && picotool reboot

prep: gensin.h
	cmake -DPICO_PLATFORM=rp2350 -S . -B build

gensin.h: gensin
	./gensin > gensin.h

gensin: gensin.c
	$(CC) gensin.c -o gensin -lm

test_sincos: gensin.h test_sincos.c
	 $(CC) test_sincos.c -o test_sincos -lm

test: test_sincos
	./test_sincos

.PHONY: build test
