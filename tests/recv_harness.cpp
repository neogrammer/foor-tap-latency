// Runs the real PedalReceiver on a desktop and prints what the app would have been handed.
#include "PedalReceiver.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

static int64_t nowNs() {
    struct timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: recv_harness listenPort peerPort seconds\n"); return 2; }
    const int listenPort = atoi(argv[1]), peerPort = atoi(argv[2]);
    const double secs = atof(argv[3]);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    PedalReceiver r;
    if (!r.start(listenPort, peerPort)) return 3;
    const int64_t end = nowNs() + (int64_t)(secs * 1e9);
    const char *names[] = {"down", "up", "down", "up"};
    while (nowNs() < end) {
        for (const PedalEvent &ev : r.drain()) {
            printf("{\"ev\":\"%s\",\"hat\":%d,\"seq\":%u,\"mono\":%lld,\"arrival\":%lld,\"timed\":%d,\"late\":%d,"
                   "\"recovered\":%d,\"rtt\":%lld,\"drained\":%lld}\n",
                   names[(int)ev.kind], (ev.kind == PedalEvent::Kind::HatClose || ev.kind == PedalEvent::Kind::HatOpen) ? 1 : 0,
                   ev.seq, (long long)ev.monoNs, (long long)ev.arrivalNs, ev.timed ? 1 : 0, ev.late ? 1 : 0,
                   ev.recovered ? 1 : 0, (long long)ev.rttNs, (long long)nowNs());
        }
        usleep(11111);      // one 90 Hz frame: the cadence the headset empties the queue at
    }
    const PedalLink L = r.link();
    printf("{\"ev\":\"link\",\"online\":%d,\"synced\":%d,\"offset\":%lld,\"rtt\":%lld,\"packets\":%llu,\"probes\":%llu,"
           "\"replies\":%llu,\"presses\":%llu,\"untimed\":%llu,\"recovered\":%llu,\"lost\":%llu,\"duplicates\":%llu,\"peer\":\"%s\"}\n",
           L.online ? 1 : 0, L.synced ? 1 : 0, (long long)L.offsetNs, (long long)L.rttNs,
           (unsigned long long)L.packets, (unsigned long long)L.probes, (unsigned long long)L.replies,
           (unsigned long long)L.presses, (unsigned long long)L.untimed, (unsigned long long)L.recovered,
           (unsigned long long)L.lost, (unsigned long long)L.duplicates, L.peer);
    r.stop();
    return 0;
}
