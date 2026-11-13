#!/bin/bash
# start_relay.sh : run the relay by hand in this terminal and print every press and release.
# This is for testing a pedal. Normal use needs none of it: the footpedal service starts at boot
# (see install_service.sh).
cd "$(dirname "$0")" || exit 1
sudo systemctl stop footpedal 2>/dev/null
pkill -f '[p]edal_relay.py' 2>/dev/null
sleep 0.5
echo "Ctrl+C to stop. Afterwards bring the service back with:  sudo systemctl start footpedal"
exec python3 -u pedal_relay.py --monitor "$@"
