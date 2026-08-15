#!/usr/bin/env bash
# Runs on EVERY boot of Node B (via hoppernet-update.service, before relay).
# Pulls latest system packages + RF24, rebuilds, then the relay service starts.
set -euo pipefail
exec > /var/log/hoppernet-onboot.log 2>&1

echo "=== hoppernet onboot $(date) ==="

# Wait for network (WiFi / ethernet)
for i in $(seq 1 60); do
  if ip route | grep -q default; then break; fi
  sleep 2
done

echo "==> apt update + install tools"
apt-get update -y
apt-get install -y build-essential cmake libboost-python-dev python3-dev python3-pip git
apt-get upgrade -y || true

raspi-config nonint do_spi 0 || true
systemctl enable ssh || true

echo "==> pulling latest hoppernet (relay.py)"
PROJ=/home/pi/hoppernet
if [ -d "$PROJ/.git" ]; then
  cd "$PROJ" && git pull --ff-only || true
else
  git clone https://github.com/haripriyanr/hoppernet.git "$PROJ" || true
fi
mkdir -p "$PROJ/firmware/node_b/run"

echo "==> pulling latest RF24 + rebuilding"
SRC=/home/pi/rf24
if [ -d "$SRC/.git" ]; then
  cd "$SRC" && git pull --ff-only || true
else
  git clone https://github.com/nRF24/RF24.git "$SRC"
fi

cd "$SRC"
mkdir -p build && cd build
cmake .. -D RF24_DRIVER=SPIDEV -D CMAKE_BUILD_TYPE=Release
make -j4
make install
ldconfig

cd "$SRC/pyRF24"
python3 setup.py build
python3 setup.py install

# Self-update: install freshly-pulled onboot.sh for the NEXT boot
if [ -f "$PROJ/scripts/onboot.sh" ]; then
  install -m 0755 "$PROJ/scripts/onboot.sh" /usr/local/bin/hoppernet-onboot.sh
fi

echo "=== onboot done ==="