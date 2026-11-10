/* Prova da riga di comando di ping_core: probe_cli <host> <porta> <secondi>
   Stampa una riga ogni 250 ms: t_ms stato rtt last var silence connects attempts */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "enet/enet.h"
#include "../src/ping_core.h"

static const char *name(PingState s)
{
    switch (s) {
    case PING_NO_TARGET: return "NO_TARGET";
    case PING_CONNECTING: return "CONNECTING";
    case PING_ONLINE: return "ONLINE";
    case PING_NO_ANSWER: return "NO_ANSWER";
    default: return "ERROR";
    }
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned port = argc > 2 ? (unsigned)atoi(argv[2]) : 24872;
    unsigned secs = argc > 3 ? (unsigned)atoi(argv[3]) : 5;
    PingProbe *p = ping_create(500, 1500);
    PingStatus st;
    unsigned long long start, last_print = 0, now;

    if (!p) {
        fprintf(stderr, "ping_create failed\n");
        return 2;
    }
    ping_set_target(p, host, (unsigned short)port);
    start = enet_time_get();
    for (;;) {
        unsigned idle = ping_step(p, 10);
        if (idle)
            sleep_ms(idle > 20 ? 20 : idle);
        now = enet_time_get() - start;
        if (now >= (unsigned long long)secs * 1000)
            break;
        if (now - last_print >= 250) {
            last_print = now;
            ping_status(p, &st);
            printf("%6llu %-10s rtt=%d last=%d enet=%d var=%d silence=%u connects=%u attempts=%u\n", now,
                   name(st.state), st.rtt_ms, st.last_rtt_ms, st.enet_rtt_ms, st.rtt_var_ms, st.silence_ms,
                   st.connects, st.attempts);
            fflush(stdout);
        }
    }
    ping_destroy(p);
    return 0;
}
