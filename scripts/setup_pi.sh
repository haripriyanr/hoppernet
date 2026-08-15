#!/usr/bin/env bash
# One-shot setup for Node B (Raspberry Pi 4) running as relay + edge buffer.
# Run: sudo bash setup_pi.sh
set -euo pipefail

echo "==> enabling SPI"
raspi-config nonint do_spi 0 || true

echo "==> installing RF24 python library"
apt-get update -y
apt-get install -y python3-rf24 python3-pip

echo "==> installing project"
PROJ=/home/pi/hoppernet
mkdir -p "$PROJ/firmware/node_b/run"

echo "==> installing systemd service"
cat > /etc/systemd/system/hoppernet-nodeb.service <<EOF
[Unit]
Description=hoppernet Node B relay + edge buffer
After=network.target

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
systemctl enable hoppernet-nodeb
systemctl start hoppernet-nodeb

echo "==> done. Logs: $PROJ/firmware/node_b/run/node_b.log"
echo "    service status: systemctl status hoppernet-nodeb"
