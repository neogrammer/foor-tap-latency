// Runs the real TapExperiment through a whole session on a desktop, with a stand-in audio
// stream and clock, so the experiment logic can be checked without a headset.
//
// Built twice: once against the app's new TapExperiment.h (-DNEW), once against the untouched
// original. With controller taps the two must log identical events.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

typedef int64_t XrTime;

static int64_t g_nowMono = 0;                        // the simulated CLOCK_MONOTONIC, ns

struct AudioEngine {
    static constexpr int kVoiceBeat = 0, kVoiceLeadIn = 1, kVoiceFeedback = 2;
    static constexpr int64_t kFloorNs = 42000000;    // soonest a click can be heard after "now"
    bool Ready() const { return true; }
    int  SampleRate() const { return 48000; }
    bool MonotonicNsToFrame(int64_t ns, int64_t *f) { *f = (int64_t)((double)ns * 48000.0 / 1e9); return true; }
    bool FrameToMonotonicNs(int64_t f, int64_t *ns) { *ns = (int64_t)((double)f * 1e9 / 48000.0); return true; }
    int64_t ScheduleClick(int64_t frame, int) {
        int64_t floorFrame = 0; MonotonicNsToFrame(g_nowMono + kFloorNs, &floorFrame);
        return frame < floorFrame ? floorFrame : frame;
    }
};

struct XrClock {
    int64_t off = 21000000;                          // display time runs this far ahead of "now"
    int holds = 0, releases = 0; bool held = false;
    bool Exact() const { return false; }
    void Hold(bool on) { if (on && !held) holds++; if (!on && held) releases++; held = on; }
    bool MonotonicToXr(int64_t m, XrTime *o) const { *o = m + off; return true; }
    bool XrToMonotonic(XrTime t, int64_t *o) const { *o = t - off; return true; }
};

#include "TapLogger.h"
#include "TapExperiment.h"

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "controller";     // controller | pedal | mixed
    const std::string out  = argc > 2 ? argv[2] : "out.csv";
    const int participant  = argc > 3 ? atoi(argv[3]) : 1;

    AudioEngine audio; XrClock clock; TapExperiment exp;
    exp.SetAudio(&audio, &clock);
    exp.Init("P01", participant);

    const int64_t frameNs = 11111111;
    const int64_t beatNs  = 600000000;
    srand(3);
    using Phase = TapExperiment::Phase;

    int64_t nextBeatMono = 0; bool inRun = false; int beatsLeft = 0, leadIn = 0, blocks = 0;
    struct Due { int64_t press, deliver; bool pedal; };
    std::vector<Due> due;
    Phase last = Phase::Diagnostic;

    for (int frame = 0; frame < 400000 && !exp.Finished(); frame++) {
        g_nowMono += frameNs;
        const XrTime frameTime = g_nowMono + clock.off;
        TapExperiment::RenderState rs = exp.Render();

        bool go = false; int pick = 0;
        if (rs.awaitingInput && frame % 40 == 0) {                   // a patient participant
            if (rs.phase == Phase::Rating || rs.phase == Phase::Tlx) pick = 1 + (frame / 40) % 7;
            if (rs.phase == Phase::Rank) { for (int i = 0; i < 5; i++) if (!rs.rankTaken[i]) { pick = i + 1; break; } }
            go = (frame % 80 == 0);
        }

        // A run of beats has just started: work out when they will be heard, so the simulated
        // foot can aim 30 ms ahead of each one, the way people do.
        if ((rs.phase == Phase::Practice || rs.phase == Phase::Block) && rs.phase != last) {
            inRun = true;
            const bool practice = rs.phase == Phase::Practice;
            beatsLeft = practice ? 12 : 64; leadIn = practice ? 0 : 4;
            // StartBeats ran inside the previous Update, with now = previous frameTime
            nextBeatMono = (g_nowMono - frameNs) + 700000000;
            if (!practice) blocks++;
        }
        if (inRun && rs.phase != Phase::Practice && rs.phase != Phase::Block) inRun = false;
        last = rs.phase;

        if (inRun && beatsLeft > 0 && g_nowMono >= nextBeatMono - 120000000) {
            const int64_t press = nextBeatMono - 30000000 + (rand() % 20001 - 10000) * 1000LL;
            const bool pedal = (mode == "pedal") || (mode == "mixed" && blocks % 2 == 1);
            // pedal: 3 ms confirm plus 2 to 12 ms of WiFi. controller: seen by the next frame.
            const int64_t lag = pedal ? 3000000 + 2000000 + (rand() % 10001) * 1000LL : 0;
            if (leadIn > 0) leadIn--; else due.push_back({press, press + lag, pedal});
            nextBeatMono += beatNs; beatsLeft--;
        }

        bool ctrl = false; XrTime ctrlTime = 0;
        for (size_t i = 0; i < due.size();) {
            if (g_nowMono >= due[i].deliver) {
                if (due[i].pedal) {
#ifdef NEW
                    XrTime t = 0; clock.MonotonicToXr(due[i].press, &t);
                    exp.PushPedalTap(t, true, 3.0f);
#endif
                } else { ctrl = true; ctrlTime = frameTime - 21000000; }   // what lastChangeTime really is
                due.erase(due.begin() + (long)i);
            } else i++;
        }
        exp.Update(frameTime, ctrl, ctrlTime, go, pick);
    }
    if (!exp.Finished()) { fprintf(stderr, "session never finished\n"); return 1; }
    if (!exp.WriteCSV(out)) { fprintf(stderr, "write failed\n"); return 1; }
#ifdef NEW
    printf("holds=%d releases=%d held_at_end=%d summary: %s\n", clock.holds, clock.releases, clock.held ? 1 : 0, exp.SourceSummary().c_str());
#endif
    return 0;
}
