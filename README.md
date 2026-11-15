# Foot Tap Latency Study

How much input lag can foot tapping tolerate in VR before timing performance falls apart,
and before the user notices?

Justin Hardee. CSCI-7420 Human-Computer Interaction, Fall 2026, Augusta University.
Milestone 5, final release.

- **Report:** https://neogrammer.github.io/foor-tap-latency/final_report.html
- **Live analysis page:** https://neogrammer.github.io/foor-tap-latency/analysis.html
- **Executable:** `app-debug.apk`, at the top of this repository
- **Demo video:** linked at the top of the report

---

## What changed since the beta

- **The tap is a real foot pedal now.** A foot switch on a Raspberry Pi, timed at the switch
  itself, with the Pi's clock matched to the headset's. The floor controller still works as
  a fallback.
- **A third participant**, run on the pedal. The 63 ms floor the beta found showed up again
  at 62 ms on completely different input hardware.
- **Two things the beta said were wrong, and the session logs proved it.** The display was
  not running at 72 Hz, it runs at whatever the headset picks (90 Hz for the first two
  sessions, 72 Hz for the third). And the controller's tap timestamp is not the moment of
  the press, it is the moment the frame checked the controller. Both are in the report,
  section 5.7.
- **Every session file now says what it was recorded with:** pedal or controller, how good
  the pedal's clock was, how many taps were lost, and the frame period the headset actually
  ran at.
- **The ranking screen was reworded.** A participant could not tell which way the ranking
  ran and had to ask. It now says what is being picked on every screen.
- **Tests.** The pedal's timing is checked by running the real relay and the real headset
  receiver against each other over a fake network that drops and delays packets.

## Layout

```
Chapter2/            the Quest app, an Android Studio project
Common/              shared scaffold from the Khronos OpenXR tutorial
Shaders/             shader sources, used by the scaffold
cmake/               build helpers, used by the scaffold
pi/                  the pedal relay that runs on the Raspberry Pi
tests/               bench tests for the pedal and the experiment logic
app-debug.apk        a prebuilt APK, for sideloading without building
analysis.html        the analysis and charting page, open it in a browser
final_report.html    the final report
data/                the three recorded sessions, for loading into the page
sample_session.csv   synthetic data, for checking the charts without a headset
```

`Chapter2`, `Common`, `Shaders` and `cmake` all come from the Khronos tutorial and all four
are needed for the build. My own work is the files listed further down, plus `pi/`, `tests/`
and `analysis.html`.

## What this is

Three programs with files between them.

**`Chapter2/`** is the headset app. Native OpenXR and C++ on a Meta Quest 3, built on the
Khronos OpenXR tutorial project with Vulkan. The participant taps a foot pedal in time with
a metronome. The app then waits a set amount of time before clicking and flashing a pad on
the floor to confirm the tap. That wait is the thing being studied: 0, 50, 100, 200 or
400 ms, one value per block. Every beat, tap and confirmation is written to a CSV with a
nanosecond timestamp.

Nothing is held in the hand, so the menus are driven by hand tracking: point a finger at a
button and hold still for one second.

After the five scored blocks the session collects NASA-TLX across six dimensions and a
preference ranking of the five blocks.

**`pi/`** is the pedal. A small Python program on a Raspberry Pi reads the foot switch,
stamps each press with the time it happened, and sends it to the headset over WiFi.

**`analysis.html`** reads the CSV and charts it. It pairs each tap with its nearest beat,
works out how consistent the tapping was, and plots that against lag alongside the
participant's own responsiveness ratings. It also runs the whole experiment from the
keyboard as a desktop pilot, which is how the measures and the chart code were checked
before the headset work started.

Nothing is calculated inside the headset. The device logs raw events only, so a mistake in
the pairing rule is fixed by editing the page, not by asking a participant to come back.

---

## Setup: the analysis page

No build step and no dependencies.

1. Open https://neogrammer.github.io/foor-tap-latency/analysis.html, or download
   `analysis.html` and open it in any desktop browser.
2. Click **Choose Files** and pick **all three** files in `data/`. Those are the real
   recorded sessions, so the charts that come up are the actual result, with one line per
   participant on shared axes.

The page shows one session in detail (the last one alphabetically) and all of them together
at the bottom. To see a different one in detail, load it on its own.

You can also click **Start session** to run the whole experiment on the keyboard, or load
`sample_session.csv`, which is synthetic and exists only for checking the chart code.

The page pairs each tap with at most one beat, derives the latency actually delivered from
the logged feedback events, fits the bend in each curve, and shows the NASA-TLX and ranking
results. The fitted bend is only drawn when it is clearly better than the next candidate
breakpoint. With five conditions it often is not, and the page says so instead of naming a
threshold the data cannot support.

The summary at the top says whether a session used the pedal or the controller and what
display rate the headset ran at. Every per-block number in the report can be reproduced by
loading the matching file.

---

## Setup: the Quest app

If you only want to run it, `app-debug.apk` at the top of the repository is a prebuilt
build. Put the Quest in developer mode and sideload it:

```
adb install -r app-debug.apk
```

To build it yourself you need Android Studio, the Android NDK, and a Meta Quest 3 in
developer mode.

1. Clone or download this repository.
2. In Android Studio, **Open** the `Chapter2` folder.
3. Let it sync. It will fetch the OpenXR loader and the Gradle wrapper on its own.
4. Plug in the headset and run.

If the NDK is missing, Android Studio will say so and offer to install it. Accept.

### What is mine and what is not

The project is built on the Khronos OpenXR tutorial
(https://github.com/KhronosGroup/OpenXR-Tutorials), which supplies the Vulkan setup, the
session and swapchain handling, and the Android glue. The experiment is mine, and it lives
in these files under `Chapter2/app/src/main/cpp/`:

| File | What it does |
|---|---|
| `TapExperiment.h` | The whole study: block order, lead-in, countdown, five lag conditions, ratings, NASA-TLX, ranking |
| `TapLogger.h` | CSV event log, one row per beat, tap, confirmation, rating and block boundary |
| `AudioEngine.h` | AAudio stream, click scheduling pinned to exact audio samples |
| `XrClock.h` | Converts between the system clock and the runtime clock |
| `network/PedalReceiver.h`, `.cpp` | Hears the pedal over WiFi, matches the Pi's clock, puts each press on the headset's clock |
| `XRInput.h` | Action set and bindings for the fallback floor controller |
| `HandTracking.h` | Hand joints and the aim ray |
| `HandPointer.h` | Point-and-hold selection, plus the panel button layout |
| `TextRenderer.h` | Stroke font. Letters built from thin rotated cuboids |
| `main.cpp` | The tutorial file, heavily modified. The scene drawing and the per-frame loop are mine |

---

## Setup: the pedal

**Parts.** Any momentary foot switch, a Raspberry Pi with WiFi (a Pi 5 here), two wires.

**Wiring.** One wire of the switch to physical pin 11 (GPIO17), the other to physical pin 9
(ground). No resistor and no power wire. Which wire goes on which pin does not matter.

**Install, once.** Copy the `pi/` folder to the Pi and run the installer from inside it:

```
scp -r pi <user>@<pi-address>:pedals
ssh <user>@<pi-address> "cd pedals && bash install_service.sh"
```

That makes the relay start by itself whenever the Pi has power. There is nothing to start by
hand after this. It needs `python3-gpiozero`, which Raspberry Pi OS ships with (`sudo apt install python3-gpiozero` if it is missing).

**Use.** Plug the Pi in and wait about 20 seconds. Put the headset on the same WiFi as the
Pi and open the app. The first screen says **PEDAL READY** under the panel. Stomp once and a
cube lights up and the count goes up.

No address is typed in anywhere. The headset shouts for the pedal on the network, the Pi
answers, and they talk directly from then on. Moving to a different network needs no
changes, as long as both devices are on it. On a network that will not let two devices talk
to each other (some campus WiFi), use a phone hotspot and join both to that.

**If the first screen says something else:**

| It says | What it means |
|---|---|
| PEDAL OFFLINE | The Pi is off, still starting up, or on a different network |
| PEDAL FOUND  SYNCING | The Pi answered but the clocks are not matched yet. A second or two is normal. Longer means the WiFi is poor |
| PEDAL READY  0 TAPS, and stomping does nothing | The Pi is fine but is not seeing the switch. Check the two wires |

**Check on the Pi itself:**

```
systemctl status footpedal        is it running
journalctl -u footpedal -f        watch presses arrive, live
bash start_relay.sh               stop the service and run it in this terminal, printing every press and release
```

**No pedal?** Lay a Quest controller flat on the floor and step on it. The app takes a tap
from either. The session file records which one was used.

---

## Running a session

The app opens on a check screen. Four lights at the bottom right show the left controller,
the right controller, hand tracking and audio. Under the panel is the pedal status.

**The participant number sets itself.** The app looks at the session files already on the
headset and takes the next free number. Hold a finger on **+** or **-** to change it. The
number also picks the row of the Latin square, so each participant gets a different
condition order. One build runs everybody; nothing needs editing between sessions.

Point a finger at the **GO** button and hold still for a second to advance. Every screen that
waits for you has GO in the same place.

The session then runs: a practice block at zero lag, five scored blocks of 60 taps with a 1
to 7 responsiveness rating after each, six NASA-TLX screens, and a ranking of the five blocks
from most to least responsive. The three recorded sessions took between 5 and 11 minutes
after practice. Tap in time with the beat and ignore what the floor pad does, since that is
the lag being tested.

Run sessions on the headset's own speakers. Bluetooth audio adds 100 ms or more and would
swamp the conditions.

### Getting the data off

The CSV is written when the session finishes and again on exit.

```
adb pull /sdcard/Android/data/com.example.vulkanxray/files/taps_P01.csv
```

---

## Conditions and measures

Five lag settings: 0, 50, 100, 200 and 400 ms. They bracket MacKenzie and Ware's range and
straddle Waltemate's 75 ms whole-body threshold. Order rotates by participant number. Each
block opens with four unscored lead-in beats.

Measures: the signed gap between each tap and its beat, the spread of those gaps (the sample
standard deviation, which is the main one), a seven-point responsiveness rating per block,
NASA-TLX, and a ranking.

---

## How the timing works

The study is one subtraction: when the foot landed, minus when its beat was heard. Both ends
have to be better than a display frame, which is 11 to 14 ms depending on the rate the
headset picks.

- **The beat** is scheduled at an exact audio sample. AAudio returns a matching pair of
  audio frame position and system clock time, so a sample scheduled in the future has a
  known arrival time. The beat is logged at the moment it will be heard.
- **The tap, on the pedal,** is timed on the Pi at the switch, using the kernel's own
  timestamp for the edge. The headset asks the Pi for its clock twenty times a second and
  works out the offset between the two from the fastest recent round trip. In the recorded
  pedal session that round trip was 2.6 ms, so tap times are good to about 1.3 ms, whatever
  the WiFi did to the packet on the way.
- **The tap, on the controller,** uses `XrActionStateBoolean::lastChangeTime`. On this
  headset that turned out to be the time the frame checked the controller, not the time of
  the press, so controller taps are rounded to the frame. That is the reason the pedal
  exists.
- **The confirmation click** is pinned to a sample the same way as the beat, and the time it
  was actually scheduled for is logged. That is how the floor was found: at 0 and 50 ms the
  click cannot come out sooner than about 63 ms, so those two conditions are one condition.
- **One clock for all three.** The pedal press, the beat and the click all go through the
  same conversion between the system clock and the runtime clock, held fixed for the length
  of a block, so whatever error that conversion has is in all of them and cancels.

During a block nothing answers the pedal except the delayed click and flash. An instant lamp
or sound would give away the very thing being withheld.

If audio will not open, the beat falls back to frame-level timing and the session still
runs. The CSV header records which path ran.

---

## CSV format

One header line, then one row per event.

```
# participant=P03 beat_interval_ms=600 audio_timed=1 clock_exact=0 tap_source=pedal controller_taps=0 pedal_taps=318 pedal_untimed=0 pedal_age_ms=14.7 pedal_age_max_ms=88.1 pedal_rtt_ms=2.6 pedal_recovered=0 pedal_lost=0 frame_ms=13.88
event,xr_time_ns,block,latency_ms,index,source_time_ns
```

| Header field | Meaning |
|---|---|
| `audio_timed` | 1 when beats were pinned to audio samples |
| `clock_exact` | 1 when the runtime's own clock conversion was available. It was not on this headset |
| `tap_source` | `pedal`, `controller`, `mixed` or `none` |
| `pedal_untimed` | pedal taps that had to use the packet's arrival time |
| `pedal_age_ms` | median time from the foot going down to the frame that acted on it |
| `pedal_rtt_ms` | median round trip behind the clock offset. Half of it is the most a tap time can be off by |
| `pedal_recovered`, `pedal_lost` | presses rebuilt after a lost packet, and presses that never arrived |
| `frame_ms` | the display's frame period as the headset actually ran it |

Files from before the final release stop at `clock_exact`. Their taps came from the
controller.

`event` is one of `beat`, `tap`, `feedback`, `rating`, `block_start`, `block_end`, `tlx`,
`rank`. `xr_time_ns` is when it happened. `source_time_ns` is the display time of the frame
that handled it.

Most rows use `block` for the block number and `index` for the beat number or rating value.
The two questionnaire rows reuse those columns differently:

| event | `block` | `index` |
|---|---|---|
| `tlx` | dimension, 0 to 5 in the order mental, physical, temporal, performance, effort, frustration | the 1 to 7 answer |
| `rank` | place, 1 being most responsive | the block number chosen |

---

## Tests

`tests/` checks the parts that cannot be checked by eye. It needs Python 3, a C++ compiler,
and `pip install gpiozero` (used with its built-in fake pins, so no Pi is needed).

```
cd tests
bash run_tests.sh
```

| Test | What it proves |
|---|---|
| `test_pedal.py` | Bounce, electrical spikes, chatter under a resting foot, a pedal wired the other way round, and a dropped edge all come out as the right presses, each stamped at its first edge |
| `test_link.py` | The real relay and the real headset receiver, with a fake WiFi between them and the relay's clock set wrong by 16 minutes. Press times come back right to 0.1 ms on a clean link and about 2 ms with 15% of packets dropped. `link_results.json` is the recorded run the report quotes; each new run writes its own figures to `link_results_latest.json` |
| `exp/sim.cpp` | A whole session replayed through the experiment logic. With the controller as input, the release build logs the same events, row for row, as the build that recorded P01 and P02 |

---

## Known limitations

- **The pipeline cannot answer a tap faster than about 63 ms** (62 on the pedal, where it
  also varies a little from tap to tap). Measured, not assumed, and now on two different
  inputs. The two shortest conditions are the same stimulus, and the five nominal conditions
  are four distinguishable levels. Respacing them above the floor is the next change to make.
- **That floor has not been checked against outside hardware.** It is what the audio stream
  reports about itself. High-speed video of the foot and the speaker is the next step.
- **Three participants, on two inputs.** P01 and P02 used the controller and P03 the pedal.
  Spread and ratings can be compared across them. How early each person tapped cannot.
- **Practice is too short.** Two of three participants had their worst block first.
- **The display rate is not fixed.** The app records the rate the headset chose but does not
  set one.
- **The pedal clicks.** A metal foot switch makes its own sound the instant it closes, with
  no delay. A study run on it should mask that or account for it.
- **The pedal needs both devices on one network** that lets them talk to each other.
- The condition order rotates, which balances position but not carryover. A Williams design
  is the correct structure.
- NASA-TLX is a 7-point unweighted scale and not the published 21-point weighted
  instrument, because the renderer draws letters out of boxes. Performance keeps the original
  direction, good to poor, so it is reverse scored against the other five.
- Sessions must run on the headset's own speakers.

The full discussion is in the report.  Claude Code was used to help with implementation details for my design and research.

---

## References

Bridges, D., Pitiot, A., MacAskill, M. R., & Peirce, J. W. (2020). The timing mega-study.
*PeerJ*, 8, e9414. https://doi.org/10.7717/peerj.9414

Friston, S., Karlström, P., & Steed, A. (2016). The Effects of Low Latency on Pointing and
Steering Tasks. *IEEE TVCG*, 22(5), 1605-1615. https://doi.org/10.1109/TVCG.2015.2446467

Hart, S. G., & Staveland, L. E. (1988). Development of NASA-TLX (Task Load Index). *Advances
in Psychology*, 52, 139-183. https://doi.org/10.1016/S0166-4115(08)62386-9

MacKenzie, I. S., & Ware, C. (1993). Lag as a Determinant of Human Performance in Interactive
Systems. *INTERACT '93 / CHI '93*, 488-493. https://doi.org/10.1145/169059.169431

Repp, B. H. (2005). Sensorimotor synchronization: A review of the tapping literature.
*Psychonomic Bulletin & Review*, 12(6), 969-992. https://doi.org/10.3758/BF03206433

Waltemate, T., et al. (2016). The Impact of Latency on Perceptual Judgments and Motor
Performance in Closed-Loop Interaction in Virtual Reality. *VRST '16*, 27-35.
https://doi.org/10.1145/2993369.2993381
