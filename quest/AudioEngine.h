#pragma once

// Low-latency click engine for the metronome and the tap feedback.
//
// AAudio, not Oboe. AAudio ships in the NDK, so this costs one word in
// CMakeLists and no new dependency at all.
//
// ---------------------------------------------------------------------------
// WHY THIS IMPROVES THE MEASUREMENT, not just the task
//
// A visual beat is quantised to the display frame, about 14 ms at 72 Hz. An
// audio beat is scheduled at an exact sample. AAudioStream_getTimestamp hands
// back a matched pair of (frame position, CLOCK_MONOTONIC nanoseconds), so a
// frame scheduled in the future has a known presentation time. Convert that to
// XrTime with XR_KHR_convert_timespec_time and the beat is logged at the instant
// it will actually be heard.
//
// So the chain is:  audio frame -> CLOCK_MONOTONIC -> XrTime -> same clock as
// the tap's lastChangeTime. Both ends of the subtraction are now sample- or
// runtime-accurate rather than frame-quantised.
//
// Caveat worth stating in the writeup: getTimestamp reports presentation at the
// device, which is the best estimate available without external hardware, and it
// does not survive Bluetooth. Run sessions on the headset's own speakers. A BT
// headset adds a hundred milliseconds or more and would swamp every condition.
// ---------------------------------------------------------------------------

#include <aaudio/AAudio.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>

class AudioEngine {
public:
    static constexpr int kVoiceBeat     = 0;   // the metronome
    static constexpr int kVoiceLeadIn   = 1;   // quieter, higher, unscored beats
    static constexpr int kVoiceFeedback = 2;   // response to a tap, delayed
    static constexpr int kVoiceCount    = 3;

    bool Init() {
        AAudioStreamBuilder *b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return false;

        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(b, 1);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setDataCallback(b, &AudioEngine::Callback, this);

        aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
        if (r != AAUDIO_OK || !m_stream) {
            // Exclusive mode is not always available. Shared is fine, just slower.
            AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
            r = AAudioStreamBuilder_openStream(b, &m_stream);
        }
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK || !m_stream) return false;

        m_sampleRate = AAudioStream_getSampleRate(m_stream);
        if (m_sampleRate <= 0) m_sampleRate = 48000;

        if (AAudioStream_requestStart(m_stream) != AAUDIO_OK) {
            AAudioStream_close(m_stream);
            m_stream = nullptr;
            return false;
        }
        m_ready.store(true, std::memory_order_release);
        return true;
    }

    void Destroy() {
        m_ready.store(false, std::memory_order_release);
        if (m_stream) {
            AAudioStream_requestStop(m_stream);
            AAudioStream_close(m_stream);
            m_stream = nullptr;
        }
    }

    bool Ready() const      { return m_ready.load(std::memory_order_acquire); }
    int  SampleRate() const { return m_sampleRate; }

    // Schedule a click at an absolute stream frame position. Safe from the main
    // thread: slots are claimed with an atomic, and the audio callback only ever
    // reads them.
    void ScheduleClick(int64_t frame, int voice) {
        if (!Ready() || voice < 0 || voice >= kVoiceCount) return;
        const uint32_t slot = m_writeIndex.fetch_add(1, std::memory_order_relaxed) % kMaxScheduled;
        m_sched[slot].frame.store(frame, std::memory_order_relaxed);
        m_sched[slot].voice.store(voice, std::memory_order_relaxed);
        m_sched[slot].live.store(true,   std::memory_order_release);
    }

    // Frames the callback has produced so far. Approximate, for scheduling ahead.
    int64_t FramesWritten() const {
        return m_framesWritten.load(std::memory_order_acquire);
    }

    // Map a stream frame position to CLOCK_MONOTONIC nanoseconds. Works for
    // frames in the future, which is the whole point.
    bool FrameToMonotonicNs(int64_t frame, int64_t *outNs) {
        int64_t pos = 0, ns = 0;
        if (!Anchor(&pos, &ns)) return false;
        *outNs = ns + (int64_t)((double)(frame - pos) * 1e9 / (double)m_sampleRate);
        return true;
    }

    bool MonotonicNsToFrame(int64_t ns, int64_t *outFrame) {
        int64_t pos = 0, anchorNs = 0;
        if (!Anchor(&pos, &anchorNs)) return false;
        *outFrame = pos + (int64_t)((double)(ns - anchorNs) * (double)m_sampleRate / 1e9);
        return true;
    }

private:
    struct Slot {
        std::atomic<int64_t> frame{0};
        std::atomic<int>     voice{0};
        std::atomic<bool>    live{false};
    };
    struct Voice {
        bool   on    = false;
        int    n     = 0;      // samples elapsed in this click
        float  freq  = 880.0f;
        float  gain  = 0.22f;
        int    len   = 0;      // samples
    };

    static constexpr int kMaxScheduled = 128;

    // Pull a (framePosition, nanoseconds) pair from the stream. Cached briefly so
    // every scheduling call is not a syscall.
    bool Anchor(int64_t *pos, int64_t *ns) {
        if (!m_stream) return false;
        int64_t fp = 0, t = 0;
        if (AAudioStream_getTimestamp(m_stream, CLOCK_MONOTONIC, &fp, &t) == AAUDIO_OK) {
            m_anchorFrame = fp;
            m_anchorNs    = t;
            m_haveAnchor  = true;
        }
        if (!m_haveAnchor) {
            // Before the stream has produced a timestamp, fall back to now.
            struct timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            m_anchorFrame = FramesWritten();
            m_anchorNs    = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            m_haveAnchor  = true;
        }
        *pos = m_anchorFrame;
        *ns  = m_anchorNs;
        return true;
    }

    static aaudio_data_callback_result_t Callback(AAudioStream *, void *user,
                                                  void *audioData, int32_t numFrames) {
        return static_cast<AudioEngine *>(user)->Render(
            static_cast<float *>(audioData), numFrames);
    }

    aaudio_data_callback_result_t Render(float *out, int32_t numFrames) {
        const int64_t base = m_framesWritten.load(std::memory_order_relaxed);
        std::memset(out, 0, sizeof(float) * (size_t)numFrames);

        // Start any click whose moment falls inside this buffer.
        for (int s = 0; s < kMaxScheduled; s++) {
            if (!m_sched[s].live.load(std::memory_order_acquire)) continue;
            const int64_t f = m_sched[s].frame.load(std::memory_order_relaxed);
            if (f < base) {                       // missed it, drop rather than stack
                m_sched[s].live.store(false, std::memory_order_release);
                continue;
            }
            if (f >= base + numFrames) continue;  // later buffer

            const int v = m_sched[s].voice.load(std::memory_order_relaxed);
            m_sched[s].live.store(false, std::memory_order_release);
            StartVoice(v, (int)(f - base));
        }

        // Mix whatever is sounding.
        for (int v = 0; v < kVoiceCount; v++) {
            Voice &vo = m_voice[v];
            if (!vo.on) continue;
            for (int i = m_voiceStart[v]; i < numFrames && vo.on; i++) {
                const float t  = (float)vo.n / (float)m_sampleRate;
                const float env = (vo.len > 0)
                    ? (1.0f - (float)vo.n / (float)vo.len) : 0.0f;
                out[i] += vo.gain * env * env * sinf(6.2831853f * vo.freq * t);
                vo.n++;
                if (vo.n >= vo.len) vo.on = false;
            }
            m_voiceStart[v] = 0;
        }

        // Soft clip, since three voices can overlap.
        for (int i = 0; i < numFrames; i++) {
            if (out[i] >  1.0f) out[i] =  1.0f;
            if (out[i] < -1.0f) out[i] = -1.0f;
        }

        m_framesWritten.store(base + numFrames, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void StartVoice(int v, int offsetInBuffer) {
        Voice &vo = m_voice[v];
        vo.on = true;
        vo.n  = 0;
        switch (v) {
            case kVoiceBeat:     vo.freq = 880.0f;  vo.gain = 0.26f; break;
            case kVoiceLeadIn:   vo.freq = 1320.0f; vo.gain = 0.13f; break;
            case kVoiceFeedback: vo.freq = 523.0f;  vo.gain = 0.20f; break;
            default:             vo.freq = 880.0f;  vo.gain = 0.20f; break;
        }
        vo.len = m_sampleRate / 25;              // 40 ms
        m_voiceStart[v] = offsetInBuffer;
    }

    AAudioStream *m_stream = nullptr;
    int  m_sampleRate = 48000;
    std::atomic<bool>    m_ready{false};
    std::atomic<int64_t> m_framesWritten{0};
    std::atomic<uint32_t> m_writeIndex{0};
    Slot  m_sched[kMaxScheduled];
    Voice m_voice[kVoiceCount];
    int   m_voiceStart[kVoiceCount]{0, 0, 0};

    bool    m_haveAnchor = false;
    int64_t m_anchorFrame = 0;
    int64_t m_anchorNs    = 0;
};
