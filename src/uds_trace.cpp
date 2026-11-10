#include "uds_trace.hpp"

#include <CTRPluginFramework.hpp>

#include <cstdio>
#include <cstring>

namespace azp {

namespace {

using namespace CTRPluginFramework;

constexpr u32 kSvcSendSyncRequest = 0xEF000032; // svc 0x32
constexpr u32 kBxLr = 0xE12FFF1E;
constexpr int kMaxHooks = 4;
constexpr int kRing = 1024;
constexpr int kBytes = 128; // primi byte del contenuto che si registrano

struct Rec {
    volatile u32 seq; // (numero del record + 1) quando e' completo
    u64 t_us;
    u8 dir;           // 'S' | 'R'
    u8 node;
    u8 chan;
    u8 len;           // byte copiati in data
    u32 size;
    u8 data[kBytes];
};

Hook g_hooks[kMaxHooks]; // (solo stub con bx lr)
int g_hook_count = 0;
Rec g_ring[kRing];
volatile u32 g_head = 0;
u32 g_tail = 0;
volatile u32 g_dropped = 0;

// sessioni nwm::UDS del gioco (handle restituiti da srv GetServiceHandle)
Handle g_uds_handles[8];
volatile u32 g_uds_count = 0;

u64 HostUs() {
    s64 ns = 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&ns, 0x20000, 1)) && ns > 0) {
        return static_cast<u64>(ns) / 1000ULL;
    }
    return svcGetSystemTick() / 268ULL; // (tick del guest come ripiego, ~microsecondi)
}

bool IsUds(Handle h) {
    const u32 n = g_uds_count;
    for (u32 i = 0; i < n && i < 8; ++i) {
        if (g_uds_handles[i] == h) {
            return true;
        }
    }
    return false;
}

void Push(u8 dir, u8 node, u8 chan, u32 size, const u8* data, u8 len, u64 t_us) {
    const u32 i = __sync_fetch_and_add(&g_head, 1);
    Rec& r = g_ring[i % kRing];
    r.seq = 0;
    r.t_us = t_us;
    r.dir = dir;
    r.node = node;
    r.chan = chan;
    r.size = size;
    r.len = len;
    if (len) {
        std::memcpy(r.data, data, len);
    }
    r.seq = i + 1;
}

// Il callback: prende il posto di svcSendSyncRequest(handle) del gioco. Gira nel thread del gioco che chiama.
Result HookedSendSync(Handle h) {
    u32* cmd = getThreadCommandBuffer();
    const u32 hdr = cmd[0];
    const u32 id = hdr >> 16;
    const bool is_uds = IsUds(h);
    const bool is_srv_uds = (hdr == 0x00050100u) && cmd[1] == 0x3A6D776Eu && cmd[2] == 0x5344553Au; // "nwm::UDS"
    u8 pull_chan = 0;
    if (is_uds && id == 0x0017) {
        // SendTo: cmd[2]=nodo destinatario, cmd[3]=canale, cmd[5]=dimensione, cmd[6]=flag, cmd[8]=puntatore ai dati
        const u32 size = cmd[5];
        const u8* data = reinterpret_cast<const u8*>(cmd[8]);
        u8 len = 0;
        u8 copy[kBytes];
        if (data && size) {
            len = static_cast<u8>(size < kBytes ? size : kBytes);
            std::memcpy(copy, data, len);
        }
        Push('S', static_cast<u8>(cmd[2]), static_cast<u8>(cmd[3]), size, copy, len, HostUs());
    } else if (is_uds && id == 0x0014) {
        pull_chan = static_cast<u8>(cmd[1]); // bind node id
    }

    const Result r = HookContext::GetCurrent().OriginalFunction<Result>(h);

    if (is_srv_uds && R_SUCCEEDED(cmd[1])) {
        const Handle uds = cmd[3];
        if (!IsUds(uds) && g_uds_count < 8) {
            g_uds_handles[g_uds_count] = uds;
            __sync_synchronize();
            g_uds_count = g_uds_count + 1;
        }
    } else if (is_uds && id == 0x0014 && R_SUCCEEDED(r) && R_SUCCEEDED(cmd[1])) {
        // PullPacket: cmd[2]=byte ricevuti (0 = niente), cmd[3]=nodo mittente, cmd[5]=puntatore al buffer del gioco
        const u32 size = cmd[2];
        if (size > 0) {
            const u8* data = reinterpret_cast<const u8*>(cmd[5]);
            u8 len = 0;
            u8 copy[kBytes];
            if (data) {
                len = static_cast<u8>(size < kBytes ? size : kBytes);
                std::memcpy(copy, data, len);
            }
            Push('R', static_cast<u8>(cmd[3]), pull_chan, size, copy, len, HostUs());
        }
    }
    return r;
}

} // namespace

// ---- svc 0x32 "inline" (senza funzione: ldr r0,[r0] ; svc 0x32 ; ...), come nel codice di Monster Hunter XX ----
// Due aggancio per ogni punto: PRIMA dell'istruzione svc (legge la richiesta: r0 = handle) e SUBITO DOPO (legge la risposta;
// r0 = risultato). Lo stato fra i due e' tenuto per thread, chiave = indirizzo del command buffer del thread.
struct Pending {
    volatile u32* cmd; // 0 = libero
    u32 hdr;
    Handle h;
    bool is_uds;
    bool is_srv_uds;
    u32 bind;
};
Pending g_pend[8];

Pending* PendFind(u32* cmd) {
    for (int i = 0; i < 8; ++i) {
        if (g_pend[i].cmd == cmd) {
            return &g_pend[i];
        }
    }
    return nullptr;
}

Pending* PendTake(u32* cmd) {
    Pending* p = PendFind(cmd);
    if (p) {
        return p;
    }
    for (int i = 0; i < 8; ++i) {
        if (__sync_bool_compare_and_swap(&g_pend[i].cmd, static_cast<volatile u32*>(nullptr), static_cast<volatile u32*>(cmd))) {
            return &g_pend[i];
        }
    }
    return nullptr;
}

u32 g_inline_calls = 0;
u32 g_inline_h[4] = {0, 0, 0, 0};

void InlinePre(u32 handle) { // chiamato prima di "svc 0x32"; handle = r0 se il framework lo passa
    u32* cmd = getThreadCommandBuffer();
    Pending* p = PendTake(cmd);
    if (!p) {
        return;
    }
    const u32 hdr = cmd[0];
    p->hdr = hdr;
    p->h = handle;
    p->is_uds = IsUds(handle);
    p->is_srv_uds = (hdr == 0x00050100u) && cmd[1] == 0x3A6D776Eu && cmd[2] == 0x5344553Au;
    p->bind = cmd[1];
    if (g_inline_calls < 4) {
        g_inline_h[g_inline_calls] = handle;
    }
    ++g_inline_calls;
    const u32 id = hdr >> 16;
    if (p->is_uds && id == 0x0017) {
        const u32 size = cmd[5];
        const u8* data = reinterpret_cast<const u8*>(cmd[8]);
        u8 len = 0;
        u8 copy[kBytes];
        if (data && size) {
            len = static_cast<u8>(size < kBytes ? size : kBytes);
            std::memcpy(copy, data, len);
        }
        Push('S', static_cast<u8>(cmd[2]), static_cast<u8>(cmd[3]), size, copy, len, HostUs());
    }
}

void InlinePost(u32 result) { // chiamato subito dopo "svc 0x32"; result = r0
    u32* cmd = getThreadCommandBuffer();
    Pending* p = PendFind(cmd);
    if (!p) {
        return;
    }
    const u32 id = p->hdr >> 16;
    if (p->is_srv_uds && R_SUCCEEDED(cmd[1])) {
        const Handle uds = cmd[3];
        if (!IsUds(uds) && g_uds_count < 8) {
            g_uds_handles[g_uds_count] = uds;
            __sync_synchronize();
            g_uds_count = g_uds_count + 1;
        }
    } else if (p->is_uds && id == 0x0014 && R_SUCCEEDED(result) && R_SUCCEEDED(cmd[1])) {
        const u32 size = cmd[2];
        if (size > 0) {
            const u8* data = reinterpret_cast<const u8*>(cmd[5]);
            u8 len = 0;
            u8 copy[kBytes];
            if (data) {
                len = static_cast<u8>(size < kBytes ? size : kBytes);
                std::memcpy(copy, data, len);
            }
            Push('R', static_cast<u8>(cmd[3]), static_cast<u8>(p->bind), size, copy, len, HostUs());
        }
    }
    p->cmd = nullptr;
}

// ---- aggancio a livello di LIBRERIA UDS dell'SDK (modo 5): vale per ogni gioco che usa la libreria ufficiale ----
// Le due funzioni di basso livello che costruiscono la richiesta IPC di SendTo (0x00170182) e PullPacket (0x001400C0) sono
// identiche istruzione per istruzione in Kirby Battle Royale e Monster Hunter XX (verificato smontando entrambi): si trovano
// cercando la costante IPC nel codice (compare una volta sola, nel pool di letterali subito dopo la funzione) e risalendo al
// "push {..., lr}" che apre la funzione. Si agganciano per intero (aggancio MITM sull'ingresso), non i singoli "svc".
//   SendTo(handle*, r1, destNode, canale, datiPtr, paroleAllineate, byte, flag)
//   PullPacket(handle*, bindId, bufferPtr, paroleBuf, maxByte, u32* byteRicevuti, u16* nodoMittente)
Hook g_fn_hooks[2];
u32 g_fn_send = 0, g_fn_pull = 0;

Result HookSendTo(u32 hp, u32 a1, u32 dest, u32 chan, u32 data, u32 words, u32 bytes, u32 flags) {
    u8 len = 0;
    u8 copy[kBytes];
    if (data && bytes) {
        len = static_cast<u8>(bytes < kBytes ? bytes : kBytes);
        std::memcpy(copy, reinterpret_cast<const u8*>(data), len);
    }
    Push('S', static_cast<u8>(dest), static_cast<u8>(chan), bytes, copy, len, HostUs());
    return HookContext::GetCurrent().OriginalFunction<Result>(hp, a1, dest, chan, data, words, bytes, flags);
}

Result HookPullPacket(u32 hp, u32 bind, u32 buf, u32 words, u32 maxbytes, u32* out_size, u16* out_src) {
    const Result r = HookContext::GetCurrent().OriginalFunction<Result>(hp, bind, buf, words, maxbytes, out_size, out_src);
    if (R_SUCCEEDED(r) && out_size && out_src && *out_size > 0) {
        const u32 size = *out_size;
        u8 len = 0;
        u8 copy[kBytes];
        if (buf) {
            len = static_cast<u8>(size < kBytes ? size : kBytes);
            std::memcpy(copy, reinterpret_cast<const u8*>(buf), len);
        }
        Push('R', static_cast<u8>(*out_src), static_cast<u8>(bind), size, copy, len, HostUs());
    }
    return r;
}

// Indirizzo della costante IPC `literal` nel codice. Con near == 0 deve essere unica (0 altrimenti); con near != 0 si prende
// quella piu' vicina a `near`, entro max_dist (la costante 0x001400C0 e' di PullPacket ma compare anche in altri servizi: quella
// giusta sta nello stesso gruppo di funzioni del SendTo, a ~1,2-1,3 KB prima, in tutti i giochi visti).
u32 FindLiteral(u32 literal, u32 text_start, u32 text_end, u32 near, u32 max_dist) {
    u32 best = 0;
    u32 best_d = 0xFFFFFFFFu;
    int hits = 0;
    for (u32 a = text_start; a + 4 <= text_end; a += 4) {
        if (*reinterpret_cast<const u32*>(a) == literal) {
            ++hits;
            const u32 d = near ? (a > near ? a - near : near - a) : 0;
            if (d < best_d) {
                best_d = d;
                best = a;
            }
        }
    }
    if (near) {
        return best_d <= max_dist ? best : 0;
    }
    return hits == 1 ? best : 0;
}

// Inizio della funzione che usa la costante: il "push {..., lr}" che la precede.
u32 FunctionStartBefore(u32 hit, u32 text_start) {
    if (!hit) {
        return 0;
    }
    for (u32 a = hit - 4; a > text_start; a -= 4) {
        const u32 w = *reinterpret_cast<const u32*>(a);
        if ((w & 0xFFFF4000u) == 0xE92D4000u) { // push {..., lr}
            return a;
        }
    }
    return 0;
}

struct Site {
    u32 addr;
    u32 next;
    char kind; // 'M' = stub con bx lr (aggancio MITM), 'I' = inline (due aggancio), '-' = non agganciato
};
Site g_sites[64];
int g_site_count = 0;
int g_inline_hooked = 0;
int g_inline_failed = 0;
int g_inline_first_fail = -1;
Hook g_inline_hooks[2 * 40];

bool UdsTraceStart(int mode) { // mode (solo per prove): 0 = due aggancio per punto, 1 = solo "prima", 2 = solo "dopo", 3 = nessun inline
    const u32 text_start = 0x00100000;
    const u32 text_end = text_start + Process::GetTextSize();
    if (mode == 5) { // libreria UDS dell'SDK (default)
        const u32 send_lit = FindLiteral(0x00170182, text_start, text_end, 0, 0);
        const u32 pull_lit = send_lit ? FindLiteral(0x001400C0, text_start, text_end, send_lit, 0x1000) : 0;
        g_fn_send = FunctionStartBefore(send_lit, text_start);
        g_fn_pull = FunctionStartBefore(pull_lit, text_start);
        if (g_fn_send && g_fn_pull) {
            g_fn_hooks[0].InitializeForMitm(g_fn_send, reinterpret_cast<u32>(&HookSendTo));
            g_fn_hooks[1].InitializeForMitm(g_fn_pull, reinterpret_cast<u32>(&HookPullPacket));
            if (g_fn_hooks[0].Enable() == HookResult::Success && g_fn_hooks[1].Enable() == HookResult::Success) {
                g_hook_count = 2;
                return true;
            }
        }
        return false;
    }
    for (u32 a = text_start + 4; a + 8 <= text_end; a += 4) {
        if (*reinterpret_cast<const u32*>(a) != kSvcSendSyncRequest) {
            continue;
        }
        Site s{a, *reinterpret_cast<const u32*>(a + 4), '-'};
        if (s.next == kBxLr) {
            if (g_hook_count < kMaxHooks) {
                g_hooks[g_hook_count].InitializeForMitm(a, reinterpret_cast<u32>(&HookedSendSync));
                if (g_hooks[g_hook_count].Enable() == HookResult::Success) {
                    ++g_hook_count;
                    s.kind = 'M';
                }
            }
        } else if (mode != 3 && g_inline_hooked < 40) {
            Hook& pre = g_inline_hooks[2 * g_inline_hooked];
            Hook& post = g_inline_hooks[2 * g_inline_hooked + 1];
            pre.Initialize(a, reinterpret_cast<u32>(&InlinePre));
            pre.SetFlags(USE_LR_TO_RETURN | EXECUTE_OI_AFTER_CB); // prima la nostra funzione, poi l'svc originale
            // modo 4: il "dopo" sta DUE istruzioni oltre l'svc (a + 8), cosi' non si sovrappone al "prima" (a, a + 4)
            post.Initialize(mode == 4 ? a + 8 : a + 4, reinterpret_cast<u32>(&InlinePost));
            post.SetFlags(USE_LR_TO_RETURN | EXECUTE_OI_BEFORE_CB); // prima l'istruzione originale dopo l'svc, poi la nostra
            const HookResult r1 = mode == 1 ? HookResult::Success : post.Enable();
            const HookResult r2 = r1 != HookResult::Success ? r1 : (mode == 2 ? HookResult::Success : pre.Enable());
            if (r2 == HookResult::Success) {
                ++g_inline_hooked;
                s.kind = 'I';
            } else {
                if (r1 == HookResult::Success && mode != 1) {
                    post.Disable();
                }
                ++g_inline_failed;
                if (g_inline_first_fail < 0) {
                    g_inline_first_fail = static_cast<int>(r2);
                }
            }
        }
        if (g_site_count < 64) {
            g_sites[g_site_count++] = s;
        }
    }
    g_hook_count += g_inline_hooked;
    return g_hook_count > 0;
}

int UdsTraceDescribeSites(char* buf, int cap) {
    if (g_fn_send || g_fn_pull) {
        return std::snprintf(buf, cap, "SDK UDS wrappers: SendTo=%08lX PullPacket=%08lX", static_cast<unsigned long>(g_fn_send),
                             static_cast<unsigned long>(g_fn_pull));
    }
    int out = std::snprintf(buf, cap, "sites=%d stub(M)=%d inline(I)=%d inline_failed=%d first_fail=%d | ", g_site_count,
                            g_hook_count - g_inline_hooked, g_inline_hooked, g_inline_failed, g_inline_first_fail);
    for (int i = 0; i < g_site_count && i < 10 && out < cap - 40; ++i) {
        out += std::snprintf(buf + out, cap - out, "%08lX%c ", static_cast<unsigned long>(g_sites[i].addr), g_sites[i].kind);
    }
    return out;
}

int UdsTraceInlineStats(char* buf, int cap) { // chiamate viste dai punti inline e primi handle (per verificare che r0 arrivi)
    return std::snprintf(buf, cap, "inline_calls=%lu first_handles=%08lX %08lX %08lX %08lX uds_sessions=%lu",
                         static_cast<unsigned long>(g_inline_calls), static_cast<unsigned long>(g_inline_h[0]),
                         static_cast<unsigned long>(g_inline_h[1]), static_cast<unsigned long>(g_inline_h[2]),
                         static_cast<unsigned long>(g_inline_h[3]), static_cast<unsigned long>(g_uds_count));
}

int UdsTraceHooks() {
    return g_hook_count;
}

unsigned UdsTraceDropped() {
    return g_dropped;
}

namespace {

// ---- stimatore della latenza per nodo (profilo del protocollo di Kirby Battle Royale: magic 32 AB 98 64) ----
constexpr u8 kMagic[4] = {0x32, 0xAB, 0x98, 0x64};
constexpr int kSentRing = 1024;
constexpr int kSamples = 24;
constexpr u64 kWindowUs = 30ULL * 1000000ULL; // mediana su 30 s: preferita la stabilita' alla reattivita' (scelta dell'utente)
constexpr u64 kShowUs = 60ULL * 1000000ULL;   // una riga resta a schermo finche' c'e' un campione negli ultimi 60 s
constexpr u32 kMaxEchoSize = 128; // i pacchetti di controllo con eco sono piccoli (64 byte); gli altri sono stato di gioco

struct Sent {
    u16 ts;
    u64 t_us; // 0 = vuoto
};
struct Sample {
    u64 t_us;
    u32 rtt_us;
};
struct NodeStat {
    Sample s[kSamples];
    int head = 0;
    int count = 0;
    u64 last_rx_us = 0; // ultimo pacchetto del gioco ricevuto da questo nodo (qualunque, anche senza eco)
    bool slow = false;  // campioni rari (PRUDP: pochi dati affidabili da confermare): la mediana usa una finestra piu' lunga
};
constexpr u64 kSlowWindowUs = 120ULL * 1000000ULL;
// Un nodo da cui non arriva piu' nulla da questo tempo ha lasciato la partita: la sua riga sparisce subito e i campioni
// vecchi si scartano (i pacchetti di stato arrivano ~30 volte al secondo; la mediana su 30 s resta per la stabilita').
constexpr u64 kAliveUs = 3ULL * 1000000ULL;
Sent g_sent[kSentRing];
Sent g_last_sent = {0, 0};
NodeStat g_nodes[9];
LightLock g_est_lock;
bool g_est_ready = false;

inline u16 Be16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}

// Posizione del magic nel pacchetto: 0 in Kirby Battle Royale e Monster Hunter XX, 12 in Mario Party Star Rush (che mette
// davanti un involucro di 12 byte); si cerca nei primi 16 byte. -1 = non e' di questo protocollo.
int FindMagic(const Rec& r) {
    for (int o = 0; o <= 16 && o + 12 <= r.len; ++o) {
        if (std::memcmp(r.data + o, kMagic, 4) == 0) {
            return o;
        }
    }
    return -1;
}

// ---- PRUDP v1 (NEX), come in Final Fantasy Explorers: dopo l'involucro EA D0 01 ... Non ha timestamp con eco: il giro si ricava
// abbinando i dati AFFIDABILI mandati (tipo DATA con flag NEED_ACK) alle conferme aggregate (flag MULTI_ACK, payload = numeri di
// sequenza confermati). Le conferme pero' non partono subito: il gioco le aggrega e le manda dopo ~60 ms (anche su loopback).
// Il ritardo di aggregazione lo si MISURA OGNI VOLTA sul mio stesso gioco (dall'arrivo di un dato affidabile all'invio della mia
// conferma) e si sottrae: finche' non ce n'e' una misura si usa 60 ms.
struct PrudpPend {
    u64 t_us;
    u16 seq;
    u8 node;
    u8 used;
};
PrudpPend g_p_rx[64];
PrudpPend g_p_tx[64];
int g_p_rx_head = 0, g_p_tx_head = 0;
u32 g_hold_us[16];
int g_hold_count = 0, g_hold_head = 0;
constexpr u32 kDefaultHoldUs = 60000;

inline u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

int FindPrudp(const Rec& r) {
    for (int o = 0; o <= 16 && o + 14 <= r.len; ++o) {
        if (r.data[o] == 0xEA && r.data[o + 1] == 0xD0 && r.data[o + 2] == 0x01) {
            return o;
        }
    }
    return -1;
}

void PendPut(PrudpPend* a, int* head, u8 node, u16 seq, u64 t) {
    a[*head] = {t, seq, node, 1};
    *head = (*head + 1) % 64;
}

u64 PendTakeT(PrudpPend* a, u8 node, u16 seq) {
    for (int i = 0; i < 64; ++i) {
        if (a[i].used && a[i].node == node && a[i].seq == seq) {
            a[i].used = 0;
            return a[i].t_us;
        }
    }
    return 0;
}

// Pacchetti non-DATA (o DATA senza conferma aggregata) che ho mandato con NEED_ACK, in attesa della conferma individuale.
struct TypedPend {
    u64 t_us;
    u16 seq;
    u8 node;
    u8 typ;
    u8 used;
};
TypedPend g_typed[32];
int g_typed_head = 0;
void PendPutT(u8 node, u8 typ, u16 seq, u64 t) {
    g_typed[g_typed_head] = {t, seq, node, typ, 1};
    g_typed_head = (g_typed_head + 1) % 32;
}
u64 PendTakeTyped(u8 node, u8 typ, u16 seq) {
    for (int i = 0; i < 32; ++i) {
        if (g_typed[i].used && g_typed[i].node == node && g_typed[i].typ == typ && g_typed[i].seq == seq) {
            g_typed[i].used = 0;
            return g_typed[i].t_us;
        }
    }
    return 0;
}
// Richieste con timestamp (u16) mandate in DATA non affidabili, in attesa della risposta che lo rimanda.
struct EchoPend {
    u64 t_us;
    u16 stamp;
    u8 node;
    u8 used;
};
EchoPend g_echo[16];
int g_echo_head = 0;
void EchoPut(u8 node, u16 stamp, u64 t) {
    g_echo[g_echo_head] = {t, stamp, node, 1};
    g_echo_head = (g_echo_head + 1) % 16;
}
u64 EchoTake(u8 node, u16 stamp) {
    for (int i = 0; i < 16; ++i) {
        if (g_echo[i].used && g_echo[i].node == node && g_echo[i].stamp == stamp) {
            g_echo[i].used = 0;
            return g_echo[i].t_us;
        }
    }
    return 0;
}

// Orologio di gioco nei pacchetti di stato (Final Fantasy Explorers): ogni pacchetto "tipo 5" (payload 00 40 94 00, byte 12 = 5) porta a
// payload+20 l'orologio di gioco del mittente in ms (u32 LE). Il gioco tiene gli orologi delle console allineati (misurato: scarto
// di pochi ms), quindi "il mio orologio quando arriva" meno "l'orologio scritto dal mittente" e' il ritardo di UN verso da quel
// nodo a me, anche per un altro client (con cui il gioco non fa ping). Il mio orologio si ricava dai miei invii (primo invio di ogni
// valore nuovo) piu' il tempo trascorso sull'orologio dell'host.
struct OwdSample {
    u64 t_us;
    s32 d_ms;
};
constexpr int kOwdSamples = 160;                    // un campione per ogni valore nuovo dell'orologio del mittente: ~15 al secondo
constexpr u64 kOwdWindowUs = 8ULL * 1000000ULL;
constexpr int kOwdMinSamples = 40;
constexpr int kMeasuredMinSamples = 8;              // sotto questo numero di campioni veri nella finestra, per un nodo si preferisce la stima
OwdSample g_owd[9][kOwdSamples];
int g_owd_head[9];
int g_owd_count[9];
u32 g_owd_last_clk[9];
u32 g_my_clk = 0;
u64 g_my_clk_t = 0;

inline u32 Le32(const u8* p) {
    return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

u32 CurrentHoldUs() {
    if (g_hold_count < 3) {
        return kDefaultHoldUs;
    }
    u32 v[16];
    for (int i = 0; i < g_hold_count; ++i) v[i] = g_hold_us[i];
    for (int i = 1; i < g_hold_count; ++i) { // ordinamento per inserzione
        const u32 x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
    return v[g_hold_count / 2];
}

void TouchNode(int node, u64 t_us) {
    LightLock_Lock(&g_est_lock);
    NodeStat& n = g_nodes[node];
    if (n.last_rx_us != 0 && t_us > n.last_rx_us && t_us - n.last_rx_us > kAliveUs) {
        n.count = 0;
        n.head = 0;
    }
    n.last_rx_us = t_us;
    LightLock_Unlock(&g_est_lock);
}

void AddSample(int node, u64 t_us, u32 rtt_us) {
    LightLock_Lock(&g_est_lock);
    NodeStat& n = g_nodes[node];
    n.slow = true; // (chiamata solo dal percorso PRUDP)
    n.s[n.head] = {t_us, rtt_us};
    n.head = (n.head + 1) % kSamples;
    if (n.count < kSamples) {
        ++n.count;
    }
    LightLock_Unlock(&g_est_lock);
}

void FeedPrudp(const Rec& r) {
    const int po = FindPrudp(r);
    if (po < 0 || r.node < 1 || r.node > 8) {
        return;
    }
    const u8* d = r.data + po;
    const int psz = d[3];
    const int pay = Le16(d + 4);
    const u16 tf = Le16(d + 8);
    const int typ = tf & 0xF;
    const int fl = tf >> 4;
    const int sub = d[11];
    const u16 seq = r.len >= po + 14 ? Le16(d + 12) : 0;
    const int pl_off = po + 14 + psz + 16;
    int pl_len = r.len > pl_off ? r.len - pl_off : 0;
    if (pl_len > pay) pl_len = pay;
    if (r.dir == 'R') {
        TouchNode(r.node, r.t_us);
    }
    // Regola generale del protocollo (wiki di NintendoClients / Pretendo): ogni pacchetto con NEED_ACK (0x4) va confermato con un
    // pacchetto dello STESSO tipo, flag ACK (0x1) e lo stesso numero di sequenza. Vale per PING (tipo 4), SYN, CONNECT, DISCONNECT
    // e, nelle versioni senza conferma aggregata, anche per i DATA. La conferma individuale parte subito: nessun ritardo da togliere.
    if (!(fl & 0x200) && (typ != 2 || sub != 0 || !(fl & 0x2))) {
        if (r.dir == 'S' && (fl & 0x4) && !(fl & 0x1)) {
            PendPutT(r.node, static_cast<u8>(typ), seq, r.t_us);
        } else if (r.dir == 'R' && (fl & 0x1)) {
            const u64 t_sent = PendTakeTyped(r.node, static_cast<u8>(typ), seq);
            if (t_sent != 0 && r.t_us > t_sent && r.t_us - t_sent < 3000000ULL) {
                AddSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_sent));
            }
        }
    }
    if (typ != 2) {
        return;
    }
    // Richiesta/risposta con timestamp dentro un DATA non affidabile (Final Fantasy Explorers, ogni ~2 s verso l'host): il contenuto e'
    // "lunghezza u32 LE, ..., byte 20 = 2 (richiesta) o 3 (risposta), byte 21 = 9, byte 22-23 = timestamp u16 LE": la risposta
    // rimanda il timestamp della richiesta. Controllo di struttura: la lunghezza dichiarata deve coincidere col payload.
    if (!(fl & 0x4) && !(fl & 0x200) && pl_len >= 26 && pl_len == pay) {
        const u8* q = r.data + pl_off;
        const u32 declared = static_cast<u32>(q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24));
        if (declared + 4 == static_cast<u32>(pl_len) && q[21] == 9 && (q[20] == 2 || q[20] == 3)) {
            const u16 stamp = Le16(q + 22);
            if (q[20] == 2 && r.dir == 'S') {
                EchoPut(r.node, stamp, r.t_us);
            } else if (q[20] == 3 && r.dir == 'R') {
                const u64 t_sent = EchoTake(r.node, stamp);
                if (t_sent != 0 && r.t_us > t_sent && r.t_us - t_sent < 3000000ULL) {
                    AddSample(r.node, r.t_us, static_cast<u32>(r.t_us - t_sent));
                }
            }
            return;
        }
    }
    if (fl == 0x88 && pl_len >= 24 && pl_off + 24 <= r.len) {
        const u8* q = r.data + pl_off;
        if (q[0] == 0x00 && q[1] == 0x40 && q[2] == 0x94 && q[3] == 0x00 && q[12] == 5) {
            const u32 clk = Le32(q + 20);
            if (r.dir == 'S') {
                if (g_my_clk_t == 0 || clk != g_my_clk) { // il primo invio di un valore nuovo e' il momento piu' vicino alla sua creazione
                    g_my_clk = clk;
                    g_my_clk_t = r.t_us;
                }
            } else if (r.node >= 1 && r.node <= 8 && clk != g_owd_last_clk[r.node]) { // primo arrivo di un valore nuovo: meno attesa dentro il gioco
                g_owd_last_clk[r.node] = clk;
                if (g_my_clk_t != 0 && r.t_us >= g_my_clk_t && r.t_us - g_my_clk_t < 500000ULL) {
                    const s32 my_now = static_cast<s32>(g_my_clk + static_cast<u32>((r.t_us - g_my_clk_t) / 1000ULL));
                    const s32 d = my_now - static_cast<s32>(clk);
                    if (d >= -100 && d <= 3000) { // fuori: salto dell'orologio (risincronizzazione), non e' un ritardo
                        LightLock_Lock(&g_est_lock);
                        g_owd[r.node][g_owd_head[r.node]] = {r.t_us, d};
                        g_owd_head[r.node] = (g_owd_head[r.node] + 1) % kOwdSamples;
                        if (g_owd_count[r.node] < kOwdSamples) ++g_owd_count[r.node];
                        LightLock_Unlock(&g_est_lock);
                    }
                }
            }
        }
    }
    if (r.dir == 'R') {
        if ((fl & 0x4) && sub == 0) {
            PendPut(g_p_rx, &g_p_rx_head, r.node, seq, r.t_us); // dato affidabile che dovro' confermare
        } else if (fl & 0x200) { // conferma aggregata del nodo: chiude i miei dati affidabili
            for (int k = 2; k + 1 < pl_len; k += 2) {
                const u64 t_sent = PendTakeT(g_p_tx, r.node, Le16(r.data + pl_off + k));
                if (t_sent != 0 && r.t_us > t_sent && r.t_us - t_sent < 3000000ULL) {
                    const u32 raw = static_cast<u32>(r.t_us - t_sent);
                    const u32 hold = CurrentHoldUs();
                    AddSample(r.node, r.t_us, raw > hold ? raw - hold : 0);
                }
            }
        }
    } else { // 'S'
        if (fl & 0x200) { // la mia conferma aggregata: misura quanto il mio gioco ha trattenuto i dati ricevuti
            for (int k = 2; k + 1 < pl_len; k += 2) {
                const u64 t_rx = PendTakeT(g_p_rx, r.node, Le16(r.data + pl_off + k));
                if (t_rx != 0 && r.t_us > t_rx && r.t_us - t_rx < 500000ULL) {
                    g_hold_us[g_hold_head] = static_cast<u32>(r.t_us - t_rx);
                    g_hold_head = (g_hold_head + 1) % 16;
                    if (g_hold_count < 16) ++g_hold_count;
                }
            }
        } else if ((fl & 0x4) && sub == 0 && r.node >= 1 && r.node <= 8) {
            PendPut(g_p_tx, &g_p_tx_head, r.node, seq, r.t_us);
        }
    }
}

// Riconoscimento del protocollo, solo per il log (UdsProtoSummary): conta i pacchetti di ogni tipo e tiene i primi byte di un
// pacchetto non riconosciuto per mandata e per ricezione, cosi' si puo' analizzare un gioco nuovo anche da un log mandato da altri.
struct ProtoStat {
    u32 pia, prudp, other;
    u8 smp[2][24];       // primi byte del primo pacchetto sconosciuto: 0 = mandato, 1 = ricevuto
    u8 smp_len[2];
    u32 smp_size[2];
    u32 logged_total;    // totale al momento dell'ultima riga di log
};
ProtoStat g_proto;

void ClassifyProto(const Rec& r) {
    if (FindMagic(r) >= 0) {
        ++g_proto.pia;
    } else if (FindPrudp(r) >= 0) {
        ++g_proto.prudp;
    } else {
        ++g_proto.other;
        const int k = r.dir == 'S' ? 0 : 1;
        if (g_proto.smp_len[k] == 0 && r.len > 0) {
            g_proto.smp_len[k] = r.len < 24 ? r.len : 24;
            g_proto.smp_size[k] = r.size;
            std::memcpy(g_proto.smp[k], r.data, g_proto.smp_len[k]);
        }
    }
}

void Feed(const Rec& r) {
    ClassifyProto(r);
    const int mo = FindMagic(r);
    if (mo < 0) {
        FeedPrudp(r);
        return;
    }
    const u16 ts = Be16(r.data + mo + 8);
    if (r.dir == 'S') {
        g_sent[ts & (kSentRing - 1)] = {ts, r.t_us};
        g_last_sent = {ts, r.t_us};
        return;
    }
    if (r.node < 1 || r.node > 8) {
        return;
    }
    {
        LightLock_Lock(&g_est_lock);
        NodeStat& n = g_nodes[r.node];
        if (n.last_rx_us != 0 && r.t_us > n.last_rx_us && r.t_us - n.last_rx_us > kAliveUs) {
            n.count = 0; // il nodo e' tornato dopo una pausa (nuova partita): si riparte senza i campioni vecchi
            n.head = 0;
        }
        n.last_rx_us = r.t_us;
        LightLock_Unlock(&g_est_lock);
    }
    const u16 echo = Be16(r.data + mo + 10);
    // Header Pia (wiki NintendoClients "Pia Protocol", versioni fino alla 5.10): 0x8 = timer di sessione del mittente (ms),
    // 0xA = timer del destinatario dall'ultimo scambio. 0 = non ha ancora ricevuto nulla; 0xFFFF = valore sentinella che il
    // protocollo usa quando l'eco sarebbe 0. Nessuno dei due e' un eco.
    if (echo == 0 || echo == 0xFFFF) {
        return;
    }
    // Regola (verificata su Kirby Battle Royale e Monster Hunter XX, che hanno la stessa intestazione): i byte 8-9 sono il
    // timestamp del mittente (ms), i byte 10-11 l'ultimo timestamp RICEVUTO da me, rimandato indietro: esatto (Kirby, pacchetti
    // di controllo) o gia' aumentato del tempo che il pacchetto e' rimasto fermo da lui (MH XX, quasi ogni pacchetto).
    // In entrambi i casi il giro con quel nodo e' "il mio orologio adesso" meno l'eco. Se l'eco e' uno dei miei invii, la differenza
    // si fa direttamente sull'orologio dell'host (microsecondi); altrimenti col mio orologio di gioco, ricavato dall'ultimo invio.
    u32 rtt_us;
    const Sent& s = g_sent[echo & (kSentRing - 1)];
    if (s.t_us != 0 && s.ts == echo && r.t_us > s.t_us && r.t_us - s.t_us <= 3000000ULL) {
        rtt_us = static_cast<u32>(r.t_us - s.t_us);
    } else {
        if (g_last_sent.t_us == 0 || r.t_us < g_last_sent.t_us || r.t_us - g_last_sent.t_us > 2000000ULL) {
            return; // non ho un mio invio recente per ricavare il mio orologio
        }
        const u32 now_ts = (static_cast<u32>(g_last_sent.ts) + static_cast<u32>((r.t_us - g_last_sent.t_us) / 1000ULL)) & 0xFFFFu;
        const u32 diff_ms = (now_ts - echo) & 0xFFFFu;
        if (diff_ms > 2000) {
            return; // non e' un eco di un mio timestamp
        }
        rtt_us = diff_ms * 1000u;
    }
    LightLock_Lock(&g_est_lock);
    NodeStat& n = g_nodes[r.node];
    n.s[n.head] = {r.t_us, rtt_us};
    n.head = (n.head + 1) % kSamples;
    if (n.count < kSamples) {
        ++n.count;
    }
    LightLock_Unlock(&g_est_lock);
}

} // namespace

int UdsTracePump(char* buf, int cap) {
    if (!g_est_ready) {
        LightLock_Init(&g_est_lock);
        g_est_ready = true;
    }
    int out = 0;
    const u32 head = g_head;
    if (head - g_tail > kRing) { // l'anello e' stato riempito mentre non si leggeva: si salta ai piu' recenti
        g_dropped += head - g_tail - kRing;
        g_tail = head - kRing;
    }
    while (g_tail != head && (!buf || out < cap - 480)) {
        const Rec& r = g_ring[g_tail % kRing];
        if (r.seq != g_tail + 1) { // ancora in scrittura: si riprova al giro dopo
            break;
        }
        Feed(r);
        if (buf) {
            out += std::snprintf(buf + out, cap - out, "%llu,%c,%u,%u,%lu,", static_cast<unsigned long long>(r.t_us),
                                 r.dir, r.node, r.chan, static_cast<unsigned long>(r.size));
            for (int i = 0; i < r.len; ++i) {
                out += std::snprintf(buf + out, cap - out, "%02X", r.data[i]);
            }
            buf[out++] = '\n';
        }
        ++g_tail;
    }
    return out;
}

int UdsRttSnapshot(NodeRtt out[8]) {
    if (!g_est_ready) {
        return 0;
    }
    const u64 now = HostUs();
    int shown = 0;
    LightLock_Lock(&g_est_lock);
    for (int node = 1; node <= 8; ++node) {
        NodeRtt& o = out[node - 1];
        o = NodeRtt();
        o.node = node;
        const NodeStat& n = g_nodes[node];
        u32 v[kSamples];
        int c = 0;
        u64 newest = 0;
        u32 last = 0;
        u32 lowest = 0xFFFFFFFFu;
        for (int i = 0; i < n.count; ++i) {
            const Sample& s = n.s[i];
            if (now - s.t_us <= (n.slow ? kSlowWindowUs : kWindowUs)) {
                v[c++] = s.rtt_us;
                if (s.rtt_us < lowest) lowest = s.rtt_us;
            }
            if (s.t_us > newest) {
                newest = s.t_us;
                last = s.rtt_us;
            }
        }
        // Una riga si mostra finche' dal nodo arrivano pacchetti del gioco (anche se non ci sono ancora campioni: allora "nome --",
        // come per i client di FF Explorers, che mandano pochi dati affidabili).
        if (n.last_rx_us != 0 && now - n.last_rx_us <= kAliveUs) {
            ++shown;
            o.age_ms = newest ? static_cast<int>((now - newest) / 1000) : 0;
            o.last_ms = newest ? static_cast<int>(last / 1000) : -1;
        }
        if (c > 0) {
            for (int i = 1; i < c; ++i) { // ordinamento per inserzione (c <= 24)
                const u32 x = v[i];
                int j = i - 1;
                while (j >= 0 && v[j] > x) {
                    v[j + 1] = v[j];
                    --j;
                }
                v[j + 1] = x;
            }
            o.rtt_ms = static_cast<int>(v[c / 2] / 1000);
            o.min_ms = static_cast<int>(lowest / 1000);
            o.samples = c;
        } else if (o.age_ms >= 0 && n.count > 0) {
            // Nessun campione recente ma il nodo e' ancora connesso (FF Explorers: un client manda dati affidabili solo quando entra,
            // dopo non c'e' piu' niente da abbinare). Si mostra l'ultimo valore noto, segnato come vecchio.
            u32 w[kSamples];
            int m = 0;
            for (int i = 0; i < n.count; ++i) w[m++] = n.s[i].rtt_us;
            for (int i = 1; i < m; ++i) {
                const u32 x = w[i];
                int j = i - 1;
                while (j >= 0 && w[j] > x) {
                    w[j + 1] = w[j];
                    --j;
                }
                w[j + 1] = x;
            }
            o.rtt_ms = static_cast<int>(w[m / 2] / 1000);
            o.stale = true;
        }
        // Ritardo di un verso (orologio di gioco nei pacchetti di stato): mediana dei campioni degli ultimi 8 s, un campione per ogni
        // valore nuovo dell'orologio del mittente (cosi' non c'e' l'attesa dei pacchetti che ripetono il valore). Verificato con tre
        // istanze sullo stesso PC contro il ritardo reale di consegna (stessi pacchetti visti da chi li manda e da chi li riceve):
        // scarto entro ~5 ms su tutti e sei i versi. Il quinto percentile invece sbagliava di 3-16 ms in modo diverso da un nodo
        // all'altro.
        int d[kOwdSamples];
        int dc = 0;
        for (int i = 0; i < g_owd_count[node]; ++i) {
            if (now - g_owd[node][i].t_us <= kOwdWindowUs) d[dc++] = g_owd[node][i].d_ms;
        }
        if (dc >= kOwdMinSamples && o.age_ms >= 0) {
            for (int i = 1; i < dc; ++i) {
                const int x = d[i];
                int j = i - 1;
                while (j >= 0 && d[j] > x) {
                    d[j + 1] = d[j];
                    --j;
                }
                d[j + 1] = x;
            }
            o.owd_ms = d[dc / 2] < 0 ? 0 : d[dc / 2];
        }
    }
    // Dove c'e' il ritardo di un verso (FF Explorers, ogni nodo, anche client contro client) il giro e' 2 x ritardo di un verso, per
    // TUTTI i nodi con la stessa regola: le misure dirette (eco del ping verso l'host: +~33 ms perche' l'host risponde al suo
    // prossimo frame; conferme con la loro correzione: +~5 ms) restano solo per quando l'orologio non e' ancora disponibile.
    // Nota: sull'host (nodo 1) l'orologio dei client e' allineato con il suo (come fa NTP) e il ritardo di un verso e' esattamente meta'
    // del giro anche se la strada e' asimmetrica; verso un altro client resta l'errore di allineamento tra i due, di pochi ms.
    for (int i = 0; i < 8; ++i) {
        NodeRtt& o = out[i];
        if (o.owd_ms >= 0) {
            o.rtt_ms = 2 * o.owd_ms;
            if (o.rtt_ms < 1) o.rtt_ms = 1;
            o.stale = false;
            o.estimated = true;
        }
    }
    LightLock_Unlock(&g_est_lock);
    return shown;
}

int UdsProtoSummary(char* buf, int cap) {
    const u32 total = g_proto.pia + g_proto.prudp + g_proto.other;
    if (total == 0 || total == g_proto.logged_total) {
        return 0;
    }
    g_proto.logged_total = total;
    const char* name = "sconosciuto";
    if (g_proto.pia > 0 && g_proto.pia >= g_proto.prudp && g_proto.pia > g_proto.other) {
        name = "Pia";
    } else if (g_proto.prudp > 0 && g_proto.prudp > g_proto.pia && g_proto.prudp > g_proto.other) {
        name = "PRUDP v1";
    }
    int len = std::snprintf(buf, cap, "protocol: %s (Pia=%u PRUDPv1=%u other=%u)", name, static_cast<unsigned>(g_proto.pia),
                            static_cast<unsigned>(g_proto.prudp), static_cast<unsigned>(g_proto.other));
    for (int k = 0; k < 2 && g_proto.other > 0; ++k) {
        if (g_proto.smp_len[k] == 0 || len >= cap - 80) {
            continue;
        }
        len += std::snprintf(buf + len, cap - len, " | unknown %c size=%u:", k == 0 ? 'S' : 'R',
                             static_cast<unsigned>(g_proto.smp_size[k]));
        for (int i = 0; i < g_proto.smp_len[k] && len < cap - 4; ++i) {
            len += std::snprintf(buf + len, cap - len, " %02X", g_proto.smp[k][i]);
        }
    }
    return len;
}

} // namespace azp
