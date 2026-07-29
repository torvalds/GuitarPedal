build:
	cmake --build build --target blink

usb-device:
	cmake -B build
	cmake --build build --target blink

flash:
	picotool load build/blink.elf && picotool reboot

prep:
	git submodule init
	git submodule update --init --recursive
	cmake -S . -B build

.PHONY: build
