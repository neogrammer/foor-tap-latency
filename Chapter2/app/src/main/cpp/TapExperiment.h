#pragma once

// The experiment itself: conditions, beat scheduling, latency injection, and the
// dialog flow from the paper prototype. Knows nothing about rendering. main.cpp
// reads Render() each frame and draws boxes accordingly.
//
// ---------------------------------------------------------------------------
// HOW TIME WORKS HERE, because it is the whole study
//
// Everything logged is XrTime, in nanoseconds, from the runtime's own clock.
//
// THE BEAT is scheduled at an exact audio sample. AAudio gives a matched pair of
// (frame position, CLOCK_MONOTONIC time), so a frame scheduled in the future has
// a known presentation time; XrClock converts that to XrTime. The beat is
// therefore logged at the instant it will actually be HEARD, not at the display
// frame on which something flashed. The visual pulse still lands on a frame and
// lags by up to 14 ms, but it is accompaniment, not the stimulus.
//
// THE TAP comes from XrActionStateBoolean::lastChangeTime, the runtime's own
// record of when the button changed rather than the frame it was noticed on.
//
// So both ends of the subtraction are finer than the display frame, which is the
// only reason this measurement is worth anything on a 72 Hz device.
//
// THE FEEDBACK is likewise scheduled at a sample, so a nominal 50 ms condition
// delivers 50 ms rather than 50 ms rounded to the next frame.
//
// Without audio, all of the above degrades to frame-quantised timing and the
// session still runs. The CSV records which path produced it.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <string>

class TapExperiment {
public:
    // ----- tunables -------------------------------------------------------
    // The proposal specifies 60 scored taps per block. 24 is set here so a full
    // run takes about three minutes while you are still debugging. Raise it to
    // 60 before collecting anything you intend to report.
    static constexpr int kScoredTapsPerBlock = 24;
    static constexpr int kLeadInBeats        = 4;
    static constexpr int kPracticeBeats      = 12;
    static constexpr int kConditionCount     = 5;
    static constexpr int kBeatIntervalMs     = 600;   // 100 BPM
    static constexpr int kRatingMin = 1, kRatingMax = 7;
    static constexpr int kMaxBeats  = 256;

    static constexpr int kConditions[kConditionCount] = {0, 50, 100, 200, 400};

    enum class Phase {
        Diagnostic, Welcome, PracticeIntro, Practice, ReadyCheck,
        Countdown, Block, BlockComplete, Rating, Rest, Done
    };

    struct RenderState {
        Phase phase          = Phase::Diagnostic;
        int   blockNumber    = 0;
        int   blockTotal     = kConditionCount;
        int   tapsThisBlock  = 0;
        int   tapsTarget     = kScoredTapsPerBlock;
        int   countdown      = 0;
        int   rating         = 4;
        float beatPulse      = 0;
        float feedbackFlash  = 0;
        bool  awaitingInput  = false;
        // Diagnostic readout
        bool  leftPressed    = false;
        bool  rightPressed   = false;
        bool  handsActive    = false;
        bool  simultaneous   = false;
        bool  audioReady     = false;
        bool  clockExact     = false;
    };

    // Optional. Pass nullptr for either and the visual fallback takes over.
    void SetAudio(AudioEngine *audio, XrClock *clock) {
        m_audio = audio;
        m_clock = clock;
    }

    void Init(const std::string &participant, int participantNumber) {
        m_participant = participant;
        BuildOrder(participantNumber);
        m_phase    = Phase::Diagnostic;
        m_blockIdx = -1;
        m_rating   = 4;
        m_logger.Clear();
    }

    // goPressed     : GO was pinched (or the fallback trigger was pulled)
    // ratingPicked  : 1..7 when a rating digit was pinched this frame, else 0
    void Update(XrTime frameTime,
                bool tapJustPressed, XrTime tapTime,
                bool goPressed, int ratingPicked) {

        m_beatPulse     *= 0.80f;
        m_feedbackFlash *= 0.80f;

        switch (m_phase) {

        case Phase::Diagnostic:
            // The one unknown in the whole design -- does a controller lying on
            // the floor still report button presses -- is answerable here in
            // under a minute, before any of the rest matters.
            if (goPressed) m_phase = Phase::Welcome;
            break;

        case Phase::Welcome:
            if (goPressed) m_phase = Phase::PracticeIntro;
            break;

        case Phase::PracticeIntro:
            if (goPressed) {
                StartBeats(frameTime, kPracticeBeats, 0, /*practice=*/true);
                m_phase = Phase::Practice;
            }
            break;

        case Phase::Practice:
            ServiceBeats(frameTime, true);
            if (tapJustPressed) RegisterTap(tapTime, frameTime, true);
            ServiceFeedback(frameTime, true);
            if (m_beatsFired >= m_beatsTotal) m_phase = Phase::ReadyCheck;
            break;

        case Phase::ReadyCheck:
            if (goPressed) { m_countdownFrom = frameTime; m_phase = Phase::Countdown; }
            break;

        case Phase::Countdown: {
            const int64_t elapsedMs = (frameTime - m_countdownFrom) / 1000000LL;
            m_countdown = 3 - (int)(elapsedMs / 800);
            if (m_countdown <= 0) {
                m_blockIdx++;
                if (m_blockIdx >= kConditionCount) { m_phase = Phase::Done; break; }
                m_logger.Record(frameTime, TapLogger::Kind::BlockStart,
                                m_blockIdx + 1, CurrentLatencyMs(), 0);
                StartBeats(frameTime, kScoredTapsPerBlock + kLeadInBeats,
                           CurrentLatencyMs(), /*practice=*/false);
                m_phase = Phase::Block;
            }
            break;
        }

        case Phase::Block:
            ServiceBeats(frameTime, false);
            if (tapJustPressed) RegisterTap(tapTime, frameTime, false);
            ServiceFeedback(frameTime, false);
            if (m_beatsFired >= m_beatsTotal) {
                m_logger.Record(frameTime, TapLogger::Kind::BlockEnd,
                                m_blockIdx + 1, CurrentLatencyMs(), m_tapsThisBlock);
                m_phase = Phase::BlockComplete;
            }
            break;

        case Phase::BlockComplete:
            if (goPressed) { m_rating = 4; m_phase = Phase::Rating; }
            break;

        case Phase::Rating:
            // Pinching a digit selects it. GO commits. Two steps on purpose: a
            // stray pinch should not silently record the wrong rating.
            if (ratingPicked >= kRatingMin && ratingPicked <= kRatingMax)
                m_rating = ratingPicked;
            if (goPressed) {
                m_logger.Record(frameTime, TapLogger::Kind::Rating,
                                m_blockIdx + 1, CurrentLatencyMs(), m_rating);
                m_phase = (m_blockIdx + 1 >= kConditionCount) ? Phase::Done : Phase::Rest;
            }
            break;

        case Phase::Rest:
            if (goPressed) { m_countdownFrom = frameTime; m_phase = Phase::Countdown; }
            break;

        case Phase::Done:
            break;
        }
    }

    RenderState Render() const {
        RenderState r;
        r.phase         = m_phase;
        r.blockNumber   = (m_blockIdx >= 0) ? m_blockIdx + 1 : 0;
        r.tapsThisBlock = m_tapsThisBlock;
        r.countdown     = (m_countdown > 0 ? m_countdown : 1);
        r.rating        = m_rating;
        r.beatPulse     = m_beatPulse;
        r.feedbackFlash = m_feedbackFlash;
        r.awaitingInput = (m_phase != Phase::Practice && m_phase != Phase::Block
                           && m_phase != Phase::Countdown);
        r.audioReady    = (m_audio && m_audio->Ready());
        r.clockExact    = (m_clock && m_clock->Exact());
        return r;
    }

    bool Finished() const { return m_phase == Phase::Done; }
    bool AudioActive() const { return m_audioActive; }

    bool WriteCSV(const std::string &path) const {
        return m_logger.WriteCSV(path, m_participant, kBeatIntervalMs,
                                 m_audioActive, m_clock && m_clock->Exact());
    }
    size_t EventCount() const { return m_logger.Count(); }

    int CurrentLatencyMs() const {
        if (m_blockIdx < 0 || m_blockIdx >= kConditionCount) return 0;
        return m_order[m_blockIdx];
    }

private:
    // 5x5 cyclic Latin square. Balances position but NOT first-order carryover.
    // A Williams design is the correct structure and is in the beta plan.
    void BuildOrder(int participantNumber) {
        const int p = ((participantNumber % kConditionCount) + kConditionCount) % kConditionCount;
        for (int i = 0; i < kConditionCount; i++)
            m_order[i] = kConditions[(i + p) % kConditionCount];
    }

    // Lay out every beat in the block up front. With audio, each one is pinned to
    // a sample and its heard time is known before the block starts. Without, the
    // times are nominal and the frame they fire on is the best available.
    void StartBeats(XrTime now, int total, int latencyMs, bool practice) {
        if (total > kMaxBeats) total = kMaxBeats;
        m_beatsTotal    = total;
        m_beatsFired    = 0;
        m_tapsThisBlock = 0;
        m_latencyNs     = (int64_t)latencyMs * 1000000LL;
        m_pendingCount  = 0;
        m_audioActive   = false;

        const int64_t leadNs = 700000000LL;   // 0.7 s before the first beat
        const int leadIn = practice ? 0 : kLeadInBeats;

        if (m_audio && m_audio->Ready() && m_clock) {
            int64_t nowMono = 0;
            if (m_clock->XrToMonotonic(now, &nowMono)) {
                int64_t startFrame = 0;
                if (m_audio->MonotonicNsToFrame(nowMono + leadNs, &startFrame)) {
                    const int64_t fpb =
                        (int64_t)m_audio->SampleRate() * kBeatIntervalMs / 1000;
                    bool ok = true;
                    for (int k = 0; k < total; k++) {
                        const int64_t f = startFrame + (int64_t)k * fpb;
                        int64_t mono = 0;
                        XrTime  xr   = 0;
                        if (!m_audio->FrameToMonotonicNs(f, &mono) ||
                            !m_clock->MonotonicToXr(mono, &xr)) { ok = false; break; }
                        m_beatXr[k] = xr;
                        m_audio->ScheduleClick(
                            f, (k < leadIn) ? AudioEngine::kVoiceLeadIn
                                            : AudioEngine::kVoiceBeat);
                    }
                    m_audioActive = ok;
                }
            }
        }

        if (!m_audioActive) {
            // Visual fallback. Nominal times; the firing frame is what we get.
            for (int k = 0; k < total; k++)
                m_beatXr[k] = now + leadNs + (XrTime)k * (XrTime)kBeatIntervalMs * 1000000LL;
        }
    }

    void ServiceBeats(XrTime frameTime, bool practice) {
        const int leadIn = practice ? 0 : kLeadInBeats;
        while (m_beatsFired < m_beatsTotal && frameTime >= m_beatXr[m_beatsFired]) {
            const int scoredIndex = m_beatsFired - leadIn;
            if (!practice && scoredIndex >= 0) {
                // Log the beat's own time, not this frame's. With audio that is
                // the sample it was heard at; the frame merely noticed it.
                m_logger.Record(m_beatXr[m_beatsFired], TapLogger::Kind::Beat,
                                m_blockIdx + 1, CurrentLatencyMs(), scoredIndex,
                                frameTime);
            }
            m_beatsFired++;
            m_beatPulse = 1.0f;
        }
    }

    void RegisterTap(XrTime tapTime, XrTime frameTime, bool practice) {
        // lastChangeTime can sit slightly either side of the frame; trust it, but
        // refuse anything wild.
        XrTime t = tapTime;
        const int64_t drift = (int64_t)t - (int64_t)frameTime;
        if (t == 0 || drift > 200000000LL || drift < -200000000LL) t = frameTime;

        if (!practice) {
            m_logger.Record(t, TapLogger::Kind::Tap,
                            m_blockIdx + 1, CurrentLatencyMs(), m_tapsThisBlock, frameTime);
        }
        m_tapsThisBlock++;

        const XrTime due = t + (XrTime)m_latencyNs;

        // Pin the feedback click to a sample so the delivered latency is the
        // nominal one rather than the nominal one rounded up to a frame.
        if (m_audioActive && m_audio && m_clock) {
            int64_t mono = 0, frame = 0;
            if (m_clock->XrToMonotonic(due, &mono) &&
                m_audio->MonotonicNsToFrame(mono, &frame)) {
                m_audio->ScheduleClick(frame, AudioEngine::kVoiceFeedback);
            }
        }
        if (m_pendingCount < kMaxPending) m_pending[m_pendingCount++] = due;
    }

    void ServiceFeedback(XrTime frameTime, bool practice) {
        int w = 0;
        for (int i = 0; i < m_pendingCount; i++) {
            if (frameTime >= m_pending[i]) {
                m_feedbackFlash = 1.0f;
                if (!practice) {
                    m_logger.Record(m_pending[i], TapLogger::Kind::Feedback,
                                    m_blockIdx + 1, CurrentLatencyMs(), i, frameTime);
                }
            } else {
                m_pending[w++] = m_pending[i];
            }
        }
        m_pendingCount = w;
    }

    static constexpr int kMaxPending = 32;

    AudioEngine *m_audio = nullptr;
    XrClock     *m_clock = nullptr;
    bool         m_audioActive = false;

    std::string m_participant = "P01";
    int         m_order[kConditionCount]{};
    Phase       m_phase    = Phase::Diagnostic;
    int         m_blockIdx = -1;

    XrTime  m_beatXr[kMaxBeats]{};
    int     m_beatsTotal    = 0;
    int     m_beatsFired    = 0;
    int     m_tapsThisBlock = 0;
    int64_t m_latencyNs     = 0;

    XrTime  m_pending[kMaxPending]{};
    int     m_pendingCount  = 0;

    XrTime  m_countdownFrom = 0;
    int     m_countdown     = 3;
    int     m_rating        = 4;

    float   m_beatPulse     = 0;
    float   m_feedbackFlash = 0;

    TapLogger m_logger;
};

constexpr int TapExperiment::kConditions[TapExperiment::kConditionCount];
