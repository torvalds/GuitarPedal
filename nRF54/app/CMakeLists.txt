cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radio)

target_sources(app PRIVATE src/main.c src/pins.c src/audio.c)

if(CONFIG_BT)
	target_sources(app PRIVATE src/midi.c src/scan.c src/bind.c src/sink.c
		src/bcast.c src/unicast.c src/source.c
		src/phones.c)
endif()
