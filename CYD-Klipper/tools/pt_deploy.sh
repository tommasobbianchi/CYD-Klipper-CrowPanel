#!/bin/bash
# Build the Panda Touch firmware and flash it over WiFi. espota needs the device to connect back to the host,
# so it runs on behemoth (same LAN as the panel). usage: pt_deploy.sh [panel_ip]
set -e
IP=${1:-192.168.0.225}
HOST=tommaso@100.103.234.2
cd "$(dirname "$0")/.."
~/.platformio/penv/bin/pio run -e esp32-PANDATOUCH 2>&1 | grep -E " error |SUCCESS|FAILED|undefined" | head -10
F=.pio/build/esp32-PANDATOUCH/firmware.bin
scp -q "$F" ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py "$HOST:/tmp/"
ok=0
for try in 1 2 3; do   # OTA occasionally times out on the first invitation
  if ssh -o BatchMode=yes "$HOST" "python3 /tmp/espota.py -d -i $IP -p 3232 -f /tmp/firmware.bin" 2>&1 | tr '\r' '\n' | grep -q "Success"; then ok=1; break; fi
  echo "OTA attempt $try failed"
done
[ $ok = 1 ] || { echo "OTA failed"; exit 1; }
sleep 10   # device reboots into the new image
for i in $(seq 1 20); do
  sleep 2
  if ssh -o BatchMode=yes "$HOST" "ping -c1 -W1 $IP >/dev/null 2>&1"; then echo "panel back"; exit 0; fi
done
echo "panel did not come back"; exit 1
