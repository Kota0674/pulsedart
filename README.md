# pulsedart

Custom firmware for my HyperX Pulsefire Dart (nRF52840). Work in progress.

Step 1: get SWD working from a Raspberry Pi. The mouse runs at 2.1 V, so no level shifter:
SWCLK through a 1k/2k divider, SWDIO open-drain through 150 R (`pi/pulsedart_nolvl.cfg`).
