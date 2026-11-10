#include "ping_core.h"

#include <stdlib.h>
#include <string.h>

#include "enet/enet.h"
#include "enet/time.h" /* ENET_TIME_DIFFERENCE, ENET_TIME_GREATER_EQUAL */

#define HOST_MAX 128
#define CONNECT_CHANNELS 1 /* come NumChannels di Azahar (src/network/room.h) */
#define SAMPLES_MAX 32
#define RTT_WINDOW_MS 2000      /* finestra della media, come l'overlay Python */
#define DROP_AFTER_SILENCE_MS 4000 /* nessun datagramma per cosi' tanto: si butta la connessione e si riprova */

struct PingProbe {
    ENetHost *host;
    ENetPeer *peer;
    ENetAddress addr;
    int resolved;

    char target_host[HOST_MAX];
    unsigned short target_port;
    int has_target;

    unsigned interval_ms;
    unsigned silence_limit_ms;

    enet_uint32 connect_started; /* quando e' partito l'ultimo enet_host_connect */
    enet_uint32 next_attempt;    /* non prima di questo istante (orologio ENet) */
    unsigned backoff_ms;
    int attempt_wait;            /* next_attempt e' valido */

    /* campioni grezzi dell'RTT (uno per ACK), per la media su RTT_WINDOW_MS */
    enet_uint32 s_time[SAMPLES_MAX];
    int s_val[SAMPLES_MAX];
    unsigned s_head, s_count;
    enet_uint32 last_recv_seen;

    PingStatus st;
};

static void sample_clear(PingProbe *p) { p->s_head = p->s_count = 0; p->last_recv_seen = 0; }

static void sample_push(PingProbe *p, enet_uint32 now, int value)
{
    p->s_time[p->s_head] = now;
    p->s_val[p->s_head] = value;
    p->s_head = (p->s_head + 1) % SAMPLES_MAX;
    if (p->s_count < SAMPLES_MAX)
        ++p->s_count;
}

/* media dei campioni degli ultimi RTT_WINDOW_MS; -1 se non ce ne sono */
static int sample_mean(const PingProbe *p, enet_uint32 now)
{
    unsigned i;
    long sum = 0;
    int n = 0;
    for (i = 0; i < p->s_count; ++i) {
        unsigned idx = (p->s_head + SAMPLES_MAX - 1 - i) % SAMPLES_MAX;
        if (ENET_TIME_DIFFERENCE(now, p->s_time[idx]) > RTT_WINDOW_MS)
            break;
        sum += p->s_val[idx];
        ++n;
    }
    return n ? (int)((sum + n / 2) / n) : -1;
}

static int time_reached(enet_uint32 now, enet_uint32 t) { return ENET_TIME_GREATER_EQUAL(now, t); }

PingProbe *ping_create(unsigned ping_interval_ms, unsigned silence_limit_ms)
{
    PingProbe *p;
    if (enet_initialize() != 0)
        return NULL;
    p = (PingProbe *)calloc(1, sizeof(*p));
    if (!p) {
        enet_deinitialize();
        return NULL;
    }
    /* Regola: la sonda non chiede mai un PING piu' spesso di ogni 500 ms (l'intervallo di ENet di default). */
    p->interval_ms = ping_interval_ms < PING_MIN_INTERVAL_MS ? PING_MIN_INTERVAL_MS : ping_interval_ms;
    p->silence_limit_ms = silence_limit_ms ? silence_limit_ms : 1500;
    p->st.state = PING_NO_TARGET;
    p->st.rtt_ms = -1;
    p->st.last_rtt_ms = -1;
    p->st.enet_rtt_ms = -1;
    p->backoff_ms = 1000;
    return p;
}

static void drop_peer(PingProbe *p, int polite)
{
    if (p->peer) {
        if (polite) {
            /* DISCONNECT immediato: la stanza libera il peer subito invece di aspettare il timeout */
            enet_peer_disconnect_now(p->peer, 0);
        } else {
            enet_peer_reset(p->peer);
        }
        p->peer = NULL;
    }
}

void ping_set_target(PingProbe *p, const char *host, unsigned short port)
{
    if (!p)
        return;
    drop_peer(p, 1);
    if (p->host)
        enet_host_flush(p->host); /* il DISCONNECT parte adesso */
    p->resolved = 0;
    p->attempt_wait = 0;
    p->backoff_ms = 1000;
    p->st.rtt_ms = p->st.last_rtt_ms = p->st.enet_rtt_ms = -1;
    p->st.silence_ms = 0;
    sample_clear(p);
    if (!host || !host[0]) {
        p->has_target = 0;
        p->target_host[0] = 0;
        p->st.state = PING_NO_TARGET;
        return;
    }
    strncpy(p->target_host, host, HOST_MAX - 1);
    p->target_host[HOST_MAX - 1] = 0;
    p->target_port = port ? port : 24872;
    p->has_target = 1;
    p->st.state = PING_CONNECTING;
}

static void schedule_retry(PingProbe *p, enet_uint32 now)
{
    p->next_attempt = now + p->backoff_ms;
    p->attempt_wait = 1;
    p->backoff_ms = p->backoff_ms * 2 > 10000 ? 10000 : p->backoff_ms * 2;
}

static void try_connect(PingProbe *p, enet_uint32 now)
{
    if (!p->host) {
        p->host = enet_host_create(NULL, 1, CONNECT_CHANNELS, 0, 0);
        if (!p->host) {
            p->st.state = PING_ERROR;
            schedule_retry(p, now);
            return;
        }
        /* una stanza che manda roba enorme non ci riguarda: niente memoria sprecata */
        p->host->maximumPacketSize = 16 * 1024;
        p->host->maximumWaitingData = 32 * 1024;
    }
    if (!p->resolved) {
        ENetAddress a;
        memset(&a, 0, sizeof(a));
        if (enet_address_set_host(&a, p->target_host) != 0) {
            p->st.state = PING_ERROR;
            schedule_retry(p, now);
            return;
        }
        a.port = p->target_port;
        p->addr = a;
        p->resolved = 1;
    }
    p->peer = enet_host_connect(p->host, &p->addr, CONNECT_CHANNELS, 0);
    if (!p->peer) {
        p->st.state = PING_ERROR;
        schedule_retry(p, now);
        return;
    }
    /* Se la stanza non risponde, ENet lascia cadere la connessione dopo ~3 s (invece dei 5 s di default). */
    enet_peer_timeout(p->peer, 32, 3000, 8000);
    p->connect_started = now;
    p->attempt_wait = 0;
    ++p->st.attempts;
    p->st.state = PING_CONNECTING;
}

static void on_connect(PingProbe *p, ENetPeer *peer, enet_uint32 now)
{
    enet_uint32 handshake = ENET_TIME_DIFFERENCE(now, p->connect_started);
    if (peer != p->peer) { /* un estraneo: non lo vogliamo */
        enet_peer_reset(peer);
        return;
    }
    /* ENet parte da 500 ms e si avvicina di 1/8 a ogni campione: ci vorrebbero ~10 s. Si parte dal tempo
       dell'handshake (CONNECT -> VERIFY_CONNECT, circa un RTT), come fanno anche i client di 3DS Link. */
    /* (Non e' un campione per la media: se la stanza e' tornata dopo una pausa, i CONNECT rispediti fanno sembrare
       l'handshake lungo secondi. Il numero compare al primo vero ACK di un PING, circa 500 ms dopo.) */
    if (handshake < 1)
        handshake = 1;
    if (handshake > 500)
        handshake = 500;
    peer->roundTripTime = handshake;
    peer->roundTripTimeVariance = 0;
    peer->lastRoundTripTime = handshake;
    sample_clear(p);
    p->last_recv_seen = peer->lastReceiveTime;
    enet_peer_ping_interval(peer, p->interval_ms);
    p->backoff_ms = 1000;
    ++p->st.connects;
    p->st.state = PING_ONLINE;
}

unsigned ping_step(PingProbe *p, unsigned wait_ms)
{
    ENetEvent ev;
    enet_uint32 now;
    int served;

    if (!p)
        return wait_ms;
    now = enet_time_get();
    p->st.updated_ms = now;

    if (!p->has_target) {
        p->st.state = PING_NO_TARGET;
        return wait_ms;
    }

    if (!p->peer) {
        if (!p->attempt_wait || time_reached(now, p->next_attempt))
            try_connect(p, now);
        if (!p->peer) { /* in attesa del prossimo tentativo (o errore) */
            p->st.silence_ms = 0;
            p->st.rtt_ms = p->st.last_rtt_ms = -1;
            return wait_ms;
        }
    }

    served = enet_host_service(p->host, &ev, wait_ms);
    while (served > 0) {
        now = enet_time_get();
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            on_connect(p, ev.peer, now);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(ev.packet); /* non ci interessa nulla di quello che manda */
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (ev.peer == p->peer) {
                p->peer = NULL; /* ENet l'ha gia' liberato */
                p->st.rtt_ms = p->st.last_rtt_ms = -1;
                p->st.state = PING_CONNECTING;
                schedule_retry(p, now);
            }
            break;
        default:
            break;
        }
        served = enet_host_check_events(p->host, &ev);
    }

    /* aggiorna lo stato esposto */
    now = enet_time_get();
    p->st.updated_ms = now;
    if (p->peer && p->peer->state == ENET_PEER_STATE_CONNECTED) {
        unsigned silence = p->peer->lastReceiveTime ? ENET_TIME_DIFFERENCE(now, p->peer->lastReceiveTime) : 0;
        /* Dalla stanza non arriva altro che gli ACK dei nostri PING: ogni datagramma nuovo e' un campione, e
           lastRoundTripTime e' il suo RTT esatto (receivedSentTime). Vale anche per un PING rispedito dopo uno
           stallo: l'RTT smussato di ENet resterebbe sporco per decine di secondi, il campione no. */
        if (p->peer->lastReceiveTime != p->last_recv_seen) {
            p->last_recv_seen = p->peer->lastReceiveTime;
            sample_push(p, now, (int)p->peer->lastRoundTripTime);
        }
        p->st.silence_ms = silence;
        p->st.rtt_ms = sample_mean(p, now);
        p->st.enet_rtt_ms = (int)p->peer->roundTripTime;
        p->st.rtt_var_ms = (int)p->peer->roundTripTimeVariance;
        p->st.last_rtt_ms = (int)p->peer->lastRoundTripTime;
        p->st.state = silence > p->silence_limit_ms ? PING_NO_ANSWER : PING_ONLINE;
        if (silence > DROP_AFTER_SILENCE_MS) {
            /* La stanza e' sparita: ENet aspetterebbe un tempo che dipende dal suo RTT smussato. Si riparte. */
            drop_peer(p, 1);
            p->st.rtt_ms = p->st.last_rtt_ms = p->st.enet_rtt_ms = -1;
            p->st.state = PING_CONNECTING;
            schedule_retry(p, now);
        }
    } else {
        p->st.silence_ms = 0;
        if (p->st.state != PING_ERROR)
            p->st.state = PING_CONNECTING;
    }
    return 0;
}

void ping_status(const PingProbe *p, PingStatus *out)
{
    if (!out)
        return;
    if (!p) {
        memset(out, 0, sizeof(*out));
        out->rtt_ms = out->last_rtt_ms = -1;
        return;
    }
    *out = p->st;
}

void ping_destroy(PingProbe *p)
{
    if (!p)
        return;
    drop_peer(p, 1);
    if (p->host) {
        enet_host_flush(p->host);
        enet_host_destroy(p->host);
    }
    free(p);
    enet_deinitialize();
}
