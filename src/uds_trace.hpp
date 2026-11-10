// uds_trace - osserva i pacchetti UDS che il GIOCO manda e riceve, senza modificarli.
//
// Si aggancia (CTRPluginFramework Hook, modo MITM) alla funzione del gioco che fa  svc 0x32 (svcSendSyncRequest): il gioco
// la usa per ogni richiesta ai servizi. Si riconosce la sessione di nwm::UDS dalla richiesta srv "GetServiceHandle" con
// nome "nwm::UDS" (il valore restituito e' la sessione del gioco), poi si guardano solo due comandi di quella sessione:
//   0x0017 SendTo      (il gioco manda un pacchetto: nodo destinatario, canale, dimensione, primi byte)
//   0x0014 PullPacket  (il gioco riceve un pacchetto: nodo mittente, dimensione, primi byte)
// Ogni evento ha l'ora dell'HOST di Azahar in microsecondi (svcGetSystemInfo 0x20000/1): vale anche se l'emulazione e' lenta.
// La richiesta originale viene sempre eseguita, intatta. Il callback non blocca e non scrive su file: mette i record in un
// anello e li legge UdsTraceFormat() dal thread di rete.
#pragma once

#include <3ds.h>

namespace azp {

bool UdsTraceStart(int mode = 0);                     // cerca il stub svc 0x32 nel codice del gioco e lo aggancia; false se non lo trova
int  UdsTraceDescribeSites(char* buf, int cap); // testo: tutti i punti "svc 0x32" trovati nel codice del gioco (H = agganciato)
int  UdsTraceInlineStats(char* buf, int cap);   // testo: chiamate viste dagli aggancio inline e primi handle
int  UdsTraceHooks();                       // quanti stub sono stati agganciati
// Elabora i record nuovi (va chiamata spesso, dal thread di rete): li passa allo stimatore della latenza e, se buf non e'
// nullptr, li scrive come CSV (t_us,dir,node,chan,size,hex; dir: S = mandato, R = ricevuto). Ritorna i byte scritti.
int  UdsTracePump(char* buf, int cap);
unsigned UdsTraceDropped();                 // record persi perche' l'anello era pieno

// Latenza per giocatore, dai pacchetti del gioco stesso (nessun pacchetto in piu' e nessun plugin sugli altri):
// il protocollo di Kirby Battle Royale mette in ogni pacchetto di controllo il timestamp del mittente (byte 8-9, ms) e,
// rimandato indietro, l'ultimo timestamp ricevuto dal destinatario (byte 10-11). Quando un pacchetto da un nodo porta
// un timestamp che e' uno dei miei, il tempo da quando l'ho mandato a ora e' un giro completo con quel nodo
// (comprende la cadenza dei frame del gioco da entrambe le parti: oltre al ritardo di rete c'e' di norma 15-35 ms).
struct NodeRtt {
    int node = 0;       // numero di nodo UDS (1 = host)
    int rtt_ms = -1;    // mediana dei campioni degli ultimi 15 s; -1 se non ce ne sono
    int min_ms = -1;    // il piu' basso
    int last_ms = -1;   // l'ultimo
    int age_ms = -1;    // da quanto e' l'ultimo campione
    int samples = 0;    // campioni nella finestra
    bool stale = false; // il valore e' vecchio: nessun campione nella finestra, si mostra l'ultimo noto finche' il nodo e' connesso
    int owd_ms = -1;    // ritardo di UN verso (da quel nodo a me) ricavato dall'orologio di gioco nei pacchetti di stato; -1 se non c'e'
    bool estimated = false; // rtt_ms non e' misurato ma stimato: 2 x owd + correzione ricavata dai nodi che hanno una misura vera
};
// Riga di log sul protocollo che il gioco usa ("Pia", "PRUDP v1" o "sconosciuto", con i primi byte di un pacchetto sconosciuto).
// Ritorna 0 se non e' arrivato niente di nuovo dall'ultima volta (cosi' non si ripete la riga).
int UdsProtoSummary(char* buf, int cap);
int UdsRttSnapshot(NodeRtt out[8]);         // ritorna quanti nodi hanno almeno un campione recente (< 30 s)

} // namespace azp
