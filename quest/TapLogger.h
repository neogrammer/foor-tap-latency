#pragma once

// Tap and beat logger for the foot-tap latency study.
//
// Same shape as HeadMotionLogger: record into memory during the session, write
// CSV once on shutdown. Nothing is averaged or thresholded here on purpose, so
// the analysis can change its mind later without re-running a participant.
//
// Every row carries the XrTime it happened at, in nanoseconds, straight from the
// runtime. Asynchrony is derived offline from the beat and tap rows rather than
// stored, so a mistake in the matching rule is fixable after the fact.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class TapLogger {
public:
    // Feedback is logged separately from Tap so the ACTUAL delivered latency is
    // recoverable. Feedback appears on a frame boundary, so a nominal 50 ms
    // condition really lands somewhere near it. Record what happened, not what
    // was asked for.
    enum class Kind : uint8_t {
        Beat = 0, Tap = 1, Rating = 2, BlockStart = 3, BlockEnd = 4, Feedback = 5
    };

    struct Event {
        XrTime   time;        // ns, runtime clock
        Kind     kind;
        int32_t  block;       // -1 during practice
        int32_t  latencyMs;   // condition for this block
        int32_t  index;       // beat number within block, or rating value
        XrTime   sourceTime;  // for a Tap: the action's lastChangeTime if it differed
    };

    // ~20 minutes of session at a generous event rate.
    explicit TapLogger(size_t maxEvents = 20000) : m_max(maxEvents) {
        m_events.reserve(m_max);
    }

    void Record(XrTime t, Kind k, int32_t block, int32_t latencyMs,
                int32_t index, XrTime sourceTime = 0) {
        if (m_events.size() >= m_max) return;
        m_events.push_back({t, k, block, latencyMs, index, sourceTime});
    }

    bool WriteCSV(const std::string &path, const std::string &participant,
                  int32_t beatIntervalMs,
                  bool audioTimed = false, bool clockExact = false) const {
        if (m_events.empty()) return false;
        FILE *f = fopen(path.c_str(), "w");
        if (!f) return false;

        // Header carries the session constants so one file is self-describing.
        // audio_timed says whether beats were pinned to samples or to display
        // frames, which is the difference between a precise measurement and a
        // 14 ms-quantised one. Never analyse a file without reading this line.
        fprintf(f, "# participant=%s beat_interval_ms=%d audio_timed=%d clock_exact=%d\n",
                participant.c_str(), beatIntervalMs,
                audioTimed ? 1 : 0, clockExact ? 1 : 0);
        fprintf(f, "event,xr_time_ns,block,latency_ms,index,source_time_ns\n");
        for (const Event &e : m_events) {
            const char *k = "unknown";
            switch (e.kind) {
                case Kind::Beat:       k = "beat";        break;
                case Kind::Tap:        k = "tap";         break;
                case Kind::Rating:     k = "rating";      break;
                case Kind::BlockStart: k = "block_start"; break;
                case Kind::BlockEnd:   k = "block_end";   break;
                case Kind::Feedback:   k = "feedback";    break;
            }
            fprintf(f, "%s,%lld,%d,%d,%d,%lld\n",
                    k,
                    static_cast<long long>(e.time),
                    e.block, e.latencyMs, e.index,
                    static_cast<long long>(e.sourceTime));
        }
        fclose(f);
        return true;
    }

    size_t Count() const { return m_events.size(); }
    bool   Full()  const { return m_events.size() >= m_max; }
    void   Clear()       { m_events.clear(); }

private:
    std::vector<Event> m_events;
    size_t             m_max;
};
