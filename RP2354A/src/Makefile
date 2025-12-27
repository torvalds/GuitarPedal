build:
	cmake --build build --target blink

.PHONY: build

flash:
	picotool load build/blink.elf && picotool reboot

prep:
	cmake -DPICO_PLATFORM=rp2350 -S . -B build
