/*
 * ping_core - misura del ping verso una stanza multiplayer di Azahar, con ENet.
 *
 * Idea: ci si collega alla stanza come fa qualunque client (connessione ENet), ma senza mai inviare la
 * richiesta di "join": per la stanza la connessione non e' un membro (non occupa un posto da giocatore,
 * non compare nell'elenco, non riceve i pacchetti degli altri). ENet manda da solo un PING con ACK ogni
 * `ping_interval_ms` e ne ricava l'RTT (peer->roundTripTime), lo stesso valore che calcola il client di
 * Azahar sulla propria connessione.
 *
 * Nessuna dipendenza dalla piattaforma oltre a ENet (che ha il proprio strato: unix.c su PC, enet_3ds.c
 * sulla console). Un solo thread deve chiamare ping_step(); ping_status() si puo' chiamare da un altro.
 *
 * Licenza: GPL-2.0-or-later (come il resto del progetto).
 */
#ifndef PING_CORE_H
#define PING_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PING_NO_TARGET = 0, /* nessun indirizzo configurato */
    PING_CONNECTING,    /* handshake ENet in corso (o in attesa del prossimo tentativo) */
    PING_ONLINE,        /* connesso e la stanza risponde */
    PING_NO_ANSWER,     /* connesso, ma nessun datagramma dalla stanza da piu' di silence_limit_ms */
    PING_ERROR          /* indirizzo non risolvibile / socket non creato */
} PingState;

typedef struct {
    PingState state;
    int rtt_ms;            /* media dei campioni grezzi (un ACK ciascuno) degli ultimi 2 s; -1 se non ce ne sono */
    int enet_rtt_ms;       /* peer->roundTripTime di ENet (smussato 1/8), per confronto; -1 se non c'e' */
    int rtt_var_ms;        /* varianza di ENet */
    int last_rtt_ms;       /* ultimo campione grezzo; -1 se non c'e' */
    unsigned silence_ms;   /* da quanto non arriva nulla dalla stanza (0 se non connesso) */
    unsigned connects;     /* connessioni riuscite dall'avvio */
    unsigned attempts;     /* tentativi di connessione dall'avvio */
    unsigned long long updated_ms; /* orologio ENet (ms) dell'ultimo aggiornamento di questa struttura */
} PingStatus;

typedef struct PingProbe PingProbe;

#define PING_MIN_INTERVAL_MS 500 /* il PING non viene mai chiesto piu' spesso di cosi' */

/* Crea la sonda. ping_interval_ms: ogni quanto ENet manda il suo PING; minimo (e default) 500: valori piu' bassi
   vengono portati a 500.
   silence_limit_ms: dopo quanto senza risposte lo stato diventa PING_NO_ANSWER (1500 consigliato).
   Chiama enet_initialize(). NULL se fallisce. */
PingProbe *ping_create(unsigned ping_interval_ms, unsigned silence_limit_ms);

/* Imposta (o cambia) la stanza. host: nome o IPv4 puntato. Una connessione in corso viene chiusa con un
   DISCONNECT (la stanza libera subito il peer). host NULL o vuoto = nessuna stanza. Non risolve il nome
   (lo fa ping_step, sul thread di rete). */
void ping_set_target(PingProbe *p, const char *host, unsigned short port);

/* Fa lavorare ENet per al massimo wait_ms (puo' tornare prima): invia i PING, riceve gli ACK, gestisce
   riconnessioni (attesa crescente 1 s -> 10 s fra un tentativo fallito e il successivo).
   Ritorna 0 se ha atteso da se' (dentro ENet); altrimenti i ms che il chiamante deve dormire prima di
   richiamarla (nessuna stanza, o in attesa del prossimo tentativo): cosi' il thread non gira a vuoto. */
unsigned ping_step(PingProbe *p, unsigned wait_ms);

/* Copia lo stato (ultimo aggiornamento di ping_step). */
void ping_status(const PingProbe *p, PingStatus *out);

/* Chiude la connessione (con DISCONNECT immediato) e libera tutto. */
void ping_destroy(PingProbe *p);

#ifdef __cplusplus
}
#endif

#endif
