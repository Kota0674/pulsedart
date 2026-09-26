#!/usr/bin/env bash
# Step 1: check APPROTECT on the Pulsefire Dart nRF52840 (read-only).
# Run on the Raspberry Pi from this directory:  ./step1.sh
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v openocd >/dev/null; then
    echo "Installing openocd..."
    sudo apt-get update && sudo apt-get install -y openocd
fi

ver=$(openocd --version 2>&1 | head -n1)
echo "$ver"
case "$ver" in
    *" 0.10."*|*" 0.11."*)
        echo "OpenOCD >= 0.12 required (config uses 'adapter gpio' syntax)."; exit 1;;
esac

if ! openocd -c "echo [adapter list]; shutdown" 2>&1 | grep -q linuxgpiod; then
    echo "This openocd build has no linuxgpiod driver."; exit 1
fi

mkdir -p logs
log="logs/step1_$(date +%Y%m%d_%H%M%S).log"

# sudo only if the user is not in the gpio group
SUDO=""
id -nG | grep -qw gpio || SUDO="sudo"

$SUDO openocd -f "${CFG:-pulsedart_nolvl.cfg}" -f check_approtect.tcl 2>&1 | tee "$log"
echo
echo "Log saved to $log"
