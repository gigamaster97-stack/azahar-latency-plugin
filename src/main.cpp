// AzaharPing - mostra in sovrimpressione il ping verso la stanza multiplayer di Azahar. Plugin 3GX per Azahar.
// Licenza: GPL-2.0-or-later (vedi LICENSE). Usa ENet (MIT, third_party/enet) e CTRPluginFramework.
//
// Come funziona: un thread di rete apre una connessione ENet alla stanza (stesso indirizzo che si usa in Azahar)
// SENZA fare il join, quindi senza occupare un posto da giocatore (src/ping_core.c), e ENet misura l'RTT con un PING
// ogni 500 ms (mai piu' spesso). Il callback dell'OSD disegna il numero sullo schermo superiore.
#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "ping_core.h"
}
#include "uds_probe.hpp"
#include "uds_trace.hpp"

// libctru's hidInit() apre anche ir:rst su una New 3DS, e dentro al processo del gioco rompe il C-stick emulato di
// alcuni giochi. Al plugin non serve: (stesso accorgimento di 3DS Link).
extern "C" bool hidShouldUseIrrst(void) {
    return false;
}

namespace CTRPluginFramework {

namespace {

constexpr const char* kIniPath = "/azahar_ping.ini";
constexpr const char* kLogPath = "/azahar_ping.log";
constexpr bool kRoomProbe = false; // sonda ENet verso la stanza (src/ping_core.c): spenta, vedi NetThread
constexpr u32 kSocBlockSize = 0x20000; // il minimo che soc:U accetta con un buffer utile (come 3DS Link)

// ----------------------------------------------------------------------------------------------------------------
// Configurazione (azahar_ping.ini nella radice della SD virtuale)
// ----------------------------------------------------------------------------------------------------------------
struct Config {
    std::string host;      // nome o IPv4 della stanza; vuoto = non configurata
    int port = 24872;
    int interval_ms = 500; // ogni quanto ENet manda il PING (mai meno di 500 ms)
    int corner = 1;        // 0 alto-sx, 1 alto-dx, 2 basso-sx, 3 basso-dx (schermo superiore)
    bool show = true;
    bool debug = true;     // scrive azahar_ping.log
    int hookmode = 5;      // uds_hookmode: 5 = funzioni UDS dell'SDK (default); 0-4 = vecchi tentativi sugli svc (0 e 4 mandano in crash MH XX)
    bool names = true;    // mostra il nome del giocatore (dal nodo UDS) invece di P1, P2... quando il gioco lo dice
    bool players = true;  // mostra la latenza di ogni giocatore della partita (dai pacchetti UDS del gioco; aggancia il gioco)
    bool uds_trace = false; // diagnostica: registra in /azahar_uds_pkts.csv i pacchetti UDS che il gioco manda e riceve
    bool uds_diag = false; // diagnostica: scrive nel log lo stato della partita UDS (nodi, nomi, host); solo lettura
    int refresh_hz = 30;   // tetto degli aggiornamenti del numero a schermo (1-60); sotto questi fps segue gli fps
};

LightLock g_lock;          // protegge g_cfg e g_status
Config g_cfg;
volatile bool g_cfg_dirty = false;
PingStatus g_status;
azp::NodeRtt g_players[8];  // latenza per giocatore della partita UDS (uds_trace.cpp), aggiornata dal thread di rete
char g_names[9][32];        // nome di ogni nodo UDS (indice = numero di nodo 1..8), dal thread di rete
char g_net_note[64] = "";  // "" = rete ok; altrimenti il motivo per cui non c'e'
volatile bool g_stop = false;Thread g_thread = nullptr;
u32 g_soc_buffer = 0;

// ----------------------------------------------------------------------------------------------------------------
// Log su file (utile per capire cosa succede senza guardare lo schermo)
// ----------------------------------------------------------------------------------------------------------------
void Log(const char* fmt, ...) {
    if (!g_cfg.debug) {
        return;
    }
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    File f;
    if (File::Open(f, kLogPath, File::RWC | File::APPEND | File::SYNC) == 0) {
        f.WriteLine(line);
        f.Close();
    }
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// "host", "host:port" -> host/port. Restituisce false se la porta non e' valida.
bool ParseAddress(const std::string& text, std::string* host, int* port) {
    std::string t = Trim(text);
    size_t colon = t.rfind(':');
    if (colon != std::string::npos && t.find(':') == colon) { // un solo ':' = host:porta (IPv6 non supportato)
        int p = std::atoi(t.c_str() + colon + 1);
        if (p < 1 || p > 65535) {
            return false;
        }
        *port = p;
        t.erase(colon);
    }
    *host = t;
    return true;
}

void LoadConfig() {
    File f;
    if (File::Open(f, kIniPath, File::READ) != 0) {
        return;
    }
    const u64 size = f.GetSize();
    if (size == 0 || size > 8192) {
        f.Close();
        return;
    }
    std::string text(static_cast<size_t>(size), '\0');
    f.Read(&text[0], static_cast<u32>(size));
    f.Close();

    Config c;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        for (char& ch : key) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (key == "host" || key == "address") {
            int port = c.port;
            std::string h;
            if (ParseAddress(val, &h, &port)) { c.host = h; c.port = port; }
        } else if (key == "port") {
            int p = std::atoi(val.c_str());
            if (p >= 1 && p <= 65535) c.port = p;
        } else if (key == "update_ms") {
            int v = std::atoi(val.c_str());
            if (v >= 1 && v <= 5000) c.interval_ms = v < PING_MIN_INTERVAL_MS ? PING_MIN_INTERVAL_MS : v;
        } else if (key == "corner") {
            int v = std::atoi(val.c_str());
            if (v >= 0 && v <= 3) c.corner = v;
        } else if (key == "uds_hookmode") {
            c.hookmode = std::atoi(val.c_str());
        } else if (key == "names") {
            c.names = !(val == "0" || val == "false" || val == "off" || val == "no");
        } else if (key == "players") {
            c.players = !(val == "0" || val == "false" || val == "off" || val == "no");
        } else if (key == "uds_trace") {
            c.uds_trace = (val == "1" || val == "true" || val == "on" || val == "yes");
        } else if (key == "uds_diag") {
            c.uds_diag = (val == "1" || val == "true" || val == "on" || val == "yes");
        } else if (key == "refresh_hz") {
            int v = std::atoi(val.c_str());
            if (v >= 1 && v <= 60) c.refresh_hz = v;
        } else if (key == "show") {
            c.show = !(val == "0" || val == "false" || val == "off" || val == "no");
        } else if (key == "debug") {
            c.debug = !(val == "0" || val == "false" || val == "off" || val == "no");
        }
    }
    g_cfg = c;
}

void SaveConfig() {
    File f;
    if (File::Open(f, kIniPath, File::RWC | File::TRUNCATE | File::SYNC) != 0) {
        return;
    }
    f.Clear();
    char line[160];
    f.WriteLine("; AzaharPing - impostazioni");
    f.WriteLine("; host = indirizzo (o nome) della stanza di Azahar, come lo scrivi in Multiplayer > Collegati a una stanza");
    std::snprintf(line, sizeof(line), "host = %s", g_cfg.host.c_str());
    f.WriteLine(line);
    std::snprintf(line, sizeof(line), "port = %d", g_cfg.port);
    f.WriteLine(line);
    f.WriteLine("; ogni quanti millisecondi ENet manda il PING alla stanza (500-5000; mai piu' spesso di ogni 500 ms)");
    std::snprintf(line, sizeof(line), "update_ms = %d", g_cfg.interval_ms);
    f.WriteLine(line);
    f.WriteLine("; angolo dello schermo superiore: 0 alto-sx, 1 alto-dx, 2 basso-sx, 3 basso-dx");
    std::snprintf(line, sizeof(line), "corner = %d", g_cfg.corner);
    f.WriteLine(line);
    f.WriteLine("; tetto degli aggiornamenti del numero a schermo (1-60 al secondo). Con il gioco sopra questi fps lo stesso");
    f.WriteLine("; valore resta su piu' frame; sotto, il numero si aggiorna a ogni frame (segue gli fps e la velocita' dell'emulazione)");
    std::snprintf(line, sizeof(line), "refresh_hz = %d", g_cfg.refresh_hz);
    f.WriteLine(line);
    std::snprintf(line, sizeof(line), "show = %d", g_cfg.show ? 1 : 0);
    f.WriteLine(line);
    f.WriteLine("; debug = 1 scrive azahar_ping.log nella radice della SD virtuale");
    std::snprintf(line, sizeof(line), "debug = %d", g_cfg.debug ? 1 : 0);
    f.WriteLine(line);
    f.WriteLine("; players = 1: latenza di ogni giocatore della partita, ricavata dai pacchetti UDS del gioco (oggi: Kirby Battle Royale)");
    std::snprintf(line, sizeof(line), "players = %d", g_cfg.players ? 1 : 0);
    f.WriteLine(line);
    f.WriteLine("; uds_trace = 1: registra in azahar_uds_pkts.csv i pacchetti UDS che il gioco manda (S) e riceve (R)");
    std::snprintf(line, sizeof(line), "uds_trace = %d", g_cfg.uds_trace ? 1 : 0);
    f.WriteLine(line);
    f.WriteLine("; uds_diag = 1: nel log lo stato della partita UDS del gioco (chi e' l'host, nomi dei giocatori)");
    std::snprintf(line, sizeof(line), "uds_diag = %d", g_cfg.uds_diag ? 1 : 0);
    f.WriteLine(line);
    f.Close();
}

// ----------------------------------------------------------------------------------------------------------------
// Memoria per soc:U
// ----------------------------------------------------------------------------------------------------------------
// socInit() vuole un blocco di memoria "normale" dello heap del processo, allineato a 4 KB. Lo si prende in cima
// all'area dello heap (0x08000000-0x0E000000, 96 MiB), dove lo heap del gioco non arriva (la cosa che 3DS Link
// dimostra su console; qui sotto Azahar e' piu' permissivo, ma e' lo stesso posto). Si fa in PatchProcess, prima che
// il gioco parta.
u32 ReserveSocketMemory(u32 size) {
    constexpr u32 kHeapEnd = 0x0E000000;
    for (u32 i = 0; i < 24; ++i) {
        const u32 base = kHeapEnd - size - i * 0x100000;
        MemInfo info{};
        PageInfo page;
        if (R_FAILED(svcQueryMemory(&info, &page, base)) || info.state != MEMSTATE_FREE ||
            static_cast<u64>(info.base_addr) + info.size < static_cast<u64>(base) + size) {
            continue;
        }
        u32 out = 0;
        const Result rc = svcControlMemory(&out, base, 0, size, MEMOP_ALLOC,
                                           static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
        if (R_SUCCEEDED(rc) && out == base) {
            return out;
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------------------------------------------
// Thread di rete
// ----------------------------------------------------------------------------------------------------------------
void SetNote(const char* text) {
    LightLock_Lock(&g_lock);
    std::snprintf(g_net_note, sizeof(g_net_note), "%s", text);
    LightLock_Unlock(&g_lock);
}

void NetThread(void*) {
    LightLock_Lock(&g_lock);
    Config cfg = g_cfg;
    g_cfg_dirty = false;
    LightLock_Unlock(&g_lock);

    // La sonda verso la stanza (ENet) e' spenta (kRoomProbe = false): il riferimento di latenza e' quello verso l'host della
    // partita, ricavato dai pacchetti del gioco (scelta dell'utente: niente IP, niente socket, niente memoria per soc:U).
    PingProbe* probe = nullptr;
    bool soc_ok = false;
    if (kRoomProbe && !cfg.host.empty()) {
        if (!g_soc_buffer) {
            SetNote("no memory");
            Log("net: no memory block for soc:U");
        } else {
            const Result src = socInit(reinterpret_cast<u32*>(g_soc_buffer), kSocBlockSize);
            if (R_FAILED(src)) {
                char note[64];
                std::snprintf(note, sizeof(note), "socInit 0x%08lX", static_cast<unsigned long>(src));
                SetNote(note);
                Log("net: socInit failed 0x%08lX", static_cast<unsigned long>(src));
            } else {
                soc_ok = true;
                probe = ping_create(static_cast<unsigned>(cfg.interval_ms), 1500);
                if (!probe) {
                    SetNote("enet");
                    socExit();
                    soc_ok = false;
                } else {
                    ping_set_target(probe, cfg.host.c_str(), static_cast<unsigned short>(cfg.port));
                    Log("net: room probe on '%s':%d", cfg.host.c_str(), cfg.port);
                }
            }
        }
    } else {
        Log("net: no room probe (latency to the match host comes from the game's UDS packets)");
    }

    PingState last_state = static_cast<PingState>(-1);
    u64 last_report = 0;
    u64 uds_last_ms = 0, uds_report_ms = 0, trace_flush_ms = 0;
    std::string trace_acc;
    u64 inline_log_ms = 0, rtt_log_ms = 0, names_ms = 0, proto_log_ms = 0;
    bool uds_open = false, uds_open_logged = false;
    char uds_me[16] = "?", uds_sig[200] = "";
    while (!g_stop) {
        if (g_cfg_dirty) {
            LightLock_Lock(&g_lock);
            Config now_cfg = g_cfg;
            g_cfg_dirty = false;
            LightLock_Unlock(&g_lock);
            if (probe && (now_cfg.host != cfg.host || now_cfg.port != cfg.port)) {
                ping_set_target(probe, now_cfg.host.c_str(), static_cast<unsigned short>(now_cfg.port));
                Log("net: new target '%s':%d", now_cfg.host.c_str(), now_cfg.port);
            }
            cfg = now_cfg;
        }
        unsigned idle = 20;
        PingStatus st;
        if (probe) {
            idle = ping_step(probe, 20);
            ping_status(probe, &st);
        } else {
            ping_status(nullptr, &st); // vuoto: stato 0, rtt -1
        }
        LightLock_Lock(&g_lock);
        g_status = st;
        LightLock_Unlock(&g_lock);

        const u64 now = svcGetSystemTick() / 268112; // ms (tick ARM11)
        if (st.state != last_state || (cfg.debug && now - last_report >= 5000)) {
            Log("net: state %d rtt=%d last=%d enet=%d silence=%u connects=%u attempts=%u", static_cast<int>(st.state),
                st.rtt_ms, st.last_rtt_ms, st.enet_rtt_ms, st.silence_ms, st.connects, st.attempts);
            last_state = st.state;
            last_report = now;
        }
        // --- diagnostica UDS (solo se uds_diag = 1): ogni 500 ms legge i nodi della partita, scrive solo i cambiamenti
        if (cfg.uds_diag && now - uds_last_ms >= 500) {
            uds_last_ms = now;
            if (!uds_open) {
                uds_open = azp::UdsOpen();
                if (uds_open) {
                    char me[16] = "";
                    const bool ok = azp::UdsMyName(me, sizeof(me));
                    std::snprintf(uds_me, sizeof(uds_me), "%s", ok ? me : "?");
                    Log("uds: nwm::UDS open, my username '%s'", uds_me);
                } else if (!uds_open_logged) {
                    uds_open_logged = true;
                    Log("uds: nwm::UDS not available yet");
                }
            }
            azp::UdsSnapshot snap;
            if (uds_open && azp::UdsRead(&snap)) {
                char sig[200] = "";
                int len = 0;
                for (int i = 0; i < snap.count && len < 150; ++i) {
                    len += std::snprintf(sig + len, sizeof(sig) - len, "%u:%s:%08lX%08lX ", snap.nodes[i].id,
                                         snap.nodes[i].name, static_cast<unsigned long>(snap.nodes[i].seed >> 32),
                                         static_cast<unsigned long>(snap.nodes[i].seed & 0xFFFFFFFFu));
                }
                if (snap.count == 0) {
                    std::snprintf(sig, sizeof(sig), "none (err %08lX)", static_cast<unsigned long>(snap.first_error));
                }
                if (std::strcmp(sig, uds_sig) != 0 || now - uds_report_ms >= 5000) {
                    int me_id = 0;
                    for (int i = 0; i < snap.count; ++i) {
                        if (std::strcmp(snap.nodes[i].name, uds_me) == 0) me_id = snap.nodes[i].id;
                    }
                    Log("uds: nodes=%d host=%s me=node%d room_rtt=%d | %s", snap.count,
                        snap.count > 0 ? snap.nodes[0].name : "-", me_id, st.rtt_ms, sig);
                    std::snprintf(uds_sig, sizeof(uds_sig), "%s", sig);
                    uds_report_ms = now;
                }
            }
        }
        if (cfg.players || cfg.uds_trace) {
            // i pacchetti UDS del gioco: sempre allo stimatore; nel CSV solo con uds_trace, scrivendo a blocchi ogni 250 ms
            static char trace_buf[24000];
            const bool csv = cfg.uds_trace;
            const int n = azp::UdsTracePump(csv ? trace_buf : nullptr, sizeof(trace_buf));
            if (n > 0) {
                trace_acc.append(trace_buf, static_cast<size_t>(n));
            }
            if (!trace_acc.empty() && now - trace_flush_ms >= 250) {
                trace_flush_ms = now;
                File tf;
                if (File::Open(tf, "/azahar_uds_pkts.csv", File::RWC | File::APPEND | File::SYNC) == 0) {
                    tf.Write(trace_acc.data(), static_cast<u32>(trace_acc.size()));
                    tf.Close();
                }
                trace_acc.clear();
            }
            if (cfg.debug && now - inline_log_ms >= 10000) {
                inline_log_ms = now;
                char st_buf[200];
                azp::UdsTraceInlineStats(st_buf, sizeof(st_buf));
                Log("uds: %s", st_buf);
            }
            // nomi dei giocatori (GetNodeInformation, sola lettura), ogni secondo: servono per mostrare "Player 58 ms" invece di "P1"
            if (cfg.players && cfg.names && now - names_ms >= 1000) {
                names_ms = now;
                if (azp::UdsOpen()) {
                    azp::UdsSnapshot sn;
                    char nm[9][32];
                    std::memset(nm, 0, sizeof(nm));
                    if (azp::UdsRead(&sn)) {
                        for (int i = 0; i < sn.count; ++i) {
                            const u16 id = sn.nodes[i].id;
                            if (id >= 1 && id <= 8) {
                                std::snprintf(nm[id], sizeof(nm[id]), "%s", sn.nodes[i].name);
                            }
                        }
                    }
                    LightLock_Lock(&g_lock);
                    std::memcpy(g_names, nm, sizeof(g_names));
                    LightLock_Unlock(&g_lock);
                }
            }
            if (cfg.debug && now - proto_log_ms >= 10000) { // che protocollo usa il gioco: serve per i log mandati da altri
                proto_log_ms = now;
                char pr[400];
                if (azp::UdsProtoSummary(pr, sizeof(pr)) > 0) {
                    Log("%s", pr);
                }
            }
            azp::NodeRtt nr[8];
            azp::UdsRttSnapshot(nr);
            if (cfg.debug && now - rtt_log_ms >= 5000) { // per controllare senza guardare lo schermo: cosa mostra l'overlay
                rtt_log_ms = now;
                char pb[160];
                int len = 0;
                pb[0] = 0;
                for (int i = 0; i < 8 && len < 120; ++i) {
                    if (nr[i].age_ms >= 0) {
                        len += std::snprintf(pb + len, sizeof(pb) - len, "n%d=%dms(%d)%s%s owd=%d ", nr[i].node, nr[i].rtt_ms,
                                             nr[i].samples, nr[i].estimated ? "est" : "", nr[i].spike ? "!" : "", nr[i].owd_ms); // "!" = gli ultimi 2 s, non la finestra
                    }
                }
                if (len > 0) {
                    Log("uds: players %s", pb);
                }
            }
            LightLock_Lock(&g_lock);
            std::memcpy(g_players, nr, sizeof(g_players));
            LightLock_Unlock(&g_lock);
        }
        if (idle) {
            svcSleepThread(static_cast<s64>(idle > 50 ? 50 : idle) * 1000000LL);
        }
    }
    azp::UdsClose();
    if (probe) {
        ping_destroy(probe); // manda il DISCONNECT: la stanza libera il peer subito
    }
    if (soc_ok) {
        socExit();
    }
    Log("net: stopped");
}

// ----------------------------------------------------------------------------------------------------------------
// Disegno
// ----------------------------------------------------------------------------------------------------------------
// Colori sul valore mostrato (ora e' il giro completo fra due giocatori, ricavato dai pacchetti del gioco: non c'e' piu' da
// raddoppiare niente). Blu fino a 30, verde 31-60, giallo 61-100, arancione 101-150, rosso oltre.
Color PingColor(int v) {
    if (v <= 30) return Color(80, 170, 255);
    if (v <= 60) return Color(80, 220, 110);
    if (v <= 100) return Color(250, 220, 60);
    if (v <= 150) return Color(255, 150, 40);
    return Color(255, 70, 70);
}

// Colore di ogni giocatore secondo il suo NODO (scelta dell'utente, la stessa di 3DS Link): 1 azzurro (l'host), 2 verde, 3 giallo, 4 arancione,
// 5 rosso, 6 viola, 7 bianco, 8 fucsia. Il nome del giocatore ha il colore del suo nodo, il valore quello della scala della latenza.
Color NodeColor(int node) {
    static const Color c[8] = {Color(70, 178, 255), Color(74, 218, 128), Color(255, 198, 88), Color(255, 140, 56),
                               Color(255, 72, 72),  Color(186, 140, 255), Color(255, 255, 255), Color(255, 90, 210)};
    return c[(node >= 1 && node <= 8 ? node : 1) - 1];
}

// Un nome troppo lungo si taglia come i messaggi della chat di 3DS Link: i primi 9 caratteri e "..." (12 al massimo), senza lasciare
// uno spazio prima dei puntini. Nel log il nome resta intero.
std::string CutName(const std::string& s) {
    if (s.size() <= 12) {
        return s;
    }
    std::string cut = s.substr(0, 9);
    while (!cut.empty() && cut.back() == ' ') {
        cut.pop_back();
    }
    return cut + "...";
}

// Millisecondi dell'orologio dell'HOST (tempo reale, non influenzato dalla velocita' dell'emulazione); se manca, il tick del guest.
u64 HostMs() {
    s64 ns = 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&ns, 0x20000, 1)) && ns > 0) {
        return static_cast<u64>(ns) / 1000000ULL;
    }
    return svcGetSystemTick() / 268112ULL;
}

bool DrawPing(const Screen& scr) {
    if (!scr.IsTop) {
        return false;
    }
    if (LightLock_TryLock(&g_lock) != 0) {
        return false; // il thread di rete sta copiando: un fotogramma senza numero non fa male
    }
    const Config cfg = g_cfg;
    const PingStatus latest = g_status;
    azp::NodeRtt players_latest[8];
    std::memcpy(players_latest, g_players, sizeof(players_latest));
    char names_latest[9][32];
    std::memcpy(names_latest, g_names, sizeof(names_latest));
    char note[64];
    std::snprintf(note, sizeof(note), "%s", g_net_note);
    LightLock_Unlock(&g_lock);

    // Il callback gira una volta per fotogramma del gioco. Il numero mostrato si ricalcola al massimo refresh_hz volte al
    // secondo (di default 30): con il gioco a 60 fps lo stesso valore resta su due frame; con il gioco a 30 fps o meno
    // (o con l'emulazione piu' lenta del 100%) il tempo reale tra due frame supera l'intervallo e il numero si aggiorna a
    // ogni frame, cioe' segue gli fps. Il tempo e' quello dell'host, quindi la velocita' dell'emulazione e' gia' dentro.
    // (Il -3 ms tiene conto del jitter: un frame a 60 fps puo' arrivare a 33,2 ms invece di 33,3.)
    static PingStatus shown;
    static azp::NodeRtt shown_players[8];
    static bool have_shown = false;
    static u64 last_update_ms = 0;
    static u32 frames = 0, updates = 0;
    static u64 report_ms = 0;
    const u64 now_ms = HostMs();
    const u64 gate_ms = 1000u / static_cast<u64>(cfg.refresh_hz < 1 ? 1 : cfg.refresh_hz) - 3;
    ++frames;
    if (!have_shown || now_ms - last_update_ms >= gate_ms) {
        shown = latest;
        std::memcpy(shown_players, players_latest, sizeof(shown_players));
        have_shown = true;
        last_update_ms = now_ms;
        ++updates;
    }
    if (cfg.debug && now_ms - report_ms >= 5000) {
        if (report_ms != 0) {
            const unsigned long span = static_cast<unsigned long>(now_ms - report_ms);
            const unsigned long fps10 = frames * 10000UL / span, ups10 = updates * 10000UL / span; // decimi (niente float nel printf)
            Log("draw: %lu frames, %lu updates in %lu ms (%lu.%lu fps, %lu.%lu updates/s, gate %lu ms)",
                static_cast<unsigned long>(frames), static_cast<unsigned long>(updates), span, fps10 / 10, fps10 % 10,
                ups10 / 10, ups10 % 10, static_cast<unsigned long>(gate_ms));
        }
        report_ms = now_ms;
        frames = updates = 0;
    }
    const PingStatus& st = shown;

    if (!cfg.show) {
        return false;
    }
    // Riferimento di latenza dell'utente = la latenza verso chi fa da HOST della partita (nodo 1), ricavata dai pacchetti del
    // gioco: e' la riga principale "PING n". Sotto, gli altri giocatori ("P2 36 ms", ...). Se non c'e' il nodo 1 tra i giocatori
    // da cui ricevo pacchetti, l'host sono io: "PING HOST". Fuori da una partita non si disegna nulla.
    // (scelta dell'utente) L'host NON vede la riga "PING": vede solo i giocatori, numerati P1, P2, ... fino al numero dei giocatori
    // connessi meno la propria sessione; ogni riga porta il NOME del giocatore se il gioco lo dice (nome del nodo UDS) e se e' unico.
    // Un client ha in piu' la riga principale "PING n" = giro con l'host (nodo 1).
    const azp::NodeRtt& host_row = shown_players[0];
    int others = 0;
    for (int i = 0; i < 8; ++i) {
        if (shown_players[i].age_ms >= 0) ++others;
    }
    const bool in_match = cfg.players && others > 0;
    const bool i_am_host = in_match && host_row.age_ms < 0; // nessun pacchetto dal nodo 1: l'host sono io
    std::string text;
    Color fg = Color(160, 160, 160);
    bool have_main = true;
    if (note[0]) {
        text = std::string("PING n/a (") + note + ")";
        fg = Color(255, 120, 120);
    } else if (in_match) {
        have_main = false; // (scelta dell'utente) niente riga "PING": c'e' gia' il valore di ogni giocatore, l'host e' segnato con "H"
        (void)host_row;
        (void)i_am_host;
    } else if (kRoomProbe && st.state == PING_ONLINE && st.rtt_ms >= 0) {
        char num[24];
        std::snprintf(num, sizeof(num), "ROOM %d", st.rtt_ms);
        text = num;
        fg = PingColor(st.rtt_ms * 2); // (ramo ROOM: giro verso la stanza, conteggiato doppio come prima; la sonda e' spenta)
    } else {
        return false; // niente partita: niente riquadro
    }

    std::string lines[8];
    Color line_color[8];
    std::string part_name[8], part_val[8]; // la riga divisa: nome (colore del nodo) e valore (colore della scala)
    int line_node[8], line_rtt[8];
    int line_count = 0;
    if (in_match) {
        // nomi: se piu' giocatori mostrati hanno lo stesso nome (in Kirby i salvataggi copiati danno "Kami-pc" a tutti) si
        // aggiunge il numero di nodo; senza nome si usa P<k>, con k = 1, 2, ... in ordine di nodo.
        int shown_count = 0;
        for (int i = 0; i < 8; ++i) {
            if (shown_players[i].age_ms >= 0) ++shown_count;
        }
        int k = 0;
        for (int i = 0; i < 8; ++i) {
            const azp::NodeRtt& p = shown_players[i];
            if (p.age_ms < 0) {
                continue; // nessun pacchetto recente da questo nodo
            }
            ++k;
            std::string label = "P" + std::to_string(k);
            const char* nm = cfg.names ? names_latest[p.node] : "";
            if (nm[0]) {
                int same = 0;
                for (int j = 0; j < 8; ++j) {
                    if (shown_players[j].age_ms >= 0 && std::strcmp(names_latest[shown_players[j].node], nm) == 0) ++same;
                }
                label = same > 1 ? std::string(nm) + "#" + std::to_string(p.node) : std::string(nm);
            }
            (void)shown_count;
            label = CutName(label);
            if (p.node == 1) {
                label = "(H)" + label; // il nodo 1 e' chi ospita la partita
            }
            char buf[64];
            if (p.rtt_ms >= 0 && p.stale) {
                std::snprintf(buf, sizeof(buf), "%s (%dms)", label.c_str(), p.rtt_ms); // tra parentesi (e grigio) = ultimo valore noto, non piu' misurato. Il carattere "~" manca nel font del 3DS e si vede come un quadratino
                line_color[line_count] = Color(150, 150, 160);
            } else if (p.rtt_ms >= 0) {
                std::snprintf(buf, sizeof(buf), "%s %dms%s", label.c_str(), p.rtt_ms, p.spike ? "!" : ""); // "!" = gli ultimi 2 s, chiaramente peggiori della finestra
                line_color[line_count] = PingColor(p.rtt_ms);
            } else {
                std::snprintf(buf, sizeof(buf), "%s --", label.c_str());
                line_color[line_count] = Color(160, 160, 160);
            }
            lines[line_count] = buf;
            part_name[line_count] = label;
            part_val[line_count] = lines[line_count].substr(label.size()); // (" 28ms", " (28ms)" o " --")
            line_node[line_count] = p.node;
            line_rtt[line_count] = p.rtt_ms;
            ++line_count;
        }
    }

    if (cfg.debug && in_match) { // per controllare cosa si disegna senza guardare lo schermo (ogni 5 s)
        static u64 dbg_ms = 0;
        if (now_ms - dbg_ms >= 5000) {
            dbg_ms = now_ms;
            char db[200];
            int dl = 0;
            db[0] = 0;
            for (int i = 0; i < line_count && dl < 150; ++i) {
                dl += std::snprintf(db + dl, sizeof(db) - dl, "[%s] ", lines[i].c_str());
            }
            Log("overlay: %s| names n1=%s n2=%s n3=%s n4=%s", db, names_latest[1], names_latest[2], names_latest[3], names_latest[4]);
        }
    }

    // Fino a 4 giocatori nella lobby (3 altri): una riga per giocatore, "Nome 28ms". Con PIU' di 4 (un gioco tipo Tetris ne tiene fino a 8) i nomi non
    // stanno: una riga sola di quadratini attaccati, uno per ogni altro giocatore (mai il mio), ognuno del colore della sua latenza, con una H nera
    // per l'host e sotto un rettangolino del colore del suo nodo (il colore con cui si riconosce chi e' chi). Il log ha sempre tutti i nomi.
    const bool tiles = line_count > 3;
    const int tile_w = 16, tile_h = 16, bar_h = 3;
    const int h = have_main ? 20 : 0, line_h = 18;
    const int total_h = h + (tiles ? tile_h + bar_h : line_count * line_h);
    const int w = static_cast<int>(OSD::GetTextWidth(true, text)) + 10;
    const int y0 = (cfg.corner & 2) ? 240 - total_h - 3 : 3;
    int x = (cfg.corner & 1) ? 400 - w - 3 : 3;
    // Niente riquadro scuro: solo il testo, con un'ombra nera di 1 pixel (senza si legge male sulle scene chiare).
    if (have_main) {
        scr.DrawSysfont(text, x + 6, y0 + 3, Color(0, 0, 0));
        scr.DrawSysfont(text, x + 5, y0 + 2, fg);
    }
    if (tiles) {
        const int tx0 = (cfg.corner & 1) ? 400 - line_count * tile_w - 3 : 3;
        const int ty = y0 + h;
        for (int i = 0; i < line_count; ++i) {
            const int tx = tx0 + i * tile_w;
            scr.DrawRect(tx, ty, tile_w, tile_h, line_rtt[i] >= 0 ? PingColor(line_rtt[i]) : Color(150, 150, 160), true);
            if (line_node[i] == 1) {
                scr.DrawSysfont("H", tx + 4, ty - 1, Color(0, 0, 0)); // l'host
            }
            scr.DrawRect(tx, ty + tile_h, tile_w, bar_h, NodeColor(line_node[i]), true);
        }
    } else {
        for (int i = 0; i < line_count; ++i) {
            const int lw = static_cast<int>(OSD::GetTextWidth(true, lines[i])) + 10;
            const int lx = (cfg.corner & 1) ? 400 - lw - 3 : 3;
            const int ly = y0 + h + i * line_h;
            const int nw = static_cast<int>(OSD::GetTextWidth(true, part_name[i]));
            scr.DrawSysfont(lines[i], lx + 6, ly + 2, Color(0, 0, 0));
            scr.DrawSysfont(part_name[i], lx + 5, ly + 1, NodeColor(line_node[i]));
            scr.DrawSysfont(part_val[i], lx + 5 + nw, ly + 1, line_color[i]);
        }
    }    return true;
}

// ----------------------------------------------------------------------------------------------------------------
// Menu (Select)
// ----------------------------------------------------------------------------------------------------------------
[[maybe_unused]] void EntryRoom(MenuEntry*) { // (non piu' nel menu: la stanza non serve)
    std::string current = g_cfg.host;
    if (!current.empty() && g_cfg.port != 24872) {
        current += ":" + std::to_string(g_cfg.port);
    }
    Keyboard kb("Room address (host or host:port)");
    kb.SetMaxLength(100);
    std::string input;
    if (kb.Open(input, current) != 0) {
        return;
    }
    std::string host;
    int port = 24872;
    if (!ParseAddress(input, &host, &port)) {
        MessageBox("Invalid port")();
        return;
    }
    LightLock_Lock(&g_lock);
    g_cfg.host = host;
    g_cfg.port = port;
    g_cfg_dirty = true;
    LightLock_Unlock(&g_lock);
    SaveConfig();
    OSD::Notify(host.empty() ? "Room cleared" : "Room: " + host + ":" + std::to_string(port));
}

void EntryCorner(MenuEntry*) {
    LightLock_Lock(&g_lock);
    g_cfg.corner = (g_cfg.corner + 1) & 3;
    LightLock_Unlock(&g_lock);
    SaveConfig();
}

void EntryShow(MenuEntry*) {
    LightLock_Lock(&g_lock);
    g_cfg.show = !g_cfg.show;
    LightLock_Unlock(&g_lock);
    SaveConfig();
    OSD::Notify(g_cfg.show ? "Ping shown" : "Ping hidden");
}

} // namespace

// Chiamata prima di main e prima che il gioco parta: il momento in cui si puo' prendere memoria dello heap.
void PatchProcess(FwkSettings& settings) {
    settings.AllowActionReplay = false; // niente trucchi / ricerca memoria del framework: qui non servono
    settings.AllowSearchEngine = false;
    LightLock_Init(&g_lock);
    std::memset(&g_status, 0, sizeof(g_status));
    g_status.rtt_ms = g_status.last_rtt_ms = g_status.enet_rtt_ms = -1;
    if (kRoomProbe) {
        g_soc_buffer = ReserveSocketMemory(kSocBlockSize);
    }
}

void OnProcessExit(void) {}

static void OnProcessEvent(Process::Event event) {
    if (event == Process::Event::EXIT) {
        g_stop = true;
    }
}

int main(void) {
    LoadConfig();
    Log("---- AzaharPing start (build %s %s): host '%s' port %d, soc buffer %08lX", __DATE__, __TIME__, g_cfg.host.c_str(),
        g_cfg.port, static_cast<unsigned long>(g_soc_buffer));
    Process::SetProcessEventCallback(OnProcessEvent);
    if (g_cfg.uds_trace || g_cfg.players) {
        const bool ok = azp::UdsTraceStart(g_cfg.hookmode);
        Log("uds: %s, %d svc stub hooked in the game", ok ? "packet observer on" : "NOT started (stub not found)",
            azp::UdsTraceHooks());
        char sites[500];
        if (azp::UdsTraceDescribeSites(sites, sizeof(sites)) > 0) {
            Log("uds: svc 0x32 sites: %s", sites);
        }
    }

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(NetThread, nullptr, 0x8000, prio - 1, -2, false);
    if (!g_thread) {
        Log("net: threadCreate failed");
        SetNote("thread");
    }

    OSD::Run(DrawPing);

    PluginMenu* menu = new PluginMenu("AzaharPing", 1, 0, 0,
                                      "Shows your latency to the host of the match, and to the other players,\n"
                                      "measured from the game's own UDS packets. No setup needed.");
    menu->SynchronizeWithFrame(true);
    menu->ShowWelcomeMessage(false);
    menu->SetHexEditorState(false);
    menu->Append(new MenuEntry("Move to next corner", nullptr, EntryCorner));
    menu->Append(new MenuEntry("Show / hide ping", nullptr, EntryShow));
    menu->Run();

    g_stop = true;
    if (g_thread) {
        threadJoin(g_thread, 800000000ULL); // il framework aspetta ~1 s dal ritorno di main
    }
    return 0;
}

} // namespace CTRPluginFramework
