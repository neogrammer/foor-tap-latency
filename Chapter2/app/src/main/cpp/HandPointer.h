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

    struct State {
        bool  active   = false;
        float x = 0, y = 0;                   // live hit point on the panel plane
        float rayX = 0, rayY = 0, rayZ = 0;   // ray origin, for drawing
        bool  pinching = false;
        int   hand     = -1;

        int   target   = 0;     // what is under the cursor, set by the caller
        float progress = 0;     // 0..1 dwell fill
        bool  fired    = false; // activated this frame, by dwell or by pinch
    };

    // Step 1, once per frame: work out where the hand is pointing.
    void Update(const HandTracking &hands, float planeZ) {
        m_state.active   = false;
        m_state.fired    = false;

        for (int h = 0; h < 2; h++) {
            const HandTracking::Hand &hand = hands.GetHand(h);
            if (!hand.active || hand.count <= kIndexTip) { m_pinched[h] = false; continue; }

            float ox, oy, oz, dx, dy, dz;
            bool  pinchNow = false;

            if (hand.aimValid) {
                // Runtime-supplied ray when the extension is there.
                ox = hand.aimX;  oy = hand.aimY;  oz = hand.aimZ;
                dx = hand.aimDX; dy = hand.aimDY; dz = hand.aimDZ;
                pinchNow = hand.aimPinching;
            } else {
                const HandTracking::Joint &tt = hand.joints[kThumbTip];
                const HandTracking::Joint &ip = hand.joints[kIndexProx];
                const HandTracking::Joint &it = hand.joints[kIndexTip];
                if (!tt.valid || !ip.valid || !it.valid) { m_pinched[h] = false; continue; }

                ox = it.x; oy = it.y; oz = it.z;
                dx = it.x - ip.x; dy = it.y - ip.y; dz = it.z - ip.z;
                const float len = sqrtf(dx*dx + dy*dy + dz*dz);
                if (len < 1e-5f) continue;
                dx /= len; dy /= len; dz /= len;

                const float px = tt.x - it.x, py = tt.y - it.y, pz = tt.z - it.z;
                const float gap = sqrtf(px*px + py*py + pz*pz);
                pinchNow = m_pinched[h] ? (gap < kPinchOff) : (gap < kPinchOn);
            }

            const bool justClosed = (!m_pinched[h] && pinchNow);
            m_pinched[h] = pinchNow;

            bool  hit = false;
            float hx = 0, hy = 0;
            if (dz < -1e-4f) {
                const float t = (planeZ - oz) / dz;
                if (t > 0.0f && t < 20.0f) { hx = ox + dx * t; hy = oy + dy * t; hit = true; }
            }
            if (!hit) continue;

            // Ring of recent hit points, so a pinch can reach back past the curl.
            m_hx[h][m_head[h]] = hx;
            m_hy[h][m_head[h]] = hy;
            if (m_filled[h] < kHistory) m_filled[h]++;
            m_head[h] = (m_head[h] + 1) % kHistory;

            if (!m_state.active || justClosed) {
                m_state.active   = true;
                m_state.hand     = h;
                m_state.x        = hx;
                m_state.y        = hy;
                m_state.rayX     = ox;
                m_state.rayY     = oy;
                m_state.rayZ     = oz;
                m_state.pinching = m_pinched[h];
                if (justClosed) {
                    // Oldest point in the ring: where the finger was aiming
                    // before it started closing.
                    if (m_filled[h] >= kHistory) {
                        m_state.x = m_hx[h][m_head[h]];
                        m_state.y = m_hy[h][m_head[h]];
                    }
                    m_pinchFired = true;
                }
            }
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
constexpr float kPanelH = 0.46f;

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

// Target ids shared between the hit test and the dwell tracker.
constexpr int kTargetNone  = 0;
constexpr int kTargetGo    = 1;
inline int    TargetDigit(int v) { return 10 + v; }
inline int    DigitOfTarget(int t) { return (t > 10 && t <= 17) ? t - 10 : 0; }

} // namespace PanelUI
