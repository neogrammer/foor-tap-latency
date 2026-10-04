# Integration guide

Six header files, no new .cpp, one word added to CMakeLists. Drop them next to
`main.cpp` in `app/src/main/cpp/` and the includes resolve.

```
app/src/main/cpp/TapLogger.h
app/src/main/cpp/AudioEngine.h
app/src/main/cpp/XrClock.h
app/src/main/cpp/XRInput.h
app/src/main/cpp/HandTracking.h
app/src/main/cpp/TextRenderer.h
app/src/main/cpp/HandPointer.h
app/src/main/cpp/TapExperiment.h
```

**Run the session on the headset's own speakers.** A Bluetooth headset adds a
hundred milliseconds or more of output latency, which is larger than four of your
five conditions and would swamp the entire experiment.

**Before you touch anything: build the project unchanged and get it on the
headset.** If the August build is broken, that is the whole weekend and nothing
below matters. Find that out first.

Everything here has a kill switch. If hand tracking misbehaves, set
`kEnableHands = false` and you still have a working experiment.

---

## Edit 1 — includes

Find the Wi-Fi include near the top of `main.cpp`:

```cpp
#include "network/FootDataReceiver.h"
#include <memory>

#include "logging/HeadMotionLogger.h"
```

Replace with:

```cpp
#include <memory>
#include <string>

#include "TapLogger.h"
#include "AudioEngine.h"
#include "XrClock.h"
#include "XRInput.h"
#include "HandTracking.h"
#include "TextRenderer.h"
#include "HandPointer.h"
#include "TapExperiment.h"
```

Include order matters: `TapExperiment.h` refers to `AudioEngine` and `XrClock`.

`HeadMotionLogger` is no longer used. Leave the file on disk; it is where
`TapLogger` came from.

---

## Edit 2 — drop the Wi-Fi member

Find:

```cpp
    struct RenderLayerInfo;
    std::unique_ptr<FootDataReceiver> m_footReceiver;
```

Replace with:

```cpp
    struct RenderLayerInfo;
```

---

## Edit 3 — request the extensions

In `CreateInstance()`, find:

```cpp
        m_instanceExtensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
```

Add below it:

```cpp
        // Optional. The loop below logs and continues if a runtime lacks either,
        // and HandTracking::Init() returns false rather than failing the app.
#ifdef XR_EXT_HAND_TRACKING_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
#endif
#ifdef XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
#endif
        // Converts XrTime to and from CLOCK_MONOTONIC, which is how the audio
        // clock and the runtime clock are made to agree. Without it the code
        // falls back to a measured offset and says so in the CSV header.
#ifdef XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
#endif
        // The runtime's own pointing ray and pinch detection. Without this the
        // code derives a ray from joint positions, which swings away the moment
        // the index finger curls to pinch.
#ifdef XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
#endif
```

The existing loop already logs any extension the runtime does not offer and keeps
going, so a missing one is not fatal.

---

## Edit 4 — members

Find:

```cpp
    float m_viewHeightM = 1.5f;
    float m_strikeFlash = 0.0f;
```

Replace with:

```cpp
    float m_viewHeightM = 1.5f;

    static constexpr bool kEnableHands = true;   // kill switch
    static constexpr bool kEnableAudio = true;   // kill switch
    XRInput       m_input;
    HandTracking  m_hands;
    HandPointer   m_pointer;
    XrTime        m_lastFrameTime = 0;   // dwell timer
    AudioEngine   m_audio;
    XrClock       m_clock;
    TapExperiment m_exp;
    std::string   m_participantId  = "P01";
    int           m_participantNum = 1;          // drives the Latin square
    bool          m_wroteCSV = false;
```

Also delete these two lines further down, they belonged to the Wi-Fi build:

```cpp
    XrSpace          m_viewSpace = XR_NULL_HANDLE;
    HeadMotionLogger m_headLog;
```

and remove the `m_viewSpace` creation and destruction in `CreateReferenceSpace()`
and `DestroyReferenceSpace()`, plus the `m_headLog.Record(...)` block at the top of
`RenderFrame()`.

---

## Edit 5 — initialise, and write the CSV on the way out

In `Run()`, find:

```cpp
        static constexpr bool kEnableFootReceiver = false;

        if (kEnableFootReceiver)
        {

            m_footReceiver = std::make_unique<FootDataReceiver>();
            m_footReceiver->start(9999);  // Listen on port 9999

        }
```

Replace with:

```cpp
        if (!m_input.Init(m_xrInstance, m_session)) {
            XR_TUT_LOG_ERROR("Input init failed. No taps will be recorded.");
        }
        if (kEnableHands && m_hands.Init(m_xrInstance, m_session)) {
            m_hands.EnableSimultaneous();
            XR_TUT_LOG("Hands available. Simultaneous mode: "
                       << (m_hands.Simultaneous() ? "on" : "off"));
        }

        m_clock.Init(m_xrInstance);
        XR_TUT_LOG("Clock conversion: " << (m_clock.Exact() ? "exact (KHR)" : "measured offset"));
        if (kEnableAudio && m_audio.Init()) {
            m_exp.SetAudio(&m_audio, &m_clock);
            XR_TUT_LOG("Audio up at " << m_audio.SampleRate() << " Hz");
        } else {
            XR_TUT_LOG_ERROR("Audio unavailable. Falling back to a visual beat, "
                             "which is quantised to the display frame.");
        }

        m_exp.Init(m_participantId, m_participantNum);
```

Then find the shutdown block:

```cpp
        if (m_footReceiver) {
            m_footReceiver->stop();
        }
        if (androidApp && androidApp->activity->externalDataPath) {
            std::string out = std::string(androidApp->activity->externalDataPath) + "/head_motion.csv";
            if (m_headLog.WriteCSV(out)) {
                XR_TUT_LOG("Wrote " << m_headLog.Count() << " head samples to " << out);
            } else {
                XR_TUT_LOG_ERROR("Failed to write head motion CSV to " << out);
            }
        }
```

Replace with:

```cpp
        WriteSessionCSV();
        m_audio.Destroy();
        m_hands.Destroy();
        m_input.Destroy();
```

and add this method to the class:

```cpp
    void WriteSessionCSV() {
        if (m_wroteCSV) return;
        if (!androidApp || !androidApp->activity->externalDataPath) return;
        std::string out = std::string(androidApp->activity->externalDataPath)
                        + "/taps_" + m_participantId + ".csv";
        if (m_exp.WriteCSV(out)) {
            m_wroteCSV = true;
            XR_TUT_LOG("Wrote " << m_exp.EventCount() << " events to " << out);
        } else {
            XR_TUT_LOG_ERROR("Failed to write tap CSV to " << out);
        }
    }
```

---

## Edit 6 — drive the experiment each frame

In `RenderFrame()`, immediately after the `xrWaitFrame` call and its
`OPENXR_CHECK`, insert:

```cpp
        if (m_sessionRunning) {
            m_clock.Calibrate(frameState.predictedDisplayTime);
            m_input.Sync(frameState.predictedDisplayTime);
            if (m_hands.Available()) {
                m_hands.Update(m_localSpace, frameState.predictedDisplayTime);
            }
            // Point and HOLD drives the dialogs. Dwell rather than pinch,
            // because pinching curls the index finger and swings the aim ray
            // away in the exact moment of the click. The held controller's
            // trigger stays wired as a fallback.
            const float dt = (m_lastFrameTime > 0)
                ? (float)((frameState.predictedDisplayTime - m_lastFrameTime) / 1000000LL) / 1000.0f
                : 0.0f;
            m_lastFrameTime = frameState.predictedDisplayTime;

            m_pointer.Update(m_hands, kTextZ);
            const float panelCY    = -m_viewHeightM + 1.52f;
            const float ratingRowY = -m_viewHeightM + 1.53f;

            int target = PanelUI::kTargetNone;
            {
                const HandPointer::State &p = m_pointer.Get();
                if (p.active) {
                    if (PanelUI::InGo(p.x, p.y, panelCY)) {
                        target = PanelUI::kTargetGo;
                    } else {
                        const int d = PanelUI::DigitAt(p.x, p.y, ratingRowY);
                        if (d) target = PanelUI::TargetDigit(d);
                    }
                }
            }
            m_pointer.Dwell(target, dt);

            const HandPointer::State &ptr = m_pointer.Get();
            bool go   = m_input.Confirm().justPressed;
            int  pick = 0;
            if (ptr.fired) {
                if (ptr.target == PanelUI::kTargetGo) go = true;
                else {
                    const int d = PanelUI::DigitOfTarget(ptr.target);
                    if (d) pick = d;
                }
            }

            m_exp.Update(frameState.predictedDisplayTime,
                         m_input.Tap().justPressed, m_input.Tap().changeTime,
                         go, pick);
            // Flush as soon as the session ends so a crash later costs nothing.
            if (m_exp.Finished()) WriteSessionCSV();
        }
```

Sync must happen once per frame, before anything reads input state.

---

## Edit 7 — replace the scene

In `RenderLayer()`, find and delete this block, which reads the Wi-Fi state:

```cpp
        FootState footState{};
        uint32_t  strikes = 0;
        if (m_footReceiver) {
            footState = m_footReceiver->getLatestState();
            strikes   = m_footReceiver->consumeStrikes();
        }
        if (strikes > 0) m_strikeFlash = 1.0f;
        else             m_strikeFlash *= 0.80f;
```

Replace with:

```cpp
        const TapExperiment::RenderState rs = m_exp.Render();
```

Then, inside the per-eye loop, find everything from the floor draw down to the
strike lamp, which is this run of code:

```cpp
            renderCuboidIndex = 0;
            RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, -m_viewHeightM, 0.0f}}, {2.0f, 0.1f, 2.0f}, {0.4f, 0.5f, 0.5f});
```

...through the end of the strike lamp draw, and replace the whole run with:

```cpp
            renderCuboidIndex = 0;
            DrawScene(rs, m_pointer.Get());
```

Then add `DrawScene` and its helpers to the class:

```cpp
    static XrVector3f Accent() { return {0.25f, 0.55f, 0.95f}; }
    static XrVector3f Green()  { return {0.20f, 0.78f, 0.45f}; }
    static XrVector3f Dim()    { return {0.26f, 0.27f, 0.31f}; }
    static XrVector3f Ink()    { return {0.72f, 0.78f, 0.88f}; }

    // Text sits just in front of the panel.
    static constexpr float kTextZ = -1.26f;

    void Say(const char *s, float y, float h, XrVector3f c) {
        StrokeText::Draw(s, 0.0f, y, kTextZ, h, c,
            [&](XrPosef p, XrVector3f sc, XrVector3f col) { RenderCuboid(p, sc, col); });
    }

    void Pip(float x, float y, XrVector3f c) {
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f}, {x, y, -1.27f}},
                     {0.075f, 0.075f, 0.02f}, c);
    }

    // Everything is a box, including the letters. One pipeline, no textures.
    void DrawScene(const TapExperiment::RenderState &rs) {
        using Phase = TapExperiment::Phase;
        const float floorY = -m_viewHeightM;
        const XrQuaternionf noRot{0.0f, 0.0f, 0.0f, 1.0f};
        char buf[40];

        // Floor
        RenderCuboid({noRot, {0.0f, floorY, 0.0f}}, {3.0f, 0.08f, 3.0f},
                     {0.22f, 0.24f, 0.27f});

        // Target pad. Where the controller goes. Lit the whole session so the
        // participant always knows where the foot belongs.
        {
            XrVector3f c = {0.30f, 0.34f, 0.40f};
            if (rs.feedbackFlash > 0.02f) {
                c = {0.20f + 0.80f * rs.feedbackFlash,
                     0.34f + 0.30f * rs.feedbackFlash,
                     0.40f - 0.25f * rs.feedbackFlash};
            }
            RenderCuboid({noRot, {0.0f, floorY + 0.06f, -0.55f}},
                         {0.34f, 0.03f, 0.34f}, c);
        }

        // Beat indicator. Never delayed. This is the timing reference.
        {
            const float sz = 0.16f + 0.10f * rs.beatPulse;
            RenderCuboid({noRot, {0.0f, floorY + 1.05f, -1.15f}}, {sz, sz, sz},
                         {0.25f + 0.60f * rs.beatPulse,
                          0.55f + 0.35f * rs.beatPulse, 0.95f});
        }

        // Dialog panel
        RenderCuboid({noRot, {0.0f, floorY + 1.52f, -1.30f}},
                     {1.05f, 0.46f, 0.03f}, {0.14f, 0.15f, 0.18f});

        const float L1 = floorY + 1.63f;   // upper line
        const float L2 = floorY + 1.53f;   // middle line
        const float L3 = floorY + 1.42f;   // lower line, usually the prompt

        switch (rs.phase) {

        case Phase::Diagnostic:
            // The one unknown in the design, answerable in under a minute.
            Say("STEP ON PEDAL",    L1, 0.055f, Ink());
            Say("TRIGGER TO START", L3, 0.042f, Accent());
            Pip(-0.30f, L2, rs.leftPressed  ? Green() : Dim());
            Pip(-0.10f, L2, rs.rightPressed ? Green() : Dim());
            Pip( 0.10f, L2, rs.handsActive  ? Green() : Dim());
            Pip( 0.30f, L2, rs.audioReady   ? Green() : Dim());
            break;

        case Phase::Welcome:
            Say("TAP WITH THE BEAT", L1, 0.055f, Ink());
            Say("5 BLOCKS",          L2, 0.042f, Dim());
            Say("TRIGGER TO BEGIN",  L3, 0.042f, Accent());
            break;

        case Phase::PracticeIntro:
            Say("PRACTICE",     L1, 0.055f, Ink());
            Say("NOT RECORDED", L2, 0.042f, Dim());
            Say("TRIGGER",      L3, 0.042f, Accent());
            break;

        case Phase::Practice:
            Say("PRACTICE", L1, 0.050f, Dim());
            break;

        case Phase::ReadyCheck:
            snprintf(buf, sizeof(buf), "BLOCK %d OF %d", rs.blockNumber + 1, rs.blockTotal);
            Say(buf,              L1, 0.055f, Ink());
            Say("TAP EVERY BEAT", L2, 0.042f, Dim());
            Say("TRIGGER",        L3, 0.042f, Accent());
            break;

        case Phase::Countdown:
            snprintf(buf, sizeof(buf), "%d", rs.countdown);
            Say(buf, L2, 0.150f, Accent());
            break;

        case Phase::Block: {
            snprintf(buf, sizeof(buf), "BLOCK %d OF %d", rs.blockNumber, rs.blockTotal);
            Say(buf, L1, 0.045f, Dim());
            const int filled = (rs.tapsTarget > 0)
                ? (rs.tapsThisBlock * 10) / rs.tapsTarget : 0;
            for (int i = 0; i < 10; i++)
                Pip(-0.315f + 0.07f * i, L3, (i < filled) ? Green() : Dim());
            break;
        }

        case Phase::BlockComplete:
            snprintf(buf, sizeof(buf), "BLOCK %d DONE", rs.blockNumber);
            Say(buf,       L1, 0.055f, Green());
            Say("TRIGGER", L3, 0.042f, Accent());
            break;

        case Phase::Rating:
            // Seven digits, the chosen one in accent. A and B step it.
            Say("HOW RESPONSIVE?", L1, 0.050f, Ink());
            for (int v = 1; v <= 7; v++) {
                snprintf(buf, sizeof(buf), "%d", v);
                StrokeText::Draw(buf, -0.30f + 0.10f * (v - 1), L2, kTextZ, 0.055f,
                                 (v == rs.rating) ? Accent() : Dim(),
                    [&](XrPosef p, XrVector3f sc, XrVector3f col) {
                        RenderCuboid(p, sc, col);
                    });
            }
            Say("A B CHANGE", L3, 0.036f, Dim());
            break;

        case Phase::Rest:
            Say("REST",      L1, 0.055f, Ink());
            Say("TRIGGER",   L3, 0.042f, Accent());
            break;

        case Phase::Done:
            Say("SESSION COMPLETE", L1, 0.050f, Green());
            Say("THANK YOU",        L2, 0.042f, Ink());
            break;
        }

        // Skeleton hands. So the participant can see their hands while operating
        // the dialogs. Never used for timing.
        if (m_hands.Available()) {
            for (int h = 0; h < 2; h++) {
                const HandTracking::Hand &hand = m_hands.GetHand(h);
                if (!hand.active) continue;
                for (int j = 0; j < hand.count; j++) {
                    const HandTracking::Joint &jt = hand.joints[j];
                    if (!jt.valid) continue;
                    const float sz = (jt.radius > 0.0f) ? jt.radius * 2.0f : 0.012f;
                    RenderCuboid({noRot, {jt.x, jt.y, jt.z}}, {sz, sz, sz},
                                 {0.85f, 0.80f, 0.72f});
                }
            }
        }
    }
```

**Draw-call budget.** Every letter stroke is one `RenderCuboid`, and your
`RenderCuboid` rebinds and updates descriptors each time. The heaviest screen is
the rating dialog, at roughly 120 boxes of text plus the scene and both hands,
about 180 per eye. That is what the numbers above were chosen against. If frame
rate drops, shorten the strings before anything else; `StrokeText::StrokeCount()`
tells you what a line costs without running it.

You also need the diagnostic flags filled in. In `RenderFrame`, after
`m_exp.Update(...)`, the `RenderState` is built inside the experiment, which does
not know about input. Simplest fix: set them on the copy in `RenderLayer`, right
after you take it:

```cpp
        TapExperiment::RenderState rs = m_exp.Render();
        rs.leftPressed  = m_input.AnyLeftPressed();
        rs.rightPressed = m_input.AnyRightPressed();
        rs.handsActive  = m_hands.Available() &&
                          (m_hands.GetHand(0).active || m_hands.GetHand(1).active);
        rs.simultaneous = m_hands.Simultaneous();
```

The diagnostic panel draws a fourth pip for audio, so you can see at a glance
whether the beat is sample-timed or frame-timed before you collect anything.

(drop the `const` from the earlier line if you use this.)

---

## Edit 8 — the one that will crash you if you skip it

In `CreateResources()`, find:

```cpp
        size_t numberOfCuboids = 4;
```

Change to:

```cpp
        // Scene + pips + 52 hand joints + up to ~120 letter strokes, with
        // headroom. The uniform buffer is indexed per cuboid per eye, so
        // undersizing this is the bug that bit the Wi-Fi build in August.
        size_t numberOfCuboids = 256;
```

---

## Edit 9 — CMakeLists, one word

Find your `target_link_libraries` block and add `aaudio` to it. It is part of the
NDK, so there is nothing to download:

```cmake
target_link_libraries(
        ${PROJECT_NAME}
        ...existing entries...
        aaudio
)
```

If your `minSdkVersion` is below 26, raise it to 26. AAudio needs it, and Quest 3
is far above that anyway.

---

## Edit 10 — manifest and dead files

Remove `<uses-permission android:name="android.permission.INTERNET" />` from
`AndroidManifest.xml`. Nothing talks to the network any more.

You can delete `network/FootDataReceiver.h` and `.cpp` from `CMakeLists.txt` and
from disk, or leave them unreferenced. Leaving them is faster and harmless.

---

## Controls

| Input | What it does |
|---|---|
| Floor controller, any button | Register a tap |
| Point at GO and hold ~1 s | Advance to the next dialog |
| Point at a digit and hold | Select that rating, then hold GO to commit |
| Pinch while pointing | Same, but instant. Optional shortcut |
| Held controller trigger | Same as GO. Fallback only, in case hands drop out |

Only one controller is needed now. It lies on the floor, and the hands do
everything else.

Which controller counts as the floor one defaults to the left. Change it with
`m_input.SetFloorHandLeft(false)` after init.

**Why dwell and not pinch.** The aim ray runs from the index knuckle through the
fingertip, which is accurate while the finger is extended. Pinching curls it, so
the ray swings away in the exact moment of the click. Latching the hit point does
not rescue it, because the finger starts moving long before the thumb-to-index
distance crosses any threshold, so whatever you latched is already wrong.

Dwell removes the problem rather than compensating for it. The finger never
leaves the pointing pose, so the ray that aimed is the ray that fires. It is
slower than a pinch and it never misses, which is the right trade for an
instrument a participant uses once.

A pinch still fires instantly for anyone who prefers it, using a time-based latch
that reaches about 220 ms back past the curl. Nothing depends on it working.

`XR_FB_hand_tracking_aim` is used for the ray when the runtime offers it. If it
is missing, the joint-derived ray is used instead, and with dwell that is fine.

Tune the hold with `kDwellSeconds` in `HandPointer.h`, currently 1.0 s.

---

## First run: answer the open question

The app opens on the **diagnostic screen**, and that screen is the point of this
whole build.

The panel reads STEP ON PEDAL, with four pips to the right of centre. The first
two light when anything on the left or right controller is pressed. The last two
show hand tracking and audio. A GO button sits in the bottom-left corner; point
at it with an index finger and pinch to continue.

**Check the audio pip before anything else.** Lit means beats are pinned to audio
samples and the timing is sample-accurate. Dark means the build fell back to a
visual beat quantised to the display frame, which is a materially weaker
measurement. The CSV header records which one produced the file, so check it there
too before you analyse anything.

Put one controller on the floor, step on it, and watch.

- **Pip lights up:** a controller on the floor reports button presses even when
  the runtime thinks it is not in hand. The whole design works. Press the trigger
  on the held controller to start the session.
- **Pip does not light:** that is the finding, and it is worth more than a clean
  run. It means the contraption plan needs rethinking, and it belongs in the alpha
  document under technical challenges. Fall back to tapping a held controller with
  your hand just to prove the pipeline end to end.

Either outcome is a result. Take a screenshot of this screen either way.

---

## Getting the data off the headset

The CSV lands in the app's external data directory and is written when the session
reaches Done, and again on exit.

```
adb shell ls /sdcard/Android/data/<your.package.name>/files/
adb pull /sdcard/Android/data/<your.package.name>/files/taps_P01.csv
```

Columns: `event, xr_time_ns, block, latency_ms, index, source_time_ns`.

The header line carries `audio_timed` and `clock_exact`. `audio_timed=1` means the
beats were pinned to audio samples; `0` means they fell back to display frames and
carry about 14 ms of quantisation. Read that line before trusting the numbers.

Rows are `beat`, `tap`, `feedback`, `rating`, `block_start`, `block_end`. Asynchrony
is derived offline by matching each tap to its nearest beat in the same block.
Nothing is pre-computed on purpose, so a mistake in the matching rule is fixable
without re-running a participant.

---

## What is deliberately not here

**Audio.** A metronome in native C++ on Quest means Oboe or AAudio, a new
dependency sitting directly on the timing-critical path. The beat is visual
instead. Visual synchronization is a real paradigm with known worse precision than
auditory, and that goes in the limitations and the beta plan.

**Textured text.** Letters are drawn as stroke outlines from the same cuboid as
everything else, because the pipeline has one vertex attribute and no sampler.
That keeps it to one pipeline and no new failure modes, at the cost of a draw call
per stroke. Real glyph textures would be cheaper per character and allow lowercase
and longer lines; that is a beta question, and the first thing to check is whether
your `GraphicsAPI` already exposes `CreateImage` and a `DescriptorInfo::Type::IMAGE`.

**NASA-TLX and the final ranking.** Cards 9 and 11 from the paper prototype. Both
are now possible, since there is text. They are beta work because they are more
dialog states, not because anything blocks them.
