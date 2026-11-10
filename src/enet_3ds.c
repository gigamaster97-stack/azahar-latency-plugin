/*
 * enet_3ds.c - strato di piattaforma di ENet per il plugin (libctru, socket BSD di soc:U).
 *
 * Derivato da unix.c di ENet (MIT, Lee Salzman) e dal file omonimo di 3DS Link (AzaharLink/Sources/enet/enet_3ds.c),
 * ridotto al minimo: qui il plugin gira solo dentro Azahar, quindi niente gestione dei buffer del servizio soc,
 * niente gate per lo scambio di memoria dell'Old 3DS, niente statistiche.
 *
 * Differenze che contano:
 *  - L'orologio. enet_time_get() usa l'orologio dell'HOST di Azahar (svcGetSystemInfo(0x20000, 1), "ns dell'host, non
 *    influenzato da lag o velocita' della CPU"), non il tick di sistema del guest: se l'emulazione va al 60% o la
 *    partita rallenta, il tick del guest scorre piu' piano del tempo reale e ogni RTT verrebbe sbagliato. Fuori
 *    da Azahar/Citra (la chiamata fallisce) si ricade su svcGetSystemTick.
 *  - libctru non ha sendmsg/recvmsg: scatter/gather emulato con sendto/recvfrom.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

#include <3ds.h>

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"

static enet_uint32 timeBase = 0;
static int hostClock = -1; /* -1 = non ancora provato, 1 = orologio dell'host, 0 = tick di sistema */

static enet_uint32
enet_3ds_now_ms (void)
{
    if (hostClock != 0)
    {
        s64 ns = 0;
        if (R_SUCCEEDED (svcGetSystemInfo (& ns, 0x20000, 1)) && ns > 0)
        {
            hostClock = 1;
            return (enet_uint32) ((unsigned long long) ns / 1000000ULL);
        }
        hostClock = 0;
    }
    return (enet_uint32) ((svcGetSystemTick () * 1000ULL) / SYSCLOCK_ARM11);
}

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) svcGetSystemTick ();
}

enet_uint32
enet_time_get (void)
{
    return enet_3ds_now_ms () - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    timeBase = enet_3ds_now_ms () - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    if (! inet_pton (AF_INET, name, & address -> host))
        return -1;

    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    struct addrinfo hints, * resultList = NULL, * result = NULL;

    /* un indirizzo puntato non ha bisogno del resolver */
    if (enet_address_set_host_ip (address, name) == 0)
        return 0;

    memset (& hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo (name, NULL, & hints, & resultList) != 0)
        return -1;

    for (result = resultList; result != NULL; result = result -> ai_next)
    {
        if (result -> ai_family == AF_INET && result -> ai_addr != NULL &&
            result -> ai_addrlen >= sizeof (struct sockaddr_in))
        {
            struct sockaddr_in * sin = (struct sockaddr_in *) result -> ai_addr;

            address -> host = sin -> sin_addr.s_addr;

            freeaddrinfo (resultList);

            return 0;
        }
    }

    if (resultList != NULL)
        freeaddrinfo (resultList);

    return -1;
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    if (inet_ntop (AF_INET, & address -> host, name, nameLength) == NULL)
        return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    /* niente risoluzione inversa: si da' l'indirizzo numerico */
    return enet_address_get_host_ip (address, name, nameLength);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
       sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
       sin.sin_addr.s_addr = address -> host;
    }
    else
    {
       sin.sin_port = 0;
       sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    if (getsockname (socket, (struct sockaddr *) & sin, & sinLength) == -1)
      return -1;

    address -> host = (enet_uint32) sin.sin_addr.s_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);

    return 0;
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? 4 : backlog);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = -1;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
            result = fcntl (socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl (socket, F_GETFL) & ~O_NONBLOCK));
            break;

        case ENET_SOCKOPT_BROADCAST:
            /* soc:U non ha SO_BROADCAST per UDP (la stanza e' unicast): non si chiede */
        case ENET_SOCKOPT_RCVBUF:
        case ENET_SOCKOPT_SNDBUF:
            /* ENet chiede 256 KB: il buffer del servizio soc qui e' di 128 KB, lo lasciamo com'e' */
            (void) value;
            result = 0;
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = -1;
    socklen_t len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof (int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, value, & len);
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;
    int result;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    result = connect (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    if (result == -1 && errno == EINPROGRESS)
      return 0;

    return result;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    int result;
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    result = accept (socket,
                     address != NULL ? (struct sockaddr *) & sin : NULL,
                     address != NULL ? & sinLength : NULL);

    if (result == -1)
      return ENET_SOCKET_NULL;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return result;
}

int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how);
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != -1)
      close (socket);
}

/* il datagramma piu' grande che ENet costruisce (ENET_PROTOCOL_MAXIMUM_MTU) */
#define ENET_3DS_DGRAM_MAX 4096

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct sockaddr_in sin;
    unsigned char gather [ENET_3DS_DGRAM_MAX];
    size_t total = 0, i;
    int sentLength;

    for (i = 0; i < bufferCount; ++ i)
    {
        if (total + buffers [i].dataLength > sizeof (gather))
            return -1;
        memcpy (gather + total, buffers [i].data, buffers [i].dataLength);
        total += buffers [i].dataLength;
    }

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;

        sentLength = sendto (socket, gather, total, 0, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    }
    else
        sentLength = send (socket, gather, total, 0);

    if (sentLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

    return sentLength;
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);
    int recvLength;

    if (bufferCount == 1)
    {
        recvLength = recvfrom (socket, buffers [0].data, buffers [0].dataLength, 0,
                               (struct sockaddr *) & sin, & sinLength);
    }
    else
    {
        unsigned char scatter [ENET_3DS_DGRAM_MAX];
        size_t remaining, i, offset = 0;

        recvLength = recvfrom (socket, scatter, sizeof (scatter), 0,
                               (struct sockaddr *) & sin, & sinLength);
        if (recvLength > 0)
        {
            remaining = (size_t) recvLength;
            for (i = 0; i < bufferCount && remaining > 0; ++ i)
            {
                size_t chunk = remaining < buffers [i].dataLength ? remaining : buffers [i].dataLength;
                memcpy (buffers [i].data, scatter + offset, chunk);
                offset += chunk;
                remaining -= chunk;
            }
            if (remaining > 0)
                return -2;
        }
    }

    if (recvLength == -1)
    {
        switch (errno)
        {
            case EWOULDBLOCK:
                return 0;
            case EINTR:
            case EMSGSIZE:
                return -2;
            default:
                return -1;
        }
    }

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return recvLength;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    /* serve solo agli helper TCP opzionali di ENet; un client UDP non lo chiama */
    (void) maxSocket; (void) readSet; (void) writeSet; (void) timeout;
    return -1;
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
    struct pollfd pollSocket;
    int pollCount;

    pollSocket.fd = socket;
    pollSocket.events = 0;
    pollSocket.revents = 0;

    if (* condition & ENET_SOCKET_WAIT_SEND)
      pollSocket.events |= POLLOUT;

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      pollSocket.events |= POLLIN;

    /* Un poll() BLOCCANTE di soc:U, in Azahar, e' un lavoro asincrono (src/core/hle/service/soc/soc_u.cpp, Poll con
       RunAsync): il guest viene risvegliato solo quando l'evento torna sulla timeline emulata, e ogni RTT si
       allunga di diversi millisecondi (misurato: ~8 ms su loopback invece di ~1). Un poll con timeout 0 e' invece
       sincrono, quindi si attende noi: poll(0) e uno sleep di 1 ms fino alla scadenza. */
    if (timeout == 0)
        pollCount = poll (& pollSocket, 1, 0);
    else
    {
        const enet_uint32 started = enet_3ds_now_ms ();

        for (;;)
        {
            pollCount = poll (& pollSocket, 1, 0);
            if (pollCount != 0 || (enet_uint32) (enet_3ds_now_ms () - started) >= timeout)
                break;
            svcSleepThread (1000000LL);
        }
    }
    if (pollCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (pollCount == 0)
      return 0;

    if (pollSocket.revents & POLLOUT)
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (pollSocket.revents & POLLIN)
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
}
