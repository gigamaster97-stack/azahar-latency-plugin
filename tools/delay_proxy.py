"""Proxy UDP che aggiunge un ritardo fisso in OGNI direzione tra un client Azahar e la stanza (prova della stima di latenza).
Uso:  python delay_proxy.py [--listen 24873] [--room 127.0.0.1:24872] [--delay-ms 100]
Il client da rallentare si collega a 127.0.0.1:<listen> invece che alla stanza. Il giro in piu' e' 2 x delay-ms."""
import argparse, heapq, socket, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--listen', type=int, default=24873)
ap.add_argument('--room', default='127.0.0.1:24872')
ap.add_argument('--delay-ms', type=float, default=100.0)
a = ap.parse_args()
room = (a.room.split(':')[0], int(a.room.split(':')[1]))
delay = a.delay_ms / 1000.0

lsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
lsock.bind(('127.0.0.1', a.listen))
sessions = {}          # indirizzo client -> socket verso la stanza
heap = []              # (ora di invio, contatore, socket, dati, destinazione)
cv = threading.Condition()
counter = 0
stats = {'up': 0, 'down': 0}

def schedule(sock, data, dest):
    global counter
    with cv:
        counter += 1
        heapq.heappush(heap, (time.monotonic() + delay, counter, sock, data, dest))
        cv.notify()

def sender():
    while True:
        with cv:
            while not heap or heap[0][0] > time.monotonic():
                cv.wait(timeout=max(0.0005, heap[0][0] - time.monotonic()) if heap else 1.0)
            _, _, sock, data, dest = heapq.heappop(heap)
        try:
            sock.sendto(data, dest)
        except OSError:
            pass

def downstream(client, usock):
    while True:
        try:
            data, _ = usock.recvfrom(65535)
        except ConnectionResetError:
            continue   # Windows segnala cosi' un ICMP "porta non raggiungibile" ricevuto in precedenza
        except OSError:
            return
        stats['down'] += 1
        schedule(lsock, data, client)

threading.Thread(target=sender, daemon=True).start()
print(f'proxy: 127.0.0.1:{a.listen} -> {room[0]}:{room[1]}, ritardo {a.delay_ms} ms per verso', flush=True)
last = time.time()
while True:
    try:
        data, client = lsock.recvfrom(65535)
    except ConnectionResetError:
        continue
    usock = sessions.get(client)
    if usock is None:
        usock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        usock.bind(('127.0.0.1', 0))   # su Windows recvfrom su un socket UDP non legato da' errore: il thread di ritorno moriva subito
        sessions[client] = usock
        threading.Thread(target=downstream, args=(client, usock), daemon=True).start()
        print('proxy: nuovo client', client, flush=True)
    stats['up'] += 1
    schedule(usock, data, room)
    if time.time() - last > 30:
        last = time.time()
        print('proxy: pacchetti su', stats['up'], 'giu', stats['down'], flush=True)
