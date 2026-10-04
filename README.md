# Foot Tap Latency Study

How much input lag can foot tapping tolerate in VR before timing performance falls apart,
and before the user notices?

Justin Hardee. CSCI-7420 Human-Computer Interaction, Fall 2026, Augusta University.
Milestone 3, alpha release.

---

## Layout

```
Chapter2/            the Quest app, an Android Studio project
Common/              shared scaffold from the Khronos OpenXR tutorial
Shaders/             shader sources, used by the scaffold
cmake/               build helpers, used by the scaffold
analysis.html        the analysis and charting page, open it in a browser
data/                a real recorded session, for loading into the page
sample_session.csv   synthetic data, for checking the charts without a headset
```

`Chapter2`, `Common`, `Shaders` and `cmake` all come from the Khronos tutorial and all four
are needed for the build. My own work is the nine files listed further down.

## What this is

Two programs with a file between them.

**`Chapter2/`** is the headset app. Native OpenXR and C++ on a Meta Quest 3, built on the
Khronos OpenXR tutorial project with Vulkan. A controller lies flat on the floor and the
participant taps it with their foot. A metronome beeps and a light pulses with it. The app
then waits a set amount of time before flashing a pad on the floor to confirm the tap. That
wait is the thing being studied: 0, 50, 100, 200 or 400 ms, one value per block. Every beat,
tap and confirmation is written to a CSV with a nanosecond timestamp.

Hand tracking runs at the same time as the controller, because the only controller is on the
floor under a foot. The menus are driven by pointing a finger and holding still for one
second.

**`analysis.html`** reads the CSV and charts it. It pairs each tap with its nearest beat,
works out how consistent the tapping was, and plots that against lag alongside the
participant's own responsiveness ratings. It also runs the whole experiment from the keyboard
as a desktop pilot, which is how the measures and the chart code were checked before the
headset work started.

Nothing is calculated inside the headset. The device logs raw events only, so a mistake in
the pairing rule is fixed by editing the page rather than by asking a participant to come
back.

---

## Setup: the analysis page

No build step and no dependencies.

1. Download `analysis.html`.
2. Open it in any desktop browser (double-click it, or drag it into a browser window).
3. Click **Load CSV** and pick `data/session_P01.csv`. That is a real recorded session from
   the headset, so the charts that come up are the actual result.

You can also click **Start session** to run the whole experiment on the keyboard, or load
`sample_session.csv`, which is synthetic and exists only for checking the chart code.

A banner appears above the results if the session ran on the fallback timing path. The
included session shows one, because the clock conversion extension did not load on my
headset. The limitations section at the bottom explains what that costs.

---

## Setup: the Quest app

You need Android Studio, the Android NDK, and a Meta Quest 3 in developer mode.

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
| `TapExperiment.h` | The whole study: block order, lead-in, countdown, five lag conditions, ratings |
| `TapLogger.h` | CSV event log, one row per beat, tap, confirmation, rating and block boundary |
| `AudioEngine.h` | AAudio stream, click scheduling pinned to exact audio samples |
| `XrClock.h` | Converts between the audio clock and the runtime clock |
| `XRInput.h` | Action set and bindings, and the subaction-path split between the floor controller and the held one |
| `HandTracking.h` | Hand joints and the aim ray |
| `HandPointer.h` | Point-and-hold selection, plus the panel button layout |
| `TextRenderer.h` | Stroke font. Letters built from thin rotated cuboids |
| `main.cpp` | The tutorial file, heavily modified. The scene drawing and the per-frame loop are mine |

`INTEGRATION.md` documents every change to `main.cpp` and what each header depends on.

### Running a session

The app opens on a check screen with four lights: left controller, right controller, hand
tracking, audio. Put a controller flat on the floor, step on it, and watch the light. That
answers the one question the whole design rests on, which is whether a controller the runtime
no longer considers held still reports button presses. It does.

Point a finger at the **GO** button and hold still for a second to advance. Every screen that
waits for you has GO in the same corner.

Then: practice block at zero lag, then five scored blocks, with a 1 to 7 responsiveness
rating after each one. Tap in time with the beat. Ignore what the floor pad does, since that
is the lag being tested.

### Getting the data off

The CSV is written when the session finishes and again on exit.

```
adb pull /sdcard/Android/data/com.example.vulkanxray/files/taps_P01.csv
```

The participant number in the filename comes from the number set in `TapExperiment.h`, which also sets the condition order.

---

## Conditions and measures

Five lag settings: 0, 50, 100, 200 and 400 ms. They bracket MacKenzie and Ware's range and
straddle Waltemate's 75 ms whole-body threshold. Order rotates by participant number. Each
block opens with four unscored lead-in beats.

Measures: the signed gap between each tap and its beat, the spread of those gaps, hit rate
inside a window, and a seven-point responsiveness rating per block.

---

## How the timing works

The display runs at 72 Hz, so one frame is about 14 ms and the smallest non-zero setting is
50 ms. Three choices keep both ends of the subtraction finer than a frame.

- The beep is scheduled at an exact audio sample. AAudio returns a matching pair of audio
  frame position and system clock time, so a sample scheduled in the future has a known
  arrival time, and that gets converted into the runtime's clock. The beat is logged at the
  moment it will be heard.
- Taps are timestamped from `XrActionStateBoolean::lastChangeTime`, the runtime's own record
  of when the button changed, not the frame it was noticed on.
- The delayed confirmation is pinned to a sample the same way, so a 50 ms setting delivers
  50 ms instead of 50 ms rounded up to the next frame.

If audio will not open, all of this falls back to frame-level timing and the session still
runs. The CSV header records which path ran (`audio_timed=1` and `clock_exact=1` mean the
good path), and the analysis page shows a warning banner when it reads a fallback file.

---

## CSV format

One header line, then one row per event.

```
# participant=P01 beat_interval_ms=600 audio_timed=1 clock_exact=0
event,xr_time_ns,block,latency_ms,index,source_time_ns
```

`event` is one of `beat`, `tap`, `feedback`, `rating`, `block_start`, `block_end`.

---

## Known limitations

- `XR_KHR_convert_timespec_time` did not load on the test headset, so the audio clock and the
  runtime clock are tied together by an estimate that probably logs beats 15 to 30 ms late.
  The CSV header records this as `clock_exact=0`.
- Sessions must run on the headset's own speakers. A Bluetooth headset adds 100 ms or more,
  which is larger than four of the five conditions.
- The condition order rotates, which balances position but not carryover. A Williams design
  is the correct structure and is in the beta plan.
- NASA-TLX and the end-of-session preference ranking are not built yet.

The full list is in the alpha release report.

---

## References

Bridges, D., Pitiot, A., MacAskill, M. R., & Peirce, J. W. (2020). The timing mega-study.
*PeerJ*, 8, e9414. https://doi.org/10.7717/peerj.9414

Friston, S., Karlström, P., & Steed, A. (2016). The Effects of Low Latency on Pointing and
Steering Tasks. *IEEE TVCG*, 22(5), 1605-1615. https://doi.org/10.1109/TVCG.2015.2446467

MacKenzie, I. S., & Ware, C. (1993). Lag as a Determinant of Human Performance in Interactive
Systems. *INTERACT '93 / CHI '93*, 488-493. https://doi.org/10.1145/169059.169431

Repp, B. H. (2005). Sensorimotor synchronization: A review of the tapping literature.
*Psychonomic Bulletin & Review*, 12(6), 969-992. https://doi.org/10.3758/BF03206433

Waltemate, T., et al. (2016). The Impact of Latency on Perceptual Judgments and Motor
Performance in Closed-Loop Interaction in Virtual Reality. *VRST '16*, 27-35.
https://doi.org/10.1145/2993369.2993381
