#!/usr/bin/env python3
"""Drives the relay's debounce logic through a mock pin. No hardware, no network.

Each scenario feeds the pin the edges a real switch would produce, then checks two things: that the
right events came out, and that each event carries the time of its FIRST edge rather than the moment
it was accepted."""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pi"))
import pedal_relay as R
from gpiozero import Device
from gpiozero.pins.mock import MockFactory
Device.pin_factory = MockFactory()         # stands in for the Pi's real pins

failures = []


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + (f"   [{detail}]" if detail else ""))
    if not cond:
        failures.append(name)


def ms(ns):
    return ns / 1e6


class Rig:
    def __init__(self, pin_no, rest="auto", confirm=3.0, release=15.0, start_low=False):
        if start_low:                      # a normally-closed pedal reads LOW with the foot off
            p = Device.pin_factory.pin(pin_no)
            p.function = "input"
            p.pull = "floating"
            p.drive_low()
        self.events = []
        self.pedal = R.Pedal(pin_no, rest, confirm, release, self._got)
        if start_low:
            self.pedal.pin.drive_low()     # pull="up" in the constructor floated it high; put it back
            self.pedal.rest_high = False
            self.pedal.down = False
        self.pin = self.pedal.pin

    def _got(self, down, t_ns, lag, late):
        self.events.append(dict(down=down, t=t_ns, lag=lag, late=late, seen=R.raw_ns()))

    def level(self, pressed):
        """Put the contact in the pressed or released position, honouring the wiring."""
        closed_is_low = self.pedal.rest_high
        if pressed == closed_is_low:
            self.pin.drive_low()
        else:
            self.pin.drive_high()

    def press(self, bounces=0, gap=0.0004):
        t = R.raw_ns()
        self.level(True)
        for _ in range(bounces):
            time.sleep(gap); self.level(False); time.sleep(gap); self.level(True)
        return t

    def release(self, bounces=0, gap=0.0004):
        t = R.raw_ns()
        self.level(False)
        for _ in range(bounces):
            time.sleep(gap); self.level(True); time.sleep(gap); self.level(False)
        return t

    def downs(self):
        return [e for e in self.events if e["down"]]

    def ups(self):
        return [e for e in self.events if not e["down"]]


print("1. one clean press and release")
r = Rig(17)
check("rest level detected as open (high)", r.pedal.rest_high is True)
t0 = r.press(); time.sleep(0.12); t1 = r.release(); time.sleep(0.08)
d, u = r.downs(), r.ups()
check("exactly one press, one release", len(d) == 1 and len(u) == 1, f"{len(d)} down, {len(u)} up")
if d:
    check("press time is the first edge, within 0.3 ms", abs(d[0]["t"] - t0) < 300_000, f"{ms(d[0]['t'] - t0):+.3f} ms")
    acc = ms(d[0]["seen"] - t0)
    check("press accepted about 3 ms after the edge", 2.9 <= acc <= 6.0, f"{acc:.2f} ms")
    check("not flagged late", d[0]["late"] is False)
if u:
    check("release time is its first edge", abs(u[0]["t"] - t1) < 300_000, f"{ms(u[0]['t'] - t1):+.3f} ms")

print("2. a bouncy press: 6 chatters over about 5 ms")
r = Rig(18)
t0 = r.press(bounces=6); time.sleep(0.10); r.release(bounces=4); time.sleep(0.10)
d, u = r.downs(), r.ups()
check("still one press, one release", len(d) == 1 and len(u) == 1, f"{len(d)} down, {len(u)} up")
if d:
    check("press time is the FIRST edge, not the last bounce", abs(d[0]["t"] - t0) < 300_000, f"{ms(d[0]['t'] - t0):+.3f} ms")
check("bounces were counted, not emitted", r.pedal.bounces >= 6, f"bounces={r.pedal.bounces}")

print("3. an electrical spike: low for 0.2 ms, nobody pressed anything")
r = Rig(19)
r.level(True); time.sleep(0.0002); r.level(False); time.sleep(0.08)
check("no press reported", len(r.downs()) == 0, f"{len(r.downs())} down")
check("counted as a rejected spike", r.pedal.glitches == 1, f"spikes={r.pedal.glitches}")
t0 = r.press(); time.sleep(0.08); r.release(); time.sleep(0.06)
check("and a real press right after still works", len(r.downs()) == 1 and abs(r.downs()[0]["t"] - t0) < 300_000)

print("4. the contact opens for 6 ms while the foot is still down")
r = Rig(20)
r.press(); time.sleep(0.10)
r.level(False); time.sleep(0.006); r.level(True)
time.sleep(0.15)
check("no extra tap from the chatter", len(r.downs()) == 1, f"{len(r.downs())} down")
check("no release reported either", len(r.ups()) == 0, f"{len(r.ups())} up")
r.release(); time.sleep(0.06)
check("the real release still arrives", len(r.ups()) == 1)

print("5. ten taps, 150 ms apart, each held 60 ms")
r = Rig(21)
truth = []
for _ in range(10):
    truth.append(r.press(bounces=2)); time.sleep(0.06); r.release(bounces=2); time.sleep(0.09)
time.sleep(0.05)
d = r.downs()
check("ten presses out", len(d) == 10, f"{len(d)}")
if len(d) == 10:
    errs = [ms(e["t"] - t) for e, t in zip(d, truth)]
    check("every one stamped at its own first edge", max(abs(x) for x in errs) < 0.3, f"worst {max(errs, key=abs):+.3f} ms")

print("6. a pedal wired closed-at-rest (the other contact)")
r = Rig(22, start_low=True)
t0 = r.press(); time.sleep(0.08); r.release(); time.sleep(0.06)
d, u = r.downs(), r.ups()
check("pressing it is still a press", len(d) == 1 and len(u) == 1 and abs(d[0]["t"] - t0) < 300_000, f"{len(d)} down, {len(u)} up")

print("7. the kernel drops an edge entirely")
r = Rig(23)
saved = r.pin.when_changed
r.pin.when_changed = None                 # the press edge will never be delivered
r.level(True); time.sleep(0.005)
r.pin.when_changed = saved
time.sleep(0.25)
d = r.downs()
check("the press is noticed anyway", len(d) == 1, f"{len(d)} down")
if d:
    check("and is honestly marked late", d[0]["late"] is True)
r.release(); time.sleep(0.06)
check("then everything is back in step", len(r.ups()) == 1)

print("8. confirm set to 0: fire on the first edge")
r = Rig(24, confirm=0.0)
t0 = r.press(bounces=3); time.sleep(0.08); r.release(); time.sleep(0.06)
d = r.downs()
check("one press", len(d) == 1, f"{len(d)}")
if d:
    acc = ms(d[0]["seen"] - t0)
    check("accepted in well under a millisecond", acc < 1.0, f"{acc:.3f} ms")

print()
print("ALL PASSED" if not failures else f"{len(failures)} FAILED: " + "; ".join(failures))
sys.exit(1 if failures else 0)
