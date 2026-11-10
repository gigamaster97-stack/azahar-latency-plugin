# AzaharPing

**English summary.** A 3GX plugin for the [Azahar](https://azahar-emu.org) 3DS emulator (CTRPluginFramework, devkitARM). It shows, on the top screen, the
**latency to the other players of a local-wireless (UDS) match**, read from the game's own packets: nothing is sent, nothing is needed on the
other players' side, no configuration. One line per player, `(H)` marks the host (UDS node 1, whoever it is). Supported: **Pia** games
(timer echo: Kirby Battle Royale, Monster Hunter XX / 4 Ultimate, Mario Party: Star Rush, Tri Force Heroes, Fantasy Life, Animal Crossing:
New Leaf) and **PRUDP v1** games (ping with a stamp, PING ACKs, game clock: Final Fantasy Explorers, Pokemon X). Anything else (for example
the encrypted PRUDP of Mario Kart 7) shows nothing; with `debug = 1` the log says which protocol it saw, with the first bytes of an unknown packet.
The value is what the player feels: it includes the wait for the other game's next frame. Tried only between Azahar instances and once
against a remote room, never on a console. GPL-2.0-or-later, ENet (vendored, unused) is MIT. A version for the real 3DS inside
[3DS Link](https://github.com/gionko/3DS-Link) (log only, no overlay) is a separate patch.

Build: `docker run --rm -v "${PWD}:/src" -w /src pablomk7/libctrpf:0.8.0 make` -> `AzaharPing.3gx`. Install: copy it to
`<Azahar user dir>/sdmc/luma/plugins/default.3gx` and enable the plugin loader.

---

Plugin 3GX per **Azahar** che mostra sullo schermo superiore la **latenza verso gli altri giocatori** di una partita
multiplayer, senza configurazione e **senza che gli altri giocatori abbiano il plugin**. Gira solo dentro l'emulatore.
Licenza GPL-2.0-or-later (vedi `LICENSE`); ENet in `third_party/enet` Ã¨ MIT (`third_party/enet/LICENSE`).

## Che cosa si vede

Una riga per ogni altro giocatore connesso: `(H)` davanti al nome dell'host della partita (chiunque sia: Ã¨ il nodo 1 della
sessione di rete, non Ã¨ fisso), poi il nome (da `nwm::UDS`, altrimenti `P1..Pn`, con `#nodo` se due nomi sono uguali) e il
valore, per esempio `PlayerB 31ms`. Fuori da una partita non si vede niente. Solo testo, con un'ombra nera di 1 pixel.
Colore sul valore mostrato: blu fino a 30 ms, verde 31-60, giallo 61-100, arancione 101-150, rosso oltre. Se per qualche
secondo non arrivano campioni nuovi il valore resta, in grigio e tra parentesi.

## Come misura

Il plugin si aggancia alle funzioni `SendTo` e `PullPacket` dell'SDK di `nwm::UDS` dentro il gioco (le trova da solo) e
**osserva i pacchetti che il gioco stesso manda e riceve**, senza modificarli e senza mandarne di suoi. Il protocollo si
riconosce dai byte, non da una lista di giochi; il log (con `debug = 1`) dice quale ha riconosciuto:

```
protocol: Pia (Pia=17036 PRUDPv1=0 other=3451) | unknown S size=140: 02 01 01 FF ...
```

* **Pia** (fino alla 5.10; Kirby, Monster Hunter XX, Tri Force Heroes, Fantasy Life, Monster Hunter 4 Ultimate, Animal
  Crossing New Leaf: provati): ogni pacchetto porta il timer del mittente e, rimandato indietro, l'ultimo timer ricevuto.
  Il tempo da quando ho mandato un timer a quando un nodo me lo rimanda Ã¨ un giro completo con quel nodo.
* **PRUDP v1** (Final Fantasy Explorers: provato): non c'Ã¨ un timer con eco. Si usa l'orologio di gioco che ogni nodo
  mette nei pacchetti di stato (~15 al secondo per collegamento): ritardo di un verso = mio orologio all'arrivo meno il
  suo orologio nel pacchetto, mediana su 8 s, giro mostrato = 2 Ã— ritardo di un verso. Funziona giÃ  dalla lobby e anche
  tra due client. Dove manca l'orologio si ripiega sull'eco del ping verso l'host (ogni ~2 s) e sulle conferme.
* **Sconosciuto**: niente valori. Nel log restano la riga `protocol:` con i primi byte di un pacchetto sconosciuto, per
  analizzarlo (con `uds_trace = 1` anche un CSV di tutti i pacchetti).

L'orologio Ã¨ quello **dell'host** del PC (`svcGetSystemInfo(0x20000, 1)`), non quello del guest, quindi vale anche con
emulazione sotto il 100%.

### Cosa Ã¨ vero di questi valori

* Con tre istanze sullo stesso PC il ritardo di rete Ã¨ ~0: il numero Ã¨ soprattutto la **cadenza dei frame del gioco**
  (15-35 ms), che comunque fa parte di quello che il giocatore sente. Contro il ritardo di consegna reale (stessi
  pacchetti visti da chi li manda e da chi li riceve): FFE da Â±5 a Â±15 ms; Pia in lobby di pochi ms sotto, in missione
  (Monster Hunter 4 Ultimate) fino a 10-20 ms sopra.
* Tra due client in FFE l'errore dipende dallo scarto tra i loro orologi di gioco e puÃ² arrivare a ~15 ms: le due viste della
  stessa coppia possono non coincidere esattamente. Nei primi ~30 s dopo l'ingresso di un client i valori che lo
  riguardano oscillano mentre l'orologio si allinea.
* Provato con un ritardo di 100 ms per verso (proxy UDP in `azahar-multi/delay_proxy.py`) su FFE: i collegamenti
  rallentati sono risultati ~250 ms.
* **Non provato con rete vera** (internet): solo istanze locali.

## Giochi provati

| Gioco | Protocollo | Esito |
|---|---|---|
| Kirby Battle Royale, Monster Hunter XX, Mario Party: Star Rush | Pia | valori coerenti |
| Final Fantasy Explorers | PRUDP v1 | valori da orologio di gioco, anche client-client |
| The Legend of Zelda: Tri Force Heroes, Fantasy Life | Pia | ~30 ms stabili, coppie coerenti entro 1-3 ms |
| PokÃ©mon X (scambio/lotta locale, 2 istanze) | PRUDP v1 (stesso ping con eco da 79/91 byte di FFE, conferme dei PING) | riconosciuto da solo; ~50 ms contro ~30 ms di consegna reale (il valore comprende l'attesa del frame dell'altro gioco, circa +20 ms); le due viste concordano (50/51) |
| Monster Hunter 4 Ultimate | Pia + pacchetto proprio di Capcom da 140 byte (solo dall'host) | il pacchetto proprio Ã¨ ignorato; valori sopra il reale in missione |
| Animal Crossing New Leaf | Pia | pochi pacchetti (~3/s); sessione interrotta, non completata |
| Mario Kart 7 | PRUDP v0, cifrato | **non supportato** (non provato) |

## File

| | |
|---|---|
| `src/uds_trace.cpp/.hpp` | aggancio a `SendTo`/`PullPacket`, riconoscimento del protocollo, stima della latenza (Pia, PRUDP v1) |
| `src/uds_probe.cpp/.hpp` | lettura sola dei nodi di `nwm::UDS` (nomi, host) |
| `src/main.cpp` | colla CTRPluginFramework: thread di rete, OSD, menu, ini, log |
| `src/ping_core.c/.h`, `src/enet_3ds.c`, `third_party/enet` | sonda ENet verso una stanza: **spenta** (non serve piÃ¹; resta nel codice) |
| `../azahar-multi/` | tre istanze portatili di Azahar con script per provare (`install_plugin.ps1`, `launch_game.ps1 -Rom ...`, `delay_proxy.py`) |

## Compilare

```powershell
docker run --rm -v "${PWD}:/src" -w /src pablomk7/libctrpf:0.8.0 make
```

Esce `AzaharPing.3gx`. Per risolvere un indirizzo di crash letto dal log di Azahar: `tools/addr.sh`.

## Installare (utente)

1. Copia `AzaharPing.3gx` in `<cartella utente di Azahar>/sdmc/luma/plugins/default.3gx` (per tutti i giochi) oppure in
   `.../luma/plugins/<TitleID a 16 cifre MAIUSCOLE>/` (per un gioco solo).
2. In Azahar deve essere attivo il plugin loader (`plugin_loader=true` e `plugin_loader\default=false` in `qt-config.ini`).
3. Nessuna configurazione: parte da solo quando il gioco apre `nwm::UDS`. Opzioni in `<sdmc>/azahar_ping.ini`:
   `corner` (0 alto-sx, 1 alto-dx, 2 basso-sx, 3 basso-dx), `show`, `players`, `names` (0 = solo P1..Pn),
   `refresh_hz`, `debug` (`debug = 1` scrive `<sdmc>/azahar_ping.log`, compresa la riga `protocol:`), `uds_trace`
   (CSV di tutti i pacchetti: pesante, solo per analisi), `uds_diag`.

**Stati salvati:** un salvataggio di stato di Azahar contiene la memoria del gioco, quindi anche il plugin: caricarlo fa
girare la versione del plugin presente al momento del salvataggio, non quella appena installata.

## Costo di risorse (misurato in Azahar, 2026-11-10)

Fantasy Life (Pia), due istanze nella lobby iniziale, misura sull'host con `debug = 1`, `uds_trace = 0`, `uds_diag = 0`. Il log
(`perf:` ogni 10 s) riporta il tempo speso dal plugin in tick ARM11 (268 MHz) come % di UN core. Sono tick di guest emulato:
ordine di grandezza, non una misura su console.

| | ping a schermo (`show = 1`) | ping nascosto (`show = 0`) |
|---|---|---|
| aggancio ai pacchetti (~240 chiamate/s, 2-3 Âµs l'una) | 0,07 % | 0,07 % |
| thread di rete (stima, nomi, log) | 2,7-3,2 % | 2,8-3,4 % |
| disegno (60 fps; 1 giocatore, testo + ombra) | ~10 % (1,7 ms/fotogramma) | ~0,9 % (148 Âµs/fotogramma) |
| **totale** | **~13 %** | **~4 %** |

Memoria: immagine caricata ~0,83 MB (codice 454 KB, costanti 129 KB, dati 7 KB, bss 240 KB, di cui 152 KB Ã¨ l'anello dei
pacchetti); heap dinamico usato 1,17 MB; 32 KB di stack per il thread di rete. Il 3gx dichiara 5 MiB di memoria al
plugin loader. Con piÃ¹ giocatori il costo cresce: una riga di testo in piÃ¹ e piÃ¹ pacchetti agganciati.

## Limiti noti

* Giochi con protocollo sconosciuto o cifrato (PRUDP v0 come Mario Kart 7, Pia dalla 5.11 che non ha piÃ¹ i timer) non hanno
  valori.
* Giochi con pochi pacchetti (Animal Crossing) aggiornano il valore di rado.
* Il valore include l'attesa del gioco (cadenza dei frame, carico dell'emulazione): Ã¨ la latenza sentita dal giocatore, non
  il solo ritardo di rete.
