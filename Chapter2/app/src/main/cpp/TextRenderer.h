#pragma once

// Stroke text drawn from the cuboid you already render.
//
// No texturing pipeline, no sampler descriptor, no image upload, no new shader.
// Each glyph is a handful of line segments on a 3 x 5 lattice, and each segment
// is one thin box rotated about Z. If RenderCuboid works, this works.
//
// ---------------------------------------------------------------------------
// COST, because it is the thing that will bite you
//
// One box per stroke, and a glyph averages four strokes. A sixteen-character
// line is therefore about sixty-five draw calls. Your RenderCuboid rebinds and
// updates descriptors on every call, so this is not free.
//
// Keep lines SHORT. Two lines of sixteen characters is roughly 130 boxes, which
// with the scene and two hands lands near 200 per eye. That is the budget this
// was written against. If the frame rate drops, shorten the strings first.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstring>

namespace StrokeText {

// Lattice: x in 0..2, y in 0..4. Origin bottom-left. Each entry is a run of
// segments as {x1,y1,x2,y2}, terminated by a 255.
struct Glyph { const unsigned char *seg; int count; };

#define G(name, ...) static const unsigned char name[] = {__VA_ARGS__};

G(g_A, 0,0,0,3,  0,3,1,4,  1,4,2,3,  2,3,2,0,  0,2,2,2)
G(g_B, 0,0,0,4,  0,4,2,3,  2,3,0,2,  0,2,2,1,  2,1,0,0)
G(g_C, 2,4,0,3,  0,3,0,1,  0,1,2,0)
G(g_D, 0,0,0,4,  0,4,2,3,  2,3,2,1,  2,1,0,0)
G(g_E, 2,4,0,4,  0,4,0,0,  0,0,2,0,  0,2,1,2)
G(g_F, 2,4,0,4,  0,4,0,0,  0,2,1,2)
G(g_G, 2,4,0,3,  0,3,0,1,  0,1,2,0,  2,0,2,2,  2,2,1,2)
G(g_H, 0,0,0,4,  2,0,2,4,  0,2,2,2)
G(g_I, 0,4,2,4,  1,4,1,0,  0,0,2,0)
G(g_J, 2,4,2,1,  2,1,1,0,  1,0,0,1)
G(g_K, 0,0,0,4,  2,4,0,2,  0,2,2,0)
G(g_L, 0,4,0,0,  0,0,2,0)
G(g_M, 0,0,0,4,  0,4,1,2,  1,2,2,4,  2,4,2,0)
G(g_N, 0,0,0,4,  0,4,2,0,  2,0,2,4)
G(g_O, 0,1,0,3,  0,3,1,4,  1,4,2,3,  2,3,2,1,  2,1,1,0,  1,0,0,1)
G(g_P, 0,0,0,4,  0,4,2,3,  2,3,0,2)
G(g_Q, 0,1,0,3,  0,3,1,4,  1,4,2,3,  2,3,2,1,  2,1,1,0,  1,0,0,1,  1,1,2,0)
G(g_R, 0,0,0,4,  0,4,2,3,  2,3,0,2,  0,2,2,0)
G(g_S, 2,4,0,3,  0,3,2,1,  2,1,0,0)
G(g_T, 0,4,2,4,  1,4,1,0)
G(g_U, 0,4,0,1,  0,1,1,0,  1,0,2,1,  2,1,2,4)
G(g_V, 0,4,1,0,  1,0,2,4)
G(g_W, 0,4,0,0,  0,0,1,2,  1,2,2,0,  2,0,2,4)
G(g_X, 0,0,2,4,  0,4,2,0)
G(g_Y, 0,4,1,2,  2,4,1,2,  1,2,1,0)
G(g_Z, 0,4,2,4,  2,4,0,0,  0,0,2,0)

G(g_0, 0,1,0,3,  0,3,1,4,  1,4,2,3,  2,3,2,1,  2,1,1,0,  1,0,0,1)
G(g_1, 0,3,1,4,  1,4,1,0,  0,0,2,0)
G(g_2, 0,3,1,4,  1,4,2,3,  2,3,0,0,  0,0,2,0)
G(g_3, 0,4,2,4,  2,4,1,2,  1,2,2,1,  2,1,0,0)
G(g_4, 2,0,2,4,  2,4,0,1,  0,1,2,1)
G(g_5, 2,4,0,4,  0,4,0,2,  0,2,2,1,  2,1,0,0)
G(g_6, 2,4,0,2,  0,2,0,0,  0,0,2,0,  2,0,2,1,  2,1,0,1)
G(g_7, 0,4,2,4,  2,4,0,0)
G(g_8, 0,0,2,0,  2,0,2,2,  2,2,0,2,  0,2,0,0,  0,2,0,4,  0,4,2,4,  2,4,2,2)
G(g_9, 2,0,2,4,  2,4,0,4,  0,4,0,2,  0,2,2,2)

G(g_DOT,   1,0,1,0)
G(g_QM,    0,3,1,4,  1,4,2,3,  2,3,1,2,  1,2,1,1,  1,0,1,0)
G(g_DASH,  0,2,2,2)
G(g_SLASH, 0,0,2,4)
G(g_COLON, 1,1,1,1,  1,3,1,3)
#undef G

inline Glyph Lookup(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
        case 'A': return {g_A, (int)sizeof(g_A)/4};  case 'B': return {g_B, (int)sizeof(g_B)/4};
        case 'C': return {g_C, (int)sizeof(g_C)/4};  case 'D': return {g_D, (int)sizeof(g_D)/4};
        case 'E': return {g_E, (int)sizeof(g_E)/4};  case 'F': return {g_F, (int)sizeof(g_F)/4};
        case 'G': return {g_G, (int)sizeof(g_G)/4};  case 'H': return {g_H, (int)sizeof(g_H)/4};
        case 'I': return {g_I, (int)sizeof(g_I)/4};  case 'J': return {g_J, (int)sizeof(g_J)/4};
        case 'K': return {g_K, (int)sizeof(g_K)/4};  case 'L': return {g_L, (int)sizeof(g_L)/4};
        case 'M': return {g_M, (int)sizeof(g_M)/4};  case 'N': return {g_N, (int)sizeof(g_N)/4};
        case 'O': return {g_O, (int)sizeof(g_O)/4};  case 'P': return {g_P, (int)sizeof(g_P)/4};
        case 'Q': return {g_Q, (int)sizeof(g_Q)/4};  case 'R': return {g_R, (int)sizeof(g_R)/4};
        case 'S': return {g_S, (int)sizeof(g_S)/4};  case 'T': return {g_T, (int)sizeof(g_T)/4};
        case 'U': return {g_U, (int)sizeof(g_U)/4};  case 'V': return {g_V, (int)sizeof(g_V)/4};
        case 'W': return {g_W, (int)sizeof(g_W)/4};  case 'X': return {g_X, (int)sizeof(g_X)/4};
        case 'Y': return {g_Y, (int)sizeof(g_Y)/4};  case 'Z': return {g_Z, (int)sizeof(g_Z)/4};
        case '0': return {g_0, (int)sizeof(g_0)/4};  case '1': return {g_1, (int)sizeof(g_1)/4};
        case '2': return {g_2, (int)sizeof(g_2)/4};  case '3': return {g_3, (int)sizeof(g_3)/4};
        case '4': return {g_4, (int)sizeof(g_4)/4};  case '5': return {g_5, (int)sizeof(g_5)/4};
        case '6': return {g_6, (int)sizeof(g_6)/4};  case '7': return {g_7, (int)sizeof(g_7)/4};
        case '8': return {g_8, (int)sizeof(g_8)/4};  case '9': return {g_9, (int)sizeof(g_9)/4};
        case '.': return {g_DOT,   (int)sizeof(g_DOT)/4};
        case '?': return {g_QM,    (int)sizeof(g_QM)/4};
        case '-': return {g_DASH,  (int)sizeof(g_DASH)/4};
        case '/': return {g_SLASH, (int)sizeof(g_SLASH)/4};
        case ':': return {g_COLON, (int)sizeof(g_COLON)/4};
        default:  return {nullptr, 0};   // space and anything unknown
    }
}

// How many boxes a string will cost. Check this against your budget before
// adding a line, rather than after the frame rate drops.
inline int StrokeCount(const char *s) {
    int n = 0;
    for (; *s; s++) n += Lookup(*s).count;
    return n;
}

inline float Width(const char *s, float height) {
    const float adv = height * 0.72f;
    return (float)strlen(s) * adv - height * 0.22f;
}

// Draw one line, centred on cx. draw(pose, scale, colour) is your RenderCuboid.
//
// Height is the cap height in metres. 0.055 reads comfortably on a panel about
// 1.3 m away; 0.14 is a countdown digit.
template <typename DrawFn>
void Draw(const char *s, float cx, float cy, float z, float height,
          XrVector3f colour, DrawFn draw) {
    const float unit  = height * 0.25f;          // one lattice step
    const float adv   = height * 0.72f;          // pen advance per character
    const float thick = height * 0.10f;
    const float half  = Width(s, height) * 0.5f;

    float penX = cx - half;
    for (const char *p = s; *p; p++, penX += adv) {
        const Glyph g = Lookup(*p);
        if (!g.seg) continue;
        for (int i = 0; i < g.count; i++) {
            const float x1 = penX + (float)g.seg[i*4 + 0] * unit;
            const float y1 = cy   + (float)g.seg[i*4 + 1] * unit - height * 0.5f;
            const float x2 = penX + (float)g.seg[i*4 + 2] * unit;
            const float y2 = cy   + (float)g.seg[i*4 + 3] * unit - height * 0.5f;

            const float dx = x2 - x1, dy = y2 - y1;
            const float len = sqrtf(dx*dx + dy*dy);
            const float ang = (len > 1e-6f) ? atan2f(dy, dx) : 0.0f;
            const float hs  = sinf(ang * 0.5f), hc = cosf(ang * 0.5f);

            XrPosef pose;
            pose.orientation = {0.0f, 0.0f, hs, hc};     // rotation about Z
            pose.position    = {(x1 + x2) * 0.5f, (y1 + y2) * 0.5f, z};
            draw(pose, XrVector3f{len + thick, thick, thick}, colour);
        }
    }
}

} // namespace StrokeText
