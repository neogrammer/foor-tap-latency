#!/usr/bin/env python3
"""End to end, off the hardware: the real relay code and the real headset receiver, with a fake
WiFi in between that delays, reorders and drops packets on purpose.

The relay's clock is deliberately set wrong by a large amount. Both programs run on this one
machine, so the true time of every simulated press is known exactly, and the question is simple:
after crossing the fake WiFi and the clock correction, how far off is the time the headset ends
up with?"""
import heapq, json, os, random, socket, statistics, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "pi"))
import pedal_relay as R
from gpiozero import Device
from gpiozero.pins.mock import MockFactory
Device.pin_factory = MockFactory()

HARNESS = os.path.join(HERE, os.environ.get("HARNESS", "recv_harness"))
failures = []
summary = {}          # written to link_results.json at the end, for the report
_port = [21000]
_pin = [2]


def port():
    _port[0] += 1
    return _port[0]


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + (f"   [{detail}]" if detail else ""))
    if not cond:
        failures.append(name)


class FakeWifi:
    """Sits between the headset and the relay. Each direction gets its own base delay, random
    extra delay, occasional long stalls, and a chance of dropping the packet."""

    def __init__(self, headset_port, relay_port, up_ms=1.5, down_ms=1.5, jitter_ms=0.5,
                 loss=0.0, stall_p=0.0, drop_if=None):
        self.to_headset = ("127.0.0.1", headset_port)
        self.to_relay = ("127.0.0.1", relay_port)
        self.a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)   # the headset talks to this one
        self.a.bind(("127.0.0.1", 0))
        self.b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)   # the relay talks to this one
        self.b.bind(("127.0.0.1", 0))
        self.port_a = self.a.getsockname()[1]
        self.port_b = self.b.getsockname()[1]
        self.up, self.down, self.jitter = up_ms, down_ms, jitter_ms
        self.loss, self.stall_p, self.drop_if = loss, stall_p, drop_if
        self.q, self.cv, self.n = [], threading.Condition(), 0
        self.alive = True
        self.dropped = 0
        for sock, out, dest, base in ((self.a, self.b, self.to_relay, self.up),
                                      (self.b, self.a, self.to_headset, self.down)):
            sock.settimeout(0.1)
            threading.Thread(target=self._pump, args=(sock, out, dest, base), daemon=True).start()
        threading.Thread(target=self._deliver, daemon=True).start()

    def _pump(self, sock, out, dest, base):
        while self.alive:
            try:
                data, _ = sock.recvfrom(4096)
            except OSError:
                continue
            if random.random() < self.loss or (self.drop_if and self.drop_if(data)):
                self.dropped += 1
                continue
            d = base + random.expovariate(1.0 / self.jitter) if self.jitter > 0 else base
            if random.random() < self.stall_p:
                d += random.uniform(20, 60)
            with self.cv:
                self.n += 1
                heapq.heappush(self.q, (time.monotonic_ns() + int(d * 1e6), self.n, data, out, dest))
                self.cv.notify()

    def _deliver(self):
        while self.alive:
            with self.cv:
                if not self.q:
                    self.cv.wait(0.05)
                    continue
                due = self.q[0][0]
                now = time.monotonic_ns()
                if now < due:
                    self.cv.wait((due - now) / 1e9)
                    continue
                _, _, data, out, dest = heapq.heappop(self.q)
            try:
                out.sendto(data, dest)
            except OSError:
                pass

    def close(self):
        self.alive = False


class Bench:
    """One relay, one fake WiFi, one headset receiver."""

    def __init__(self, seconds, skew_ms=987654.321, start_headset=True, **wifi):
        R.SKEW_NS = int(skew_ms * 1e6)
        self.headset_port, self.relay_port = port(), port()
        self.wifi = FakeWifi(self.headset_port, self.relay_port, **wifi)
        self.seconds = seconds
        self.truth = []
        self.proc = None
        self.make_relay()
        if start_headset:
            self.start_headset()

    def make_relay(self):
        _pin[0] += 1
        self.link = R.Link(self.relay_port, None, False, True)
        # Broadcast does not loop back on this machine, so point the relay's heartbeat at the fake
        # WiFi by hand. On the real network the headset's broadcast probe does this job.
        self.link.static = ("127.0.0.1", self.wifi.port_b)
        self.pedal = R.Pedal(_pin[0], "auto", 3.0, 15.0, self.link.event)
        threading.Thread(target=self.link.rx_loop, daemon=True).start()
        threading.Thread(target=self.link.heartbeat_loop, daemon=True).start()

    def start_headset(self):
        self.proc = subprocess.Popen([HARNESS, str(self.headset_port), str(self.wifi.port_a), str(self.seconds)],
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def tap(self, hold=0.05, bounces=2):
        t = R.raw_ns()
        self.pedal.pin.drive_low()
        for _ in range(bounces):
            time.sleep(0.0003); self.pedal.pin.drive_high(); time.sleep(0.0003); self.pedal.pin.drive_low()
        self.truth.append(t)
        time.sleep(hold)
        self.pedal.pin.drive_high()

    def finish(self):
        out, _ = self.proc.communicate(timeout=self.seconds + 10)
        self.wifi.close()
        self.link.sock.close()
        rows = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
        self.downs = [r for r in rows if r["ev"] == "down"]
        self.link_stats = next((r for r in rows if r["ev"] == "link"), {})
        return self.downs, self.link_stats

    def errors_ms(self):
        """Headset's idea of each press time minus the truth, matched by press number."""
        by_seq = {d["seq"]: d for d in self.downs}
        return [(by_seq[i + 1]["mono"] - t) / 1e6 for i, t in enumerate(self.truth) if i + 1 in by_seq]


def record(key, label, bench, downs, link, errs):
    late = [(d["arrival"] - d["mono"]) / 1e6 for d in downs]
    a = sorted(abs(e) for e in errs)
    summary[key] = dict(
        label=label, sent=len(bench.truth), arrived=len(downs),
        timed=sum(1 for d in downs if d["timed"]), rebuilt=sum(1 for d in downs if d["recovered"]),
        lost=link.get("lost", 0), dropped_by_network=bench.wifi.dropped,
        err_median_ms=round(statistics.median(errs), 3) if errs else None,
        err_worst_ms=round(max(a), 3) if a else None,
        err_p95_ms=round(a[int(0.95 * (len(a) - 1))], 3) if a else None,
        claimed_bound_ms=round(max(d["rtt"] for d in downs) / 2e6, 3) if downs else None,
        arrival_late_median_ms=round(statistics.median(late), 2) if late else None,
        arrival_late_min_ms=round(min(late), 2) if late else None,
        arrival_late_max_ms=round(max(late), 2) if late else None)


def describe(errs):
    if not errs:
        return "no presses"
    a = sorted(abs(e) for e in errs)
    return f"median {statistics.median(errs):+.2f} ms, worst {max(errs, key=abs):+.2f} ms, 95% within {a[int(0.95 * (len(a) - 1))]:.2f} ms"


random.seed(7)

# On this one machine the headset cannot find the relay by broadcast, so it has to wait for the
# relay's once-a-second heartbeat and then for its own next probe. That takes up to a second and a
# half here. On a real network the headset's broadcast probe does it in a fraction of a second.
SETTLE = 2.6

print("A. quiet home WiFi: 1.5 ms each way, no loss. Relay clock set wrong by 16 minutes.")
b = Bench(9.6)
time.sleep(SETTLE)
for _ in range(30):
    b.tap(); time.sleep(0.15)
time.sleep(0.6)
downs, link = b.finish()
errs = b.errors_ms()
check("all 30 presses arrived", len(downs) == 30, f"{len(downs)}")
check("every one carried the Pi's own time", all(d["timed"] for d in downs))
check("press time right to within 0.5 ms", bool(errs) and max(abs(e) for e in errs) < 0.5, describe(errs))
check("each repeat copy was recognised and dropped", link.get("duplicates", 0) >= 25, f"{link.get('duplicates')} duplicates")
check("nothing lost, nothing rebuilt", link.get("lost") == 0 and link.get("recovered") == 0)
record("quiet", "Quiet network: 1.5 ms each way, no loss", b, downs, link, errs)
wobble = [(d["arrival"] - d["mono"]) / 1e6 for d in downs]
print(f"       for comparison, arrival time was late by {statistics.median(wobble):.1f} ms (range {min(wobble):.1f} to {max(wobble):.1f})")

print("B. bad WiFi: 3 ms up, 5 ms down, jitter, 10% long stalls, 15% of packets dropped.")
b = Bench(14.2, up_ms=3.0, down_ms=5.0, jitter_ms=4.0, loss=0.15, stall_p=0.10)
time.sleep(SETTLE)
for _ in range(40):
    b.tap(); time.sleep(0.20)
time.sleep(0.8)
downs, link = b.finish()
errs = b.errors_ms()
check("all 40 presses still arrived", len(downs) == 40, f"{len(downs)} arrived, receiver says {link.get('lost')} lost, {link.get('recovered')} rebuilt")
check("press time right to within 2.5 ms despite the mess", bool(errs) and max(abs(e) for e in errs) < 2.5, describe(errs))
bound = max(d["rtt"] for d in downs) / 2e6 if downs else 0
check("and inside the error the receiver itself claims (half the round trip)",
      bool(errs) and all(abs(e) <= d["rtt"] / 2e6 + 0.3 for e, d in zip(errs, sorted(downs, key=lambda d: d["seq"]))),
      f"claimed bound up to {bound:.2f} ms")
wobble = [(d["arrival"] - d["mono"]) / 1e6 for d in downs]
print(f"       for comparison, arrival time was late by {statistics.median(wobble):.1f} ms (range {min(wobble):.1f} to {max(wobble):.1f})")
print(f"       fake WiFi dropped {b.wifi.dropped} packets")
record("bad", "Bad network: 3 ms up, 5 ms down, jitter, 10% stalls of 20 to 60 ms, 15% loss", b, downs, link, errs)

print("C. every press packet is destroyed. Only heartbeats and probe replies get through.")
b = Bench(9.6, drop_if=lambda data: b'"gesture":"kick"' in data)
time.sleep(SETTLE)
for _ in range(20):
    b.tap(); time.sleep(0.25)
time.sleep(0.6)
downs, link = b.finish()
errs = b.errors_ms()
check("all 20 presses were rebuilt from later packets", len(downs) == 20 and all(d["recovered"] for d in downs), f"{len(downs)} arrived, {link.get('recovered')} rebuilt")
check("with the right press time on them", bool(errs) and max(abs(e) for e in errs) < 0.5, describe(errs))
record("nopress", "Every press packet destroyed in transit", b, downs, link, errs)

print("D. the headset app is opened after the pedal has already been pressed 7 times.")
b = Bench(6.2, start_headset=False)
for _ in range(7):
    b.tap(); time.sleep(0.08)
b.truth.clear()
b.start_headset()
time.sleep(SETTLE)
check_before = None
for _ in range(3):
    b.tap(); time.sleep(0.2)
time.sleep(0.5)
downs, link = b.finish()
check("old presses are not replayed, only the 3 new ones count", len(downs) == 3, f"{len(downs)} presses, seqs {[d['seq'] for d in downs]}")

print("E. the relay is restarted in the middle (power blip on the Pi).")
b = Bench(10.2)
time.sleep(SETTLE)
for _ in range(5):
    b.tap(); time.sleep(0.15)
b.link.sock.close(); time.sleep(0.3)
b.make_relay()                                  # new process in real life: counter back to zero, new boot id
time.sleep(1.5)
for _ in range(5):
    b.tap(); time.sleep(0.15)
time.sleep(0.6)
downs, link = b.finish()
check("5 before and 5 after, no phantom press at the restart", len(downs) == 10, f"{len(downs)} presses, seqs {[d['seq'] for d in downs]}")
check("all still timed", all(d["timed"] for d in downs))

print("F. the old two-pedal relay format, with no timestamp in the packet.")
hp, rp = port(), port()
w = FakeWifi(hp, rp)
proc = subprocess.Popen([HARNESS, str(hp), str(w.port_a), "3.0"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
old = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
time.sleep(0.5)
base = '"confidence":1.0,"pedal_depth":%s,"rssi_delta":0.0,"is_kicking":%s,"kick_force":%s'
for g, depth, kicking, force in (("kick", "0.0", "true", "1.0"), ("idle", "0.0", "false", "0.0"),
                                 ("hihat_close", "1.0", "false", "0.0"), ("hihat_open", "0.0", "false", "0.0"),
                                 ("heartbeat", "0.0", "false", "0.0")):
    old.sendto(('{"gesture":"%s",' % g + base % (depth, kicking, force) + "}").encode(), ("127.0.0.1", w.port_b))
    time.sleep(0.1)
out, _ = proc.communicate(timeout=10)
w.close()
rows = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
downs = [r for r in rows if r["ev"] == "down"]
check("a kick and a hi-hat close both count as a pedal press", len(downs) == 2, f"{len(downs)} presses")
check("and are honestly marked as arrival-timed", all(d["timed"] == 0 and d["mono"] == d["arrival"] for d in downs))

print()
# link_results.json is the recorded run the report quotes and is left alone. Each run writes its
# own figures next to it; timing differs a little from run to run because the threads do.
with open(os.path.join(HERE, "link_results_latest.json"), "w") as f:
    json.dump(summary, f, indent=1)
print("ALL PASSED" if not failures else f"{len(failures)} FAILED: " + "; ".join(failures))
sys.exit(1 if failures else 0)
