#!/usr/bin/env bash
# Node B (Raspberry Pi 4) provisioning — relay + edge buffer.
# Run: sudo bash setup_pi.sh
# This mirrors what the SD card does on first boot (see /boot/hoppernet-stage/firstboot.sh)
# for re-provisioning an already-running Pi.
set -euo pipefail

echo "==> enabling SPI"
raspi-config nonint do_spi 0 || true

echo "==> wait for network"
for i in $(seq 1 60); do
  if ip route | grep -q default; then break; fi
  sleep 2
done

echo "==> installing tools"
apt-get update -y
apt-get install -y build-essential cmake libboost-python-dev python3-dev python3-pip git
apt-get upgrade -y || true

echo "==> cloning hoppernet project"
PROJ=/home/pi/hoppernet
if [ ! -d "$PROJ/.git" ]; then
  git clone https://github.com/haripriyanr/hoppernet.git "$PROJ"
else
  cd "$PROJ" && git pull --ff-only || true
fi
mkdir -p "$PROJ/firmware/node_b/run"

echo "==> installing onboot script"
install -m 0755 "$PROJ/scripts/onboot.sh" /usr/local/bin/hoppernet-onboot.sh

echo "==> installing update service (runs every boot)"
cat > /etc/systemd/system/hoppernet-update.service <<EOF
[Unit]
Description=hoppernet update + rebuild RF24 (each boot)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/hoppernet-onboot.sh

[Install]
WantedBy=multi-user.target
EOF

echo "==> installing relay service"
cat > /etc/systemd/system/hoppernet-nodeb.service <<EOF
[Unit]
Description=hoppernet Node B relay + edge buffer
After=hoppernet-update.service
Requires=hoppernet-update.service

[Service]
User=pi
WorkingDirectory=$PROJ/firmware/node_b
ExecStart=/usr/bin/python3 $PROJ/firmware/node_b/relay.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable hoppernet-update hoppernet-nodeb
systemctl start hoppernet-update

echo "==> done. Logs: $PROJ/firmware/node_b/run/node_b.log"
echo "    status: systemctl status hoppernet-nodeb"