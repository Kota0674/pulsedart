#!/usr/bin/env bash
# Update the firmware over USB (MCUboot variant only; no SWD needed).
# The mouse must be connected to this machine with a USB cable.
#   usage: ./dfu_update.sh zephyr.signed.bin
#          (fw/mouse/build-mcuboot/mouse/zephyr/zephyr.signed.bin, signed with the project key)
# Also works from MCUboot recovery (hold Left+Right+Back while switching on, about 3 s, LEDs blue,
# USB 2FE3:0102, 2FE3:FFFF once in DFU mode).
# Flow:
#   1. dfu-util detaches the running firmware (DFU run-time interface, 0951:16E2).
#   2. The mouse re-enumerates as "Pulsedart DFU" (2FE3:0101), and the image goes to slot 1.
#   3. The mouse reboots; MCUboot checks the signature and copies slot 1 to slot 0.
#   A bad or unsigned image is rejected by MCUboot, and the old firmware keeps running.
set -euo pipefail
IMG=${1:?usage: $0 zephyr.signed.bin}

python3 - "$IMG" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read(32)
magic, _, _, _, size, _, maj, mnr, rev, bld = struct.unpack('<IIHHIIBBHI', b[:28])
if magic != 0x96f3b83d:
    sys.exit("not an MCUboot image (use zephyr.signed.bin from build-mcuboot)")
print(f"image version {maj}.{mnr}.{rev}+{bld}, {size} bytes")
EOF

command -v dfu-util >/dev/null || { echo "install dfu-util first: sudo apt install dfu-util"; exit 1; }
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"
# 'unable to read DFU status after completion' at the end is expected: the mouse reboots
if lsusb | grep -qiE "2fe3:(0102|ffff)"; then
    echo "mouse is in MCUboot recovery (blue LEDs)"
    $SUDO dfu-util -d 2fe3:0102,2fe3:ffff -a 1 -D "$IMG" -R || true   # alt 1 = image_1 (slot 1)
else
    $SUDO dfu-util -d 0951:16e2,2fe3:0101 -a 0 -D "$IMG" || true
fi
echo "waiting for MCUboot to install the image..."
sleep 15
lsusb | grep -iE "0951:16e2|2fe3" || { echo "mouse not back on USB yet; check it"; exit 1; }
echo "done: the mouse is back with the new firmware"
