build: blink.c
	cmake --build build --target blink

flash:
	picotool load build/blink.elf && picotool reboot

prep:
	cmake -DPICO_PLATFORM=rp2350 -S . -B build
