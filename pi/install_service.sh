#!/bin/bash
# install_service.sh : make the pedal relay start by itself whenever the Pi has power, and come
# back by itself if it ever stops.
#
# Run it once, on the Pi, from the folder that holds pedal_relay.py:
#     bash install_service.sh
#
# After that there is nothing to start. Plug the Pi in and the pedal is live in about 20 seconds.
#
# Check on it:   systemctl status footpedal
# Watch presses: journalctl -u footpedal -f
# Undo:          sudo systemctl disable --now footpedal && sudo rm /etc/systemd/system/footpedal.service
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
WHO="$(id -un)"

# Anything started by hand earlier would be holding the pin and the port.
pkill -f '[p]edal_relay.py' 2>/dev/null || true

sudo tee /etc/systemd/system/footpedal.service >/dev/null <<UNIT
[Unit]
Description=Foot pedal relay for the Quest tap study
After=network.target

[Service]
User=$WHO
WorkingDirectory=$DIR
ExecStart=/usr/bin/python3 -u $DIR/pedal_relay.py
Restart=always
RestartSec=2
Nice=-10

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable footpedal >/dev/null 2>&1
sudo systemctl restart footpedal
sleep 2
systemctl --no-pager --lines=6 status footpedal
