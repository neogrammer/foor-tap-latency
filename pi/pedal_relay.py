#!/usr/bin/env python3
"""
pedal_relay.py : one foot pedal on the Pi's GPIO header, sent to the Quest tap study over UDP.

WIRING
  One wire of the foot switch to physical pin 11 (GPIO17), the other to physical pin 9 (ground).
  No resistor and no power wire. The Pi holds pin 11 high through its internal pull-up, and
  pressing the pedal shorts it to ground. Which wire goes on which pin does not matter.

WHAT THE HEADSET GETS
  One UDP packet per press and one per release, as compact JSON. A press looks like this:

    {"gesture":"kick","confidence":1.0,"pedal_depth":0.0,"rssi_delta":0.0,"is_kicking":true,
     "kick_force":1.0,"seq":12,"t_ns":8123456789012,"down":true,"late":false,"boot":51966,"v":2}

  The first six fields are the old format, kept so an older build of the app still hears the
  pedal. The rest is what makes the pedal usable as a measuring instrument:

    t_ns   when the pedal closed, on this Pi's monotonic clock, in nanoseconds
    seq    press counter, so a repeated packet is not counted twice and a lost one is noticed
    boot   a random number picked at startup, so the headset can tell the relay restarted

WHY THE TIMESTAMP IS IN THE PACKET
  A packet takes a few milliseconds to cross the WiFi, and not the same few every time. If the
  headset used the arrival time as the tap time, that wobble would be added to every tap. So the
  time is read here, at the pin, and travels inside the packet. The headset then needs to know how
  this Pi's clock relates to its own. It finds out by sending small "probe" packets several times
  a second. Each reply carries the time the probe arrived here and the time the reply left, which
  is enough to work out the offset between the two clocks and how good that estimate is.

  The same probes are how the headset is found. Nothing here needs to know the headset's address:
  whoever probes gets the pedal. That is what lets this box move to another network with no edits.
  --quest is still accepted, for an older app build that does not probe.

DEBOUNCE WITHOUT LYING ABOUT TIME
  A switch does not close cleanly. The contacts chatter for a few milliseconds. A press is
  accepted once the pin has stayed pressed for --confirm-ms (3 by default), which throws away
  electrical spikes on a long cable, but the time reported is the FIRST edge, not the moment of
  acceptance. The wait costs 3 ms on how soon the headset hears about the tap and nothing on the
  tap's time. A release has to hold for 15 ms, so a pedal chattering under a resting foot is not
  counted as a new tap.

RUN IT
  python3 pedal_relay.py                 normal use, pedal on pin 11
  python3 pedal_relay.py --monitor       prints every press and release: use this to test a pedal
  sudo systemctl status footpedal        the boot service, once install_service.sh has been run
  journalctl -u footpedal -f             watch presses arrive, live
"""
import argparse
import collections
import json
import os
import socket
import statistics
import sys
import threading
import time

VERSION = 2
SKEW_NS = 0          # tests only: pretend this Pi's clock runs ahead by this much


def raw_ns():
    return time.monotonic_ns()


def now_ns():
    return time.monotonic_ns() + SKEW_NS


def log(*parts):
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


# ---- The pedal --------------------------------------------------------------------------------
class Pedal:
    """Turns raw edges on one pin into clean press and release events, each carrying the time of
    its first edge."""

    WATCH_NS = 50_000_000            # how often the pin is re-read in case an edge went missing
    RECHECK_NS = 1_000_000           # how soon to look again while a change is still settling
    RELEASE_QUIET_NS = 5_000_000     # a release also needs this long with no edges at all
    PRESS_SETTLE_NS = 25_000_000     # a press that has not settled by now was a spike
    RELEASE_SETTLE_NS = 80_000_000

    def __init__(self, pin, rest, confirm_ms, release_ms, on_event):
        from gpiozero import Device
        Device.ensure_pin_factory()
        self.pin = Device.pin_factory.pin(pin)
        self.pin.function = "input"
        self.pin.pull = "up"
        self.pin.bounce = None       # no library debounce: it would delay the edge, and timing is the point

        self.on_event = on_event
        self.confirm_ns = int(confirm_ms * 1e6)
        self.release_ns = int(release_ms * 1e6)
        self.cv = threading.Condition()
        self.cand = None             # a level change that has been seen but not yet believed
        self.last_edge = 0           # time of the most recent edge of any kind
        self.next_watch = raw_ns() + self.WATCH_NS
        self.mismatch = 0
        self.edges = self.bounces = self.glitches = self.untimed = self.late = 0
        self.lags = collections.deque(maxlen=200)

        self.rest_high = self._find_rest(rest)
        self.down = self._is_down()
        # gpiozero keeps only a weak reference to this callback. It has to be a bound method of an
        # object that stays alive (this one), never a lambda, or it silently stops being called.
        self.pin.when_changed = self._edge
        threading.Thread(target=self._service, daemon=True).start()

    def _find_rest(self, rest):
        """Which level means 'foot off'. Most pedals are open at rest (pin high). Some are wired to
        the other contact and are closed at rest; reading the pin at startup handles both."""
        if rest == "open":
            return True
        if rest == "closed":
            return False
        highs = 0
        for _ in range(20):
            highs += 1 if self.pin.state else 0
            time.sleep(0.005)
        return highs >= 10

    def _is_down(self):
        return bool(self.pin.state) != self.rest_high

    def _lag(self, ticks, raw):
        """How long ago the kernel saw this edge, in ns. The kernel stamps the edge itself, so this
        is the delay between the pin changing and Python being told. Which clock the stamp is on
        depends on the kernel, so try both and keep the one that makes sense."""
        if ticks is None:
            return None
        tick = int(ticks * 1e9)
        for ref in (raw, time.time_ns()):
            lag = ref - tick
            if -1_000_000 <= lag <= 2_000_000_000:
                return max(lag, 0)
        self.untimed += 1
        return None

    def _edge(self, ticks, state):
        raw = raw_ns()
        if state not in (0, 1):      # lgpio also reports watchdog timeouts, as level 2
            return
        lag = self._lag(ticks, raw)
        t = raw - (lag or 0)
        down = bool(state) != self.rest_high
        with self.cv:
            self.edges += 1
            self.last_edge = t
            if self.cand is not None:
                self.bounces += 1    # chatter while a change is being confirmed
                return
            if down == self.down:
                return               # back where we already were: the tail of a rejected spike
            wait = self.confirm_ns if down else self.release_ns
            self.cand = {"down": down, "t": t, "lag": lag, "late": False, "check": t + wait}
            self.cv.notify()

    def _service(self):
        while True:
            with self.cv:
                now = raw_ns()
                due = self.cand["check"] if self.cand else self.next_watch
                if now < due:
                    self.cv.wait((due - now) / 1e9)
                    now = raw_ns()
                event = self._step(now)
            if event:
                self.on_event(*event)

    def _step(self, now):
        c = self.cand
        if c is None:
            if now < self.next_watch:
                return None
            self.next_watch = now + self.WATCH_NS
            # Nothing pending. Check that what we believe still matches the pin. If the kernel ever
            # drops an edge, this is what notices. Two looks in a row, so a bounce cannot trip it.
            if self._is_down() == self.down:
                self.mismatch = 0
                return None
            self.mismatch += 1
            if self.mismatch < 2:
                return None
            # The true edge time is gone, so this one is marked late and the headset is told.
            self.cand = c = {"down": not self.down, "t": now, "lag": None, "late": True, "check": now}
        if now < c["check"]:
            return None
        settled = self._is_down() == c["down"]
        if settled and not c["down"]:
            settled = now - self.last_edge >= self.RELEASE_QUIET_NS
        if settled:
            self.cand = None
            self.down = c["down"]
            self.mismatch = 0
            if c["late"]:
                self.late += 1
            if c["lag"] is not None:
                self.lags.append(c["lag"])
            return (c["down"], c["t"] + SKEW_NS, c["lag"], c["late"])
        if now - c["t"] > (self.PRESS_SETTLE_NS if c["down"] else self.RELEASE_SETTLE_NS):
            self.cand = None         # never settled into the new level: a spike, not a foot
            self.glitches += 1
            return None
        c["check"] = now + self.RECHECK_NS
        return None

    def summary(self):
        with self.cv:
            lags = list(self.lags)
            s = f"edges={self.edges} bounces={self.bounces} spikes_rejected={self.glitches} late={self.late}"
        if lags:
            s += f" callback_ms med={statistics.median(lags) / 1e6:.2f} max={max(lags) / 1e6:.2f}"
        return s


# ---- The network ------------------------------------------------------------------------------
class Link:
    PEER_FRESH_NS = 5_000_000_000    # a headset that has not probed for this long is gone

    def __init__(self, port, quest_ip, monitor, quiet):
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("0.0.0.0", port))
        self.static = (quest_ip, port) if quest_ip else None
        self.peer = None
        self.peer_seen = 0
        self.lock = threading.Lock()
        self.boot = int.from_bytes(os.urandom(4), "big") % 2_000_000_000 + 1
        self.seq = 0                 # presses so far
        self.t_ns = 0                # time of the latest press, on this Pi's clock
        self.down = False
        self.late = False
        self.probes = 0
        self.monitor = monitor
        self.quiet = quiet

    @staticmethod
    def _legacy(gesture, kicking):
        return ('"gesture":"%s","confidence":1.0,"pedal_depth":0.0,"rssi_delta":0.0,'
                '"is_kicking":%s,"kick_force":%s' % (gesture, "true" if kicking else "false",
                                                     "1.0" if kicking else "0.0"))

    def _state(self):
        # Every packet carries the latest press. If the press packet itself is lost, the next
        # packet of any kind tells the headset what it missed, with the right time on it.
        return '"seq":%d,"t_ns":%d,"down":%s,"late":%s,"boot":%d,"v":%d' % (
            self.seq, self.t_ns, "true" if self.down else "false",
            "true" if self.late else "false", self.boot, VERSION)

    def _targets(self):
        out = []
        if self.peer and raw_ns() - self.peer_seen < self.PEER_FRESH_NS:
            out.append(self.peer)
        if self.static and self.static not in out:
            out.append(self.static)
        return out

    def _send(self, pkt, targets):
        for addr in targets:
            try:
                self.sock.sendto(pkt, addr)
            except OSError:
                pass                 # no network yet, or the headset went away: keep running

    def event(self, down, t_ns, lag, late):
        with self.lock:
            held = None
            if down:
                self.seq += 1
                self.t_ns = t_ns
                self.late = late
            elif self.t_ns:
                held = (t_ns - self.t_ns) / 1e6
            self.down = down
            pkt = ("{" + self._legacy("kick" if down else "idle", down) + "," + self._state() + "}").encode()
            targets = self._targets()
            seq = self.seq
        self._send(pkt, targets)
        if down:
            # The same packet again a moment later. WiFi loses packets in bursts, so a copy sent
            # straight away tends to die with the original and one sent 3 ms later usually does
            # not. The headset throws the copy away by its seq.
            again = threading.Timer(0.003, self._send, (pkt, targets))
            again.daemon = True
            again.start()
        if self.quiet:
            return
        if down:
            where = "to " + ", ".join(a[0] for a in targets) if targets else "nobody listening yet"
            extra = "" if lag is None else f"  callback {lag / 1e6:.2f} ms"
            log(f"press #{seq}{'  LATE' if late else ''}{extra}  {where}")
        elif self.monitor:
            log("release" + ("" if held is None else f"  held {held:.0f} ms"))

    def rx_loop(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(2048)
            except OSError:
                time.sleep(0.05)
                continue
            t1 = now_ns()
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if not isinstance(msg, dict):
                continue
            n, t0 = msg.get("probe"), msg.get("t0")
            if not isinstance(n, int) or not isinstance(t0, int):
                continue             # our own broadcast coming back, or something that is not a probe
            with self.lock:
                fresh = self.peer is not None and raw_ns() - self.peer_seen < self.PEER_FRESH_NS
                if addr != self.peer or not fresh:
                    log(f"headset found at {addr[0]}")
                self.peer = addr
                self.peer_seen = raw_ns()
                self.probes += 1
                state = self._state()
                t2 = now_ns()
            pkt = ('{"gesture":"sync","n":%d,"t0":%d,"t1":%d,"t2":%d,%s}' % (n, t0, t1, t2, state)).encode()
            try:
                self.sock.sendto(pkt, addr)
            except OSError:
                pass

    def heartbeat_loop(self):
        while True:
            time.sleep(1.0)
            with self.lock:
                pkt = ("{" + self._legacy("heartbeat", False) + "," + self._state() + "}").encode()
                targets = self._targets()
                searching = not (self.peer and raw_ns() - self.peer_seen < self.PEER_FRESH_NS)
            if searching:
                # Nobody is probing. Shout once a second so a headset on this network can find us
                # even if its own shouts are not getting through.
                targets = targets + [("255.255.255.255", self.port)]
            self._send(pkt, targets)


def main():
    global SKEW_NS
    ap = argparse.ArgumentParser(description="One foot pedal to the Quest tap study. The top of this file explains it.")
    ap.add_argument("--pin", type=int, default=17, help="GPIO number of the pedal (default 17, which is physical pin 11)")
    ap.add_argument("--port", type=int, default=9999, help="UDP port, the same on both ends (default 9999)")
    ap.add_argument("--quest", default=None, help="fixed headset address, only needed by an app build that does not probe")
    ap.add_argument("--rest", choices=["auto", "open", "closed"], default="auto",
                    help="pedal state with the foot off. auto reads the pin at startup (default)")
    ap.add_argument("--confirm-ms", type=float, default=3.0, help="how long a press must hold to count (default 3)")
    ap.add_argument("--release-ms", type=float, default=15.0, help="how long a release must hold to count (default 15)")
    ap.add_argument("--monitor", action="store_true", help="also print releases and a summary: for testing a pedal")
    ap.add_argument("--quiet", action="store_true", help="print nothing per press")
    ap.add_argument("--skew-ms", type=float, default=0.0, help=argparse.SUPPRESS)
    args = ap.parse_args()
    SKEW_NS = int(args.skew_ms * 1e6)

    try:
        link = Link(args.port, args.quest, args.monitor, args.quiet)
    except OSError as e:
        sys.exit(f"cannot open UDP port {args.port}: {e}. Is another copy already running? "
                 f"(sudo systemctl stop footpedal, or pkill -f pedal_relay.py)")
    pedal = Pedal(args.pin, args.rest, args.confirm_ms, args.release_ms, link.event)

    log(f"pedal on GPIO{args.pin}, at rest the pin reads {'HIGH (switch open)' if pedal.rest_high else 'LOW (switch closed)'}")
    if not pedal.rest_high:
        log("  that is a normally-closed pedal, or a foot was on it at startup. If presses and releases "
            "look swapped, restart with the foot off, or pass --rest open.")
    log(f"listening for the headset on UDP {args.port}" + (f", also sending to {args.quest}" if args.quest else ""))

    threading.Thread(target=link.rx_loop, daemon=True).start()
    threading.Thread(target=link.heartbeat_loop, daemon=True).start()

    last = (-1, -1)
    while True:
        time.sleep(10.0 if args.monitor else 60.0)
        seen = (link.seq, pedal.edges)
        if seen != last and not args.quiet:
            last = seen
            log(f"presses={link.seq} {pedal.summary()} probes_answered={link.probes}")


if __name__ == "__main__":
    main()
