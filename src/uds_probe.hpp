// uds_probe - lettura SOLA LETTURA dello stato della partita UDS (nwm::UDS) dal plugin, per capire chi e' l'host.
//
// Si usa solo GetNodeInformation (0x000D): non cambia nulla nello stato di Azahar. NON si usa GetConnectionStatus (0x000B):
// nel sorgente di Azahar azzera `changed_nodes` a ogni chiamata, quindi il plugin toglierebbe al gioco gli avvisi
// "un giocatore e' entrato/uscito". Non si chiama mai Initialize/Shutdown/Bind: lo stato di nwm::UDS in Azahar e' unico per
// tutto il servizio e il plugin spezzerebbe la sessione del gioco.
#pragma once

#include <3ds.h>

namespace azp {

struct UdsNode {
    u16 id = 0;          // numero di nodo nella rete UDS (1 = host)
    u64 seed = 0;        // friend code seed (identifica la console)
    char name[32] = {};  // nome del giocatore come lo dice il gioco (UTF-16 -> UTF-8; max 10 caratteri)
};

struct UdsSnapshot {
    bool service_ok = false;  // sessione con nwm::UDS aperta
    Result first_error = 0;   // errore di GetNodeInformation(1) se nessun nodo (0 = nessun errore)
    int count = 0;
    UdsNode nodes[8];
};

bool UdsOpen();                          // apre la sessione con nwm::UDS (una volta)
void UdsClose();
bool UdsRead(UdsSnapshot* out);          // legge i nodi 1..8; false se il servizio non e' aperto
bool UdsMyName(char* out, int size);     // il nome utente di questa console (cfg:u), false se non si legge

} // namespace azp
