#include "PedalReceiver.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <android/log.h>

#define LOG_TAG "PedalReceiver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int64_t kMs = 1000000LL;
constexpr int64_t kProbeFastNs   = 50 * kMs;     // a relay is answering: 20 probes a second
constexpr int64_t kProbeSearchNs = 500 * kMs;    // looking for one: two shouts a second
constexpr int64_t kPeerFreshNs   = 3000 * kMs;   // no word for this long and the relay counts as gone
constexpr int64_t kOnlineNs      = 2000 * kMs;
constexpr int64_t kWindowNs      = 3000 * kMs;   // the offset comes from the best round trip in this window
constexpr int64_t kStaleNs       = 20000 * kMs;  // past this the old offset is not trusted at all
constexpr int64_t kMaxRttNs      = 50 * kMs;     // a round trip slower than this is not used. The first
                                                 // answer to a broadcast probe is one: broadcasts wait
                                                 // for the access point's next beacon, a fifth of a second
constexpr int64_t kMaxPressAgeNs = 1000 * kMs;   // a press that took over a second to arrive is treated as lost
constexpr int64_t kLogEveryNs    = 10000 * kMs;

int64_t nowMonoNs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Tiny field readers for the relay's compact JSON. No spaces and no nesting, so a substring
// search is enough and no JSON library is needed.
bool findValue(const char *json, const char *key, const char **out) {
    std::string pat = std::string("\"") + key + "\":";
    const char *p = std::strstr(json, pat.c_str());
    if (!p) return false;
    *out = p + pat.size();
    return true;
}

float readFloat(const char *json, const char *key, float fallback) {
    const char *v = nullptr;
    if (!findValue(json, key, &v)) return fallback;
    if (std::strncmp(v, "true", 4) == 0)  return 1.0f;
    if (std::strncmp(v, "false", 5) == 0) return 0.0f;
    return std::strtof(v, nullptr);
}

bool readInt(const char *json, const char *key, int64_t *out) {
    const char *v = nullptr;
    if (!findValue(json, key, &v)) return false;
    char *end = nullptr;
    const long long n = std::strtoll(v, &end, 10);
    if (end == v) return false;
    *out = (int64_t)n;
    return true;
}

bool readBool(const char *json, const char *key, bool fallback) {
    const char *v = nullptr;
    if (!findValue(json, key, &v)) return fallback;
    if (std::strncmp(v, "true", 4) == 0)  return true;
    if (std::strncmp(v, "false", 5) == 0) return false;
    return fallback;
}

std::string readString(const char *json, const char *key) {
    const char *v = nullptr;
    if (!findValue(json, key, &v) || *v != '"') return "";
    const char *end = std::strchr(v + 1, '"');
    if (!end) return "";
    return std::string(v + 1, end);
}

} // namespace

PedalReceiver::~PedalReceiver() { stop(); }

bool PedalReceiver::start(int listenPort, int peerPort) {
    if (m_running.load()) return true;

    m_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return false;
    }

    int on = 1;
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));   // the search probes are broadcast

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)listenPort);
    if (bind(m_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind(%d) failed: %s", listenPort, strerror(errno));
        close(m_fd);
        m_fd = -1;
        return false;
    }

    m_peerPort = peerPort > 0 ? peerPort : listenPort;
    m_running.store(true);
    m_thread = std::thread(&PedalReceiver::loop, this);
    LOGI("listening for the pedal on UDP %d, probing on %d", listenPort, m_peerPort);
    return true;
}

void PedalReceiver::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

std::vector<PedalEvent> PedalReceiver::drain() {
    std::vector<PedalEvent> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    out.swap(m_queue);
    return out;
}

PedalLink PedalReceiver::link() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_link;
}

void PedalReceiver::push(const PedalEvent &ev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() < 256) m_queue.push_back(ev);   // the frame loop empties this every frame
}

void PedalReceiver::loop() {
    char buf[1024];
    int64_t nextProbe = 0;

    while (m_running.load()) {
        int64_t now = nowMonoNs();
        if (now >= nextProbe) {
            const bool fresh = m_havePeer && (now - m_peerSeen) < kPeerFreshNs;
            sendProbe(fresh);
            nextProbe = now + (fresh ? kProbeFastNs : kProbeSearchNs);
        }

        // Sleep until a packet lands or the next probe is due, whichever is first. Never longer
        // than 100 ms, so stop() is quick.
        int waitMs = (int)((nextProbe - nowMonoNs()) / kMs);
        if (waitMs < 1)   waitMs = 1;
        if (waitMs > 100) waitMs = 100;
        pollfd pfd{m_fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, waitMs);

        if (ready > 0 && (pfd.revents & POLLIN)) {
            // Take everything that is waiting, so a burst is stamped as tightly as it can be.
            for (;;) {
                sockaddr_in from{};
                socklen_t fromLen = sizeof(from);
                const ssize_t n = recvfrom(m_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                                           (sockaddr *)&from, &fromLen);
                if (n <= 0) break;
                const int64_t t3 = nowMonoNs();
                buf[n] = '\0';
                handle(buf, t3, from);
            }
        }
        publish(nowMonoNs());
    }
}

void PedalReceiver::sendProbe(bool unicast) {
    const uint32_t n = ++m_probeN;
    char msg[96];
    const int64_t t0 = nowMonoNs();
    const int len = snprintf(msg, sizeof(msg), "{\"probe\":%u,\"t0\":%lld}", n, (long long)t0);
    m_sent[n % kSent] = {n, t0};
    m_work.probes++;

    if (m_havePeer) sendto(m_fd, msg, (size_t)len, 0, (sockaddr *)&m_peer, sizeof(m_peer));
    if (unicast) return;

    // Nobody is answering, so ask the whole network. The plain broadcast address first, then
    // each interface's own, because some routers pass one and not the other.
    sockaddr_in to{};
    to.sin_family      = AF_INET;
    to.sin_port        = htons((uint16_t)m_peerPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(m_fd, msg, (size_t)len, 0, (sockaddr *)&to, sizeof(to));

    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST)) continue;
            if (i->ifa_flags & IFF_LOOPBACK) continue;
            if (!i->ifa_broadaddr) continue;
            to.sin_addr = ((sockaddr_in *)i->ifa_broadaddr)->sin_addr;
            sendto(m_fd, msg, (size_t)len, 0, (sockaddr *)&to, sizeof(to));
        }
        freeifaddrs(list);
    }
}

void PedalReceiver::handle(const char *json, int64_t t3, const sockaddr_in &from) {
    // A probe: our own shout coming back off the broadcast, or another headset's. Not a relay.
    if (std::strstr(json, "\"probe\":")) return;

    // Anything else on this port is a relay. Remember where it lives; that is where probes go.
    if (!m_havePeer || from.sin_addr.s_addr != m_peer.sin_addr.s_addr) {
        inet_ntop(AF_INET, &from.sin_addr, m_work.peer, sizeof(m_work.peer));
        LOGI("pedal relay found at %s", m_work.peer);
        m_saidSynced = false;
    }
    m_peer          = from;
    m_peer.sin_port = htons((uint16_t)m_peerPort);
    m_havePeer      = true;
    m_peerSeen      = t3;
    m_lastPacket    = t3;
    m_work.packets++;

    const std::string gesture = readString(json, "gesture");

    // The clock sample first, so a press riding in the same packet is converted with it.
    if (gesture == "sync") onSync(json, t3);

    int64_t seq = 0, tPi = 0;
    if (readInt(json, "seq", &seq) && readInt(json, "t_ns", &tPi)) {
        int64_t boot = 0;
        readInt(json, "boot", &boot);
        const bool live = (gesture == "kick");
        m_work.pedalDown = readBool(json, "down", false);

        if (!m_haveSeq || boot != m_boot) {
            // First word from this relay, or it restarted and its counter began again. Start
            // counting from here. A press in this very packet is happening now and counts;
            // anything the counter already held happened before we were listening.
            m_haveSeq = true;
            m_boot    = boot;
            m_lastSeq = live ? seq - 1 : seq;
        }
        if (seq > m_lastSeq) {
            // The relay only repeats its latest press, so a jump of more than one means the
            // ones in between are gone for good. Count them rather than pretend.
            m_work.lost += (uint64_t)(seq - m_lastSeq - 1);
            m_lastSeq = seq;
            onPress(tPi, t3, readBool(json, "late", false), !live, (uint32_t)seq);
        } else if (live) {
            m_work.duplicates++;
        }
        if (gesture == "idle") {
            PedalEvent up;
            up.kind      = PedalEvent::Kind::KickUp;
            up.monoNs    = t3;
            up.arrivalNs = t3;
            up.seq       = (uint32_t)seq;
            push(up);
        }
        return;
    }

    // An older relay. No press time in the packet, so the arrival time is all there is.
    PedalEvent ev;
    ev.monoNs    = t3;
    ev.arrivalNs = t3;
    ev.force     = readFloat(json, "kick_force", 1.0f);

    if      (gesture == "kick")        ev.kind = PedalEvent::Kind::KickDown;
    else if (gesture == "idle")        ev.kind = PedalEvent::Kind::KickUp;
    else if (gesture == "hihat_close") ev.kind = PedalEvent::Kind::HatClose;
    else if (gesture == "hihat_open")  ev.kind = PedalEvent::Kind::HatOpen;
    else if (gesture == "heartbeat" || gesture == "sync") return;
    else {
        // Unknown gesture word: fall back to the boolean fields, which keeps the oldest
        // senders usable.
        const bool kicking = readFloat(json, "is_kicking", 0.0f) > 0.5f;
        const bool closed  = readFloat(json, "pedal_depth", 0.0f) > 0.5f;
        if (kicking)                    ev.kind = PedalEvent::Kind::KickDown;
        else if (closed != m_hatClosed) ev.kind = closed ? PedalEvent::Kind::HatClose
                                                         : PedalEvent::Kind::HatOpen;
        else return;
    }

    if (ev.kind == PedalEvent::Kind::HatClose) m_hatClosed = true;
    if (ev.kind == PedalEvent::Kind::HatOpen)  m_hatClosed = false;
    m_work.pedalDown = ev.isDown();
    if (ev.isDown()) {
        m_work.presses++;
        m_work.untimed++;
    }
    push(ev);
}

void PedalReceiver::onSync(const char *json, int64_t t3) {
    int64_t n = 0, t0 = 0, t1 = 0, t2 = 0;
    if (!readInt(json, "n", &n) || !readInt(json, "t0", &t0) ||
        !readInt(json, "t1", &t1) || !readInt(json, "t2", &t2)) return;

    // Only answers to probes this headset actually sent, with the send time it actually used.
    const Sent &s = m_sent[(uint32_t)n % kSent];
    if (s.n != (uint32_t)n || s.t0 != t0) return;

    // t0 and t3 are this headset's clock, t1 and t2 are the Pi's. Time in flight is the whole
    // trip minus the time the Pi spent holding the packet.
    const int64_t rtt = (t3 - t0) - (t2 - t1);
    if (rtt < 0 || rtt > kMaxRttNs) return;

    // If the two directions took equally long, the Pi's clock read (t1 + t2) / 2 at the moment
    // this headset's read (t0 + t3) / 2. The difference is the offset.
    const int64_t off = ((t0 - t1) + (t3 - t2)) / 2;

    m_samples[m_sampleHead] = {t3, rtt, off};
    m_sampleHead = (m_sampleHead + 1) % kSamples;
    m_work.replies++;
}

bool PedalReceiver::bestOffset(int64_t now, int64_t *off, int64_t *rtt) const {
    const Sample *best = nullptr;
    // Prefer the last few seconds. If the link has gone quiet, an older sample is still better
    // than nothing for a while, because the two clocks drift apart far slower than a millisecond
    // a second.
    for (int pass = 0; pass < 2 && !best; pass++) {
        const int64_t window = (pass == 0) ? kWindowNs : kStaleNs;
        for (int i = 0; i < kSamples; i++) {
            const Sample &s = m_samples[i];
            if (s.at == 0 || now - s.at > window) continue;
            if (!best || s.rtt < best->rtt) best = &s;
        }
    }
    if (!best) return false;
    *off = best->off;
    *rtt = best->rtt;
    return true;
}

void PedalReceiver::onPress(int64_t tPi, int64_t t3, bool late, bool recovered, uint32_t seq) {
    PedalEvent ev;
    ev.kind      = PedalEvent::Kind::KickDown;
    ev.arrivalNs = t3;
    ev.late      = late;
    ev.recovered = recovered;
    ev.seq       = seq;

    int64_t off = 0, rtt = 0;
    if (bestOffset(t3, &off, &rtt)) {
        int64_t pressed = tPi + off;
        if (pressed > t3) pressed = t3;          // a press cannot be later than the packet reporting it
        if (t3 - pressed > kMaxPressAgeNs) {     // too old to act on: say so instead of clicking late
            m_work.lost++;
            return;
        }
        ev.monoNs = pressed;
        ev.timed  = true;
        ev.rttNs  = rtt;
    } else {
        if (recovered) {                         // no live packet and no clock: nothing honest to report
            m_work.lost++;
            return;
        }
        ev.monoNs = t3;
        ev.timed  = false;
        m_work.untimed++;
    }

    m_work.presses++;
    if (recovered) m_work.recovered++;
    push(ev);
}

void PedalReceiver::publish(int64_t now) {
    m_work.online = m_lastPacket != 0 && (now - m_lastPacket) < kOnlineNs;
    int64_t off = 0, rtt = 0;
    m_work.synced   = m_work.online && bestOffset(now, &off, &rtt);
    m_work.offsetNs = off;
    m_work.rttNs    = rtt;
    if (!m_work.online) m_work.pedalDown = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_link = m_work;
    }

    // Logging on this thread delays the next packet, so it is rare: once when the clocks first
    // line up, then a summary every ten seconds while a relay is there.
    if (m_work.synced && !m_saidSynced) {
        m_saidSynced = true;
        LOGI("clock matched to %s: round trip %.2f ms, so press times are good to about %.2f ms",
             m_work.peer, (double)rtt / 1e6, (double)rtt / 2e6);
    }
    if (m_work.online && now >= m_nextLog) {
        m_nextLog = now + kLogEveryNs;
        LOGI("link %s %s rtt %.2f ms | presses %llu untimed %llu recovered %llu lost %llu dup %llu | probes %llu replies %llu",
             m_work.peer, m_work.synced ? "synced" : "NOT synced", (double)rtt / 1e6,
             (unsigned long long)m_work.presses, (unsigned long long)m_work.untimed,
             (unsigned long long)m_work.recovered, (unsigned long long)m_work.lost,
             (unsigned long long)m_work.duplicates,
             (unsigned long long)m_work.probes, (unsigned long long)m_work.replies);
    }
}
