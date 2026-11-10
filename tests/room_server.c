/*
 * Finto room server di Azahar, con la libreria ENet vera, per provare ping_core su PC.
 * Riproduce quello che conta di src/network/room.cpp:
 *   - enet_host_create(&address, max_connections + 1, NumChannels = 1, 0, 0)
 *   - un solo ciclo enet_host_service(server, &event, 16)
 *   - chi si collega e non manda IdJoinRequest non e' un membro: la stanza non fa nulla per lui
 *   - alla DISCONNECT di un peer che non e' membro non succede nulla
 * Uso: room_server <porta> [max_membri]
 * Scrive sullo stdout una riga per ogni CONNECT / DISCONNECT, con il numero di peer collegati.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "enet/enet.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
    unsigned port = argc > 1 ? (unsigned)atoi(argv[1]) : 24872;
    unsigned max_members = argc > 2 ? (unsigned)atoi(argv[2]) : 16;
    ENetAddress address;
    ENetHost *server;
    ENetEvent event;
    unsigned connected = 0;

    if (enet_initialize() != 0)
        return 2;
    address.host = ENET_HOST_ANY;
    address.port = (enet_uint16)port;
    server = enet_host_create(&address, max_members + 1, 1, 0, 0);
    if (!server) {
        fprintf(stderr, "enet_host_create failed\n");
        return 3;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    printf("room_server: porta %u, %u peer (membri %u + 1 di scorta)\n", port, max_members + 1, max_members);
    fflush(stdout);

    while (!g_stop) {
        if (enet_host_service(server, &event, 16) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                ++connected;
                printf("CONNECT peers=%u\n", connected);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (connected)
                    --connected;
                printf("DISCONNECT peers=%u\n", connected);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                printf("RECEIVE id=%u len=%u (ignorato)\n", event.packet->dataLength ? event.packet->data[0] : 0,
                       (unsigned)event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
            default:
                break;
            }
            fflush(stdout);
        }
    }
    enet_host_destroy(server);
    enet_deinitialize();
    return 0;
}
