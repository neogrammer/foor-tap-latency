#pragma once

// Receives the foot pedal from the Raspberry Pi (pi/pedal_relay.py) over UDP and puts every press
// on this headset's own clock.
//
// WHAT COMES IN
//   One small JSON packet per press and per release, plus a heartbeat once a second:
//     {"gesture":"kick", ... ,"seq":12,"t_ns":8123456789012,"down":true,"late":false,"boot":51966,"v":2}
//   t_ns is when the pedal closed, read at the pin on the Pi, on the Pi's clock.
//
// WHY THIS CLASS TALKS BACK
//   A press is only useful to the study if its time is right, and a packet's arrival time is not
//   the press time: the WiFi adds a few milliseconds, and a different few each time. So the time
//   that counts is the one the Pi wrote into the packet, and this class has to know how the Pi's
//   clock relates to the headset's. It asks. Twenty times a second it sends a probe carrying its
//   own send time, and the Pi answers with when the probe arrived and when the answer left. Those
//   four times give the offset between the two clocks and the length of the round trip. The offset
//   from the fastest recent round trip is the one used, because a fast trip had the least room to
//   be lopsided. Half that round trip is the most the offset can be wrong by.
//
//   The probes do two other jobs. They find the Pi: they are broadcast until something answers,
//   so no address is written down anywhere. And they keep the headset's WiFi radio from dozing
//   between taps, which would otherwise hold a packet back until the radio next wakes.
//
// WHAT COMES OUT
//   drain() returns every press and release since the last call, oldest first. A press carries
//   monoNs, the press time on this headset's CLOCK_MONOTONIC. `timed` says whether that came from
//   the Pi's own timestamp (true) or had to fall back to the arrival time (false: an older relay
//   that sends no timestamp, or no clock offset yet).
//
//   A dropped packet does not lose a press. The relay sends each press twice and repeats the
//   latest one inside every other packet. Whichever copy lands first is used and the others are
//   recognised by their seq number.
//
// Nothing here knows about rendering, audio or the experiment. To move it into another app: copy
// the .h and .cpp, add the .cpp to CMake, put INTERNET in the manifest, start() it on a port, and
// drain() it from the frame loop.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <netinet/in.h>

struct PedalEvent {
    // Hat* only ever comes from the older two-pedal relay. The current relay has one pedal and
    // sends KickDown and KickUp.
    enum class Kind { KickDown, KickUp, HatClose, HatOpen };
    Kind     kind      = Kind::KickDown;
    int64_t  monoNs    = 0;       // when the pedal was pressed, on this headset's CLOCK_MONOTONIC
    int64_t  arrivalNs = 0;       // when the packet reached the headset
    bool     timed     = false;   // monoNs came from the Pi's timestamp through a live clock offset
    bool     late      = false;   // the Pi itself was unsure of the edge time (rare, see the relay)
    bool     recovered = false;   // the press packet was lost; rebuilt from a later packet
    int64_t  rttNs     = 0;       // round trip behind monoNs. Half of it bounds the clock error
    uint32_t seq       = 0;
    float    force     = 1.0f;

    bool isDown() const { return kind == Kind::KickDown || kind == Kind::HatClose; }
};

// A snapshot of the link, for the status line on screen and for the session log.
struct PedalLink {
    bool     online    = false;   // something arrived from a relay in the last two seconds
    bool     synced    = false;   // there is a clock offset young enough to trust
    bool     pedalDown = false;   // the pedal is being held right now
    int64_t  offsetNs  = 0;       // headset monotonic = Pi monotonic + offsetNs
    int64_t  rttNs     = 0;       // round trip of the sample that offset came from
    uint64_t packets = 0, probes = 0, replies = 0;
    uint64_t presses = 0;         // presses delivered to the app
    uint64_t untimed = 0;         // of those, how many had to use the arrival time
    uint64_t recovered = 0;       // of those, how many were rebuilt after a lost packet
    uint64_t lost = 0;            // presses the relay counted that never reached the app
    uint64_t duplicates = 0;      // repeat copies recognised and dropped
    char     peer[16] = {0};      // the relay's address, dotted quad
};

class PedalReceiver {
public:
    PedalReceiver() = default;
    ~PedalReceiver();
    PedalReceiver(const PedalReceiver &) = delete;
    PedalReceiver &operator=(const PedalReceiver &) = delete;

    // listenPort is where packets arrive. peerPort is where the relay listens for probes; leave
    // it 0 for "the same number", which is how the Pi is set up.
    bool start(int listenPort, int peerPort = 0);
    void stop();

    // Every press and release that arrived since the previous call, oldest first.
    std::vector<PedalEvent> drain();

    PedalLink link() const;
    bool online() const { return link().online; }

private:
    struct Sent   { uint32_t n = 0; int64_t t0 = 0; };
    struct Sample { int64_t at = 0, rtt = 0, off = 0; };

    void loop();
    void sendProbe(bool unicast);
    void handle(const char *json, int64_t t3, const sockaddr_in &from);
    void onSync(const char *json, int64_t t3);
    void onPress(int64_t tPi, int64_t t3, bool late, bool recovered, uint32_t seq);
    bool bestOffset(int64_t now, int64_t *off, int64_t *rtt) const;
    void push(const PedalEvent &ev);
    void publish(int64_t now);

    int               m_fd = -1;
    int               m_peerPort = 0;
    std::atomic<bool> m_running{false};
    std::thread       m_thread;

    mutable std::mutex      m_mutex;      // guards m_queue and m_link, nothing else
    std::vector<PedalEvent> m_queue;
    PedalLink               m_link;

    // Everything below is touched by the receive thread only.
    sockaddr_in m_peer{};
    bool        m_havePeer   = false;
    int64_t     m_peerSeen   = 0;
    int64_t     m_lastPacket = 0;

    static constexpr int kSent = 64;
    Sent     m_sent[kSent];
    uint32_t m_probeN = 0;

    static constexpr int kSamples = 64;
    Sample m_samples[kSamples];
    int    m_sampleHead = 0;

    bool    m_haveSeq = false;
    int64_t m_boot    = 0;
    int64_t m_lastSeq = 0;
    bool    m_hatClosed = false;

    PedalLink m_work;                     // counters build up here, then get copied to m_link
    int64_t   m_nextLog = 0;
    bool      m_saidSynced = false;
};
