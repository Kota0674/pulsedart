cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pulsedart_mouse)

# Stock-derived data (PixArt SROM, HyperX LED tables) is not in the repository.
foreach(blob pmw3389_srom.h led_tables.h)
	if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/${blob})
		message(FATAL_ERROR "src/${blob} is missing. Generate it from your own flash dump:\n"
			"  python tools/extract_blobs.py dump/flash.bin\n(see docs/flashing.md)")
	endif()
endforeach()

target_sources(app PRIVATE
	src/main.c
	src/sensor.c
	src/input.c
	src/leds.c
	src/power.c
	src/settings.c
	src/usb.c
	src/ble.c
	src/esb_link.c
	src/stockcfg.c
	src/actions.c
	src/leds_fx.c
	src/vendor.c
)
