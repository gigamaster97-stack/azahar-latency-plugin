#!/bin/bash
# Prove di ping_core su Linux, contro un finto room server con la libreria ENet vera.
# Si lancia dalla cartella plugin/ (vedi tests/run_tests.ps1, che usa Docker).
cd "$(dirname "$0")/.." || exit 2
B=/tmp/b; mkdir -p $B
CF="-O2 -Wall -std=gnu99 -Ithird_party/enet/include -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETNAMEINFO=1 -DHAS_GETADDRINFO=1 -DHAS_GETHOSTBYNAME_R=1 -DHAS_GETHOSTBYADDR_R=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1"
ENET="third_party/enet/src/*.c third_party/enet/platform/unix.c"
gcc $CF -o $B/room_server tests/room_server.c $ENET || exit 2
gcc $CF -o $B/probe_cli tests/probe_cli.c src/ping_core.c $ENET || exit 2
echo "build ok"

FAIL=0
check() { # check <descrizione> <condizione shell>
    if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; FAIL=1; fi
}
last_field() { grep -E "^ *[0-9]+ " "$1" | tail -1 | sed -E "s/.* $2=(-?[0-9]+).*/\1/"; }

start_server() { $B/room_server 24872 16 > $B/server.log 2>&1 & SERVER=$!; sleep 0.3; }

echo "== 1. loopback, 4 s"
start_server
$B/probe_cli 127.0.0.1 24872 4 > $B/t1.log
sleep 0.3
kill $SERVER; wait $SERVER 2>/dev/null
tail -3 $B/t1.log
check "stato ONLINE alla fine" "tail -1 $B/t1.log | grep -q ONLINE"
R=$(last_field $B/t1.log rtt)
check "rtt tra 1 e 15 ms (letto: $R)" "[ \"$R\" -ge 1 ] && [ \"$R\" -le 15 ]"
check "la stanza ha visto CONNECT" "grep -q 'CONNECT peers=1' $B/server.log"
check "la stanza ha visto il DISCONNECT educato alla chiusura" "grep -q 'DISCONNECT peers=0' $B/server.log"
check "la stanza non ha ricevuto pacchetti (niente join)" "! grep -q RECEIVE $B/server.log"

echo "== 2. latenza simulata (tc netem 30 ms per verso => RTT ~60 ms), 8 s"
if tc qdisc add dev lo root netem delay 30ms 2>/dev/null; then
    start_server
    $B/probe_cli 127.0.0.1 24872 8 > $B/t2.log
    kill $SERVER; wait $SERVER 2>/dev/null
    tc qdisc del dev lo root
    tail -4 $B/t2.log
    R=$(last_field $B/t2.log rtt)
    check "rtt tra 55 e 80 ms (letto: $R)" "[ \"$R\" -ge 55 ] && [ \"$R\" -le 80 ]"
    # col seme dell'handshake il numero non deve partire da 500: entro 1.5 s deve essere gia' < 120
    EARLY=$(awk '$1>1250 && $1<1750 {print}' $B/t2.log | head -1 | sed -E 's/.* rtt=(-?[0-9]+).*/\1/')
    check "dopo ~1.5 s il numero e' gia' vicino (letto: $EARLY)" "[ -n \"$EARLY\" ] && [ \"$EARLY\" -le 120 ]"
else
    echo "  SKIP tc netem non disponibile (serve --cap-add NET_ADMIN)"
fi

echo "== 3. la stanza sparisce e ritorna, 16 s"
start_server
$B/probe_cli 127.0.0.1 24872 16 > $B/t3.log &
PROBE=$!
sleep 3;  kill -STOP $SERVER          # la stanza smette di rispondere (ma il processo c'e')
sleep 4;  kill -CONT $SERVER          # riprende
sleep 3;  kill $SERVER; wait $SERVER 2>/dev/null   # la stanza chiude davvero
sleep 1;  start_server                # e una nuova ritorna
wait $PROBE
awk '{print $2}' $B/t3.log | uniq | tr '\n' ' '; echo
awk 'NR%4==0' $B/t3.log; cat $B/server.log | tail -5
check "ha visto NO_ANSWER quando la stanza non rispondeva" "grep -q NO_ANSWER $B/t3.log"
check "ha visto CONNECTING quando la stanza e' sparita" "grep -q CONNECTING $B/t3.log"
check "e' tornato ONLINE alla fine" "tail -1 $B/t3.log | grep -q ONLINE"
C=$(last_field $B/t3.log connects)
check "ha riconnesso (connects>=2, letto: $C)" "[ \"$C\" -ge 2 ]"
kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null

echo "== 4. nome non risolvibile / nessuna stanza"
$B/probe_cli nonesiste.invalid 24872 3 > $B/t4.log
check "stato ERROR senza crash" "grep -q ERROR $B/t4.log"

echo
if [ $FAIL = 0 ]; then echo "TUTTO OK"; else echo "QUALCOSA E' FALLITO"; fi
exit $FAIL
