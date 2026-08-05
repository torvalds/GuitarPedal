build:
	cmake --build build --target pedal

usb-device:
	cmake -B build
	cmake --build build --target pedal

flash:
	picotool load build/pedal.elf && picotool reboot

prep:
	git submodule init
	git submodule update --init --recursive
	cmake -S . -B build

.PHONY: build
