#!/bin/sh
# usage: ./powtest.sh LABEL SECONDS   -> synthetic motion for SECONDS, prints gauge ring
cd ~/pulsedart
oc() { openocd -f pulsedart_nolvl.cfg "$@" 2>&1 | grep -E '^idx'; }
openocd -f pulsedart_nolvl.cfg -c 'set CMD 1' -f cmd.tcl >/dev/null 2>&1
sleep $2
echo "$1: $(oc -f curread.tcl)"
openocd -f pulsedart_nolvl.cfg -c 'set CMD 2' -f cmd.tcl >/dev/null 2>&1
