#!/bin/bash
# M5-2c2 stage a -- one iteration: start a real STC client, record the identity
# the guard captured, kill it, and watch which ADR-0040 row it falls into.
#   iter.sh <tag> <watch-secs> [contend]
# "contend" starts a COMPETING address space right after the kill, to test
# whether an ASID is ever reused with a DIFFERENT ASCB (row 3).
set -u
tag=$1; secs=$2; contend=${3:-no}
cd "$(dirname "$0")"
LOG=/home/mike/MVSCE/mvslog.txt

zowe zos-console issue command "S TSTAPPDS,P=HANG" --cn nsfmeas >/dev/null 2>&1
sleep 14
id=$(ssh mvsdev "grep 'HANG ARM -- ASCB=' $LOG | tail -1")
ascb=$(echo "$id" | sed -n 's/.*ASCB=\([0-9A-F]*\).*/\1/p')
asid=$(echo "$id" | sed -n 's/.*ASID=\([0-9A-F]*\).*/\1/p')
[ -z "$ascb" ] && { echo "$tag: NO IDENTITY -- iteration void"; exit 1; }
adec=$((16#$asid))
echo "$tag: recorded ASCB=$ascb ASID=$asid ($adec) contend=$contend"

mark=$(ssh mvsdev "wc -l < $LOG")
python3 rowwatch.py "$adec" "$ascb" "$secs" 1.0 > "$tag.log" 2>&1 &
wpid=$!
sleep 3
zowe zos-console issue command "C TSTAPPDS" --cn nsfmeas >/dev/null 2>&1
if [ "$contend" = contend ]; then
    sleep 1
    zowe zos-console issue command "S TSTAPPDS,P=CLEAN" --cn nsfmeas >/dev/null 2>&1
fi
wait $wpid
grep -E '^ ' "$tag.log"
ssh mvsdev "tail -n +$mark $LOG" | grep -E 'NSF057I|NSF058I' | sed "s/^/$tag: /"
