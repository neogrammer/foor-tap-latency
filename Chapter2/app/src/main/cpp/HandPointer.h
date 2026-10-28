#pragma once

// Point-and-dwell for the dialog panels. One controller stays on the floor under
// the foot; the hands do the UI.
//
// ---------------------------------------------------------------------------
// WHY DWELL AND NOT PINCH
//
// The aim ray runs from the index knuckle through the fingertip, which is
// accurate as long as the finger is extended. Pinching curls it, so the ray
// swings away in the exact moment you click, and every press lands somewhere
// below where you aimed. Latching the hit point does not rescue it either: the
// finger starts moving long before the thumb-to-index distance crosses any
// threshold, so whatever you latched is already wrong.
//
// Dwell sidesteps all of it. Point at a target, hold for a second, it fires. The
// finger never leaves the pointing pose, so the ray that aimed is the ray that
// clicks. It is slower than a pinch and it never misses, which is the right
// trade for a research instrument a participant uses once.
//
// Pinch is still here as a shortcut for anyone who prefers it, using a
// time-based latch that reaches back past the curl. Nothing depends on it.
// ---------------------------------------------------------------------------

#include <cmath>

class HandPointer {
public:
    static constexpr int kWrist     = 1;
    static constexpr int kThumbTip  = 5;
    static constexpr int kIndexProx = 7;
    static constexpr int kIndexTip  = 10;

    // Hold this long on one target to activate it.
    static constexpr float kDwellSeconds = 1.0f;
    // Grace before a wobble off the target resets progress.
    static constexpr float kForgiveSeconds = 0.25f;

    static constexpr float kPinchOn  = 0.025f;
    static constexpr float kPinchOff = 0.040f;

    // Hit points from this many frames back, which at 72 Hz is about 220 ms,
    // comfortably before a curl begins.
    static constexpr int kHistory = 16;

    // Where one hand is pointing. Both are published so both cursors can be
    // drawn; only the primary one drives the dwell timer.
    struct HandHit {
        bool  hit  = false;
        float x = 0, y = 0;
        float rayX = 0, rayY = 0, rayZ = 0;
    };

    struct State {
        bool  active   = false;
        float x = 0, y = 0;                   // live hit point on the panel plane
        float rayX = 0, rayY = 0, rayZ = 0;   // ray origin, for drawing
        bool  pinching = false;
        int   hand     = -1;

        HandHit hands[2];                     // both, for drawing

        int   target   = 0;     // what is under the cursor, set by the caller
        float progress = 0;     // 0..1 dwell fill
        bool  fired    = false; // activated this frame, by dwell or by pinch
    };

    // Step 1, once per frame: work out where each hand is pointing.
    //
    // Both hands are tracked so either can drive the menus, and both cursors are
    // drawn so it is obvious which hand the interface is listening to. Only one
    // hand at a time owns the dwell timer, and it keeps ownership while it is
    // still pointing at the panel, so raising the other hand mid-hold does not
    // snatch the selection away.
    void Update(const HandTracking &hands, float planeZ) {
        m_state.active = false;
        m_state.fired  = false;
        m_state.hands[0] = HandHit();
        m_state.hands[1] = HandHit();

        bool  anyPinchClosed[2] = {false, false};

        for (int h = 0; h < 2; h++) {
            const HandTracking::Hand &hand = hands.GetHand(h);
            if (!hand.active || hand.count <= kIndexTip) { m_pinched[h] = false; continue; }

            float ox, oy, oz, dx, dy, dz;
            bool  pinchNow = false;

            if (hand.aimValid) {
                // Runtime-supplied ray when the extension is there. This one is
                // already arm-aligned, which is why it is preferred.
                ox = hand.aimX;  oy = hand.aimY;  oz = hand.aimZ;
                dx = hand.aimDX; dy = hand.aimDY; dz = hand.aimDZ;
                pinchNow = hand.aimPinching;
            } else {
                // Fallback ray: WRIST through fingertip, not knuckle through
                // fingertip. The knuckle version points wherever the last bone
                // of the finger happens to be aimed, which sits at roughly a
                // right angle to the forearm and feels like the cursor is
                // hinged off the hand instead of pointed by the arm.
                const HandTracking::Joint &tt = hand.joints[kThumbTip];
                const HandTracking::Joint &wr = hand.joints[kWrist];
                const HandTracking::Joint &it = hand.joints[kIndexTip];
                const HandTracking::Joint &ip = hand.joints[kIndexProx];
                if (!tt.valid || !it.valid) { m_pinched[h] = false; continue; }

                ox = it.x; oy = it.y; oz = it.z;
                if (wr.valid) {
                    dx = it.x - wr.x; dy = it.y - wr.y; dz = it.z - wr.z;
                } else if (ip.valid) {
                    dx = it.x - ip.x; dy = it.y - ip.y; dz = it.z - ip.z;
                } else {
                    m_pinched[h] = false; continue;
                }
                const float len = sqrtf(dx*dx + dy*dy + dz*dz);
                if (len < 1e-5f) { m_pinched[h] = false; continue; }
                dx /= len; dy /= len; dz /= len;

                const float px = tt.x - it.x, py = tt.y - it.y, pz = tt.z - it.z;
                const float gap = sqrtf(px*px + py*py + pz*pz);
                pinchNow = m_pinched[h] ? (gap < kPinchOff) : (gap < kPinchOn);
            }

            anyPinchClosed[h] = (!m_pinched[h] && pinchNow);
            m_pinched[h] = pinchNow;

            bool  hit = false;
            float hx = 0, hy = 0;
            if (dz < -1e-4f) {
                const float t = (planeZ - oz) / dz;
                if (t > 0.0f && t < 20.0f) { hx = ox + dx * t; hy = oy + dy * t; hit = true; }
            }
            if (!hit) continue;

            m_state.hands[h].hit  = true;
            m_state.hands[h].x    = hx;
            m_state.hands[h].y    = hy;
            m_state.hands[h].rayX = ox;
            m_state.hands[h].rayY = oy;
            m_state.hands[h].rayZ = oz;

            // Ring of recent hit points, so a pinch can reach back past the curl.
            m_hx[h][m_head[h]] = hx;
            m_hy[h][m_head[h]] = hy;
            if (m_filled[h] < kHistory) m_filled[h]++;
            m_head[h] = (m_head[h] + 1) % kHistory;
        }

        // Who drives the dwell. Keep the current hand while it is still on the
        // panel; otherwise take whichever one is. A pinch always takes over,
        // since it is a deliberate act.
        int primary = -1;
        if (m_primary >= 0 && m_state.hands[m_primary].hit) primary = m_primary;
        for (int h = 0; h < 2 && primary < 0; h++)
            if (m_state.hands[h].hit) primary = h;
        for (int h = 0; h < 2; h++)
            if (anyPinchClosed[h] && m_state.hands[h].hit) primary = h;

        m_primary = primary;
        if (primary < 0) return;

        m_state.active   = true;
        m_state.hand     = primary;
        m_state.x        = m_state.hands[primary].x;
        m_state.y        = m_state.hands[primary].y;
        m_state.rayX     = m_state.hands[primary].rayX;
        m_state.rayY     = m_state.hands[primary].rayY;
        m_state.rayZ     = m_state.hands[primary].rayZ;
        m_state.pinching = m_pinched[primary];

        if (anyPinchClosed[primary]) {
            if (m_filled[primary] >= kHistory) {
                m_state.x = m_hx[primary][m_head[primary]];
                m_state.y = m_hy[primary][m_head[primary]];
            }
            m_pinchFired = true;
        }
    }

    // Step 2, once per frame: tell it what the cursor is over, and how much time
    // passed. Target ids are the caller's own; 0 means nothing.
    void Dwell(int target, float dt) {
        m_state.target = target;

        if (target == 0 || !m_state.active) {
            // Short grace so a tremor off the edge does not wipe the progress.
            m_offTime += dt;
            if (m_offTime > kForgiveSeconds) { m_held = 0; m_state.progress = 0; m_armed = true; }
            if (m_pinchFired) { m_pinchFired = false; }
            return;
        }
        m_offTime = 0;

        if (target != m_lastTarget) { m_held = 0; m_armed = true; m_lastTarget = target; }

        // A pinch fires immediately, for anyone who prefers it.
        if (m_pinchFired) {
            m_pinchFired = false;
            if (m_armed) { m_state.fired = true; m_armed = false; m_held = 0; }
            m_state.progress = 0;
            return;
        }

        if (!m_armed) return;        // wait until they leave before firing again

        m_held += dt;
        m_state.progress = m_held / kDwellSeconds;
        if (m_state.progress >= 1.0f) {
            m_state.progress = 1.0f;
            m_state.fired    = true;
            m_armed          = false;
            m_held           = 0;
        }
    }

    const State &Get() const { return m_state; }

private:
    State m_state;
    bool  m_pinched[2] = {false, false};
    bool  m_pinchFired = false;

    float m_hx[2][kHistory]{}, m_hy[2][kHistory]{};
    int   m_head[2] = {0, 0};
    int   m_filled[2] = {0, 0};

    int   m_primary    = -1;
    int   m_lastTarget = 0;
    float m_held    = 0;
    float m_offTime = 0;
    bool  m_armed   = true;
};

// ---------------------------------------------------------------------------
// Where the buttons are. Both the renderer and the hit test read these, so the
// thing you see and the thing you can click cannot drift apart.
// ---------------------------------------------------------------------------
namespace PanelUI {

constexpr float kPanelW = 1.05f;
// Was 0.46, which put the bottom text row straight through the GO button, so
// the word LAGGY sat on top of it. Taller panel, GO moves down with it because
// GoY is measured from the bottom edge.
constexpr float kPanelH = 0.60f;

constexpr float kGoW = 0.24f;
constexpr float kGoH = 0.11f;

inline float GoX()              { return -kPanelW * 0.5f + kGoW * 0.5f + 0.045f; }
inline float GoY(float panelCY) { return panelCY - kPanelH * 0.5f + kGoH * 0.5f + 0.045f; }

// Generous beyond the drawn box. Hand aiming is not a mouse.
constexpr float kSlop = 0.030f;

inline bool InGo(float x, float y, float panelCY) {
    return fabsf(x - GoX())        <= kGoW * 0.5f + kSlop
        && fabsf(y - GoY(panelCY)) <= kGoH * 0.5f + kSlop;
}

constexpr float kDigitPitch = 0.10f;
constexpr float kDigitHalf  = 0.050f;   // touching, so no dead gaps

inline float DigitX(int v) { return -0.30f + kDigitPitch * (float)(v - 1); }

inline int DigitAt(float x, float y, float rowY) {
    if (fabsf(y - rowY) > kDigitHalf + kSlop) return 0;
    for (int v = 1; v <= 7; v++)
        if (fabsf(x - DigitX(v)) <= kDigitHalf) return v;
    return 0;
}

// A minus / plus pair, used on the first screen to step the participant number.
// Two targets instead of a row of digits, so the number has no ceiling: the
// study is not limited to however many digits happen to fit on a panel.
constexpr float kStepHalf = 0.075f;
inline float MinusX() { return -0.21f; }
inline float PlusX()  { return  0.21f; }
inline bool InStep(float x, float y, float rowY, float cx) {
    return fabsf(x - cx)    <= kStepHalf + kSlop
        && fabsf(y - rowY)  <= kStepHalf + kSlop;
}

// Target ids shared between the hit test and the dwell tracker.
constexpr int kTargetNone  = 0;
constexpr int kTargetGo    = 1;
constexpr int kTargetMinus = 2;
constexpr int kTargetPlus  = 3;
inline int    TargetDigit(int v) { return 10 + v; }
inline int    DigitOfTarget(int t) { return (t > 10 && t <= 17) ? t - 10 : 0; }

} // namespace PanelUI
