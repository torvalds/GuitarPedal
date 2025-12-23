flash:
	cmake --build build --target blink
	picotool load build/blink.elf && picotool reboot

prep:
	cmake -DPICO_PLATFORM=rp2350 -S . -B build
