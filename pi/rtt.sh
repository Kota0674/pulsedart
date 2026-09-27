#!/usr/bin/env bash
# Show the mouse firmware log (SEGGER RTT over SWD). Read-only: attaches without reset.
#   usage: ./rtt.sh        (Ctrl+C to stop)
cd "$(dirname "$0")"
openocd -f pulsedart_nolvl.cfg \
    -c "init" \
    -c "rtt setup 0x20000000 0x40000 {SEGGER RTT}" \
    -c "rtt start" \
    -c "rtt server start 9090 0" 2>&1 | grep -vE "swdio to input" &
OCD=$!
trap 'kill $OCD 2>/dev/null' EXIT
sleep 2
nc localhost 9090
