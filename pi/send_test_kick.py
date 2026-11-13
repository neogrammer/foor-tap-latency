#!/usr/bin/env python3
"""Sends one fake pedal press to the headset, to prove the network path without touching a pedal.
It uses the old packet format with no timestamp, so the app counts it as a press but marks it
as arrival-timed. Usage: python3 send_test_kick.py 192.168.0.8 9999
"""
import json, socket, sys, time
ip, port = sys.argv[1], int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
base = {"confidence": 1.0, "pedal_depth": 0.0, "rssi_delta": 0.0}
for gesture, kicking, force in (("kick", True, 1.0), ("idle", False, 0.0)):
    msg = dict(base, gesture=gesture, is_kicking=kicking, kick_force=force)
    s.sendto(json.dumps(msg, separators=(",", ":")).encode(), (ip, port))
    time.sleep(0.1)
print("fake press sent to", ip, port)
