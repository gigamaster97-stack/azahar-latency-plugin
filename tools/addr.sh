#!/bin/sh
# Risolve indirizzi del plugin (dai log di crash di Azahar) in funzioni. Uso: addr.sh 0x070559A0 0x0702B680 ...
export PATH=/opt/devkitpro/devkitARM/bin:$PATH
cd /src
for a in "$@"; do
    echo "== $a"
    arm-none-eabi-addr2line -f -C -e AzaharPing.elf "$a"
done
echo "== simboli vicini (nm)"
arm-none-eabi-nm -C -n AzaharPing.elf > /tmp/nm.txt
for a in "$@"; do
    h=$(printf '%08x' $((a)))
    awk -v h="$h" '$1 <= h { l=$0 } $1 > h { print l; exit }' /tmp/nm.txt
done
