build:
	cmake --build build --target blink

.PHONY: build

flash:
	picotool load build/blink.elf && picotool reboot

prep: gensin.h
	cmake -DPICO_PLATFORM=rp2350 -S . -B build

gensin.h: gensin
	./gensin > gensin.h

gensin: gensin.c
	$(CC) gensin.c -o gensin -lm
