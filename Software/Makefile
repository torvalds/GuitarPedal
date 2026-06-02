build:
	cmake --build build --target blink

usb-device:
	cmake -B build -DUSB_MODE=DEVICE
	cmake --build build --target blink

usb-host:
	cmake -B build -DUSB_MODE=HOST
	cmake --build build --target blink

flash:
	picotool load build/blink.elf && picotool reboot

prep:
	git submodule init
	git submodule update --init --recursive
	cmake -DPICO_PLATFORM=rp2350 -S . -B build

.PHONY: build
