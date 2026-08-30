#!/bin/bash
# M5-2c2 stage a, addendum -- the TRANSPORT-path DEAD reap, n>1.
#
# Decomposes the death->verdict interval into its two parts, because they are
# not the same kind of thing:
#   (a) ABEND -> completing datagram   -- the PEER's timing. Varied on purpose
#       across runs; in production it is unbounded and NOT a system property.
#   (b) datagram -> NSF050I            -- the STC's own latency once the
#       completing event exists. This one IS a system measurement.
#
#   park.sh <tag> <gap-seconds>
# MVS console time + 7h = host local time (verified this round).
set -u
tag=$1; gap=$2
cd "$(dirname "$0")"
LOG=/home/mike/MVSCE/mvslog.txt

zowe zos-console issue command "S TSTAPPDS,P=PARK" --cn nsfmeas >/dev/null 2>&1
sleep 16
id=$(ssh mvsdev "grep 'PARK ARM -- ASCB=' $LOG | tail -1")
ascb=$(echo "$id" | sed -n 's/.*ASCB=\([0-9A-F]*\).*/\1/p')
asid=$(echo "$id" | sed -n 's/.*ASID=\([0-9A-F]*\).*/\1/p')
[ -z "$ascb" ] && { echo "$tag: NO IDENTITY -- void"; exit 1; }

# THE CONJUNCTION: PENDING alone does not prove a request is outstanding.
zowe zos-console issue command "F NSFS,STATS" --cn nsfmeas >/dev/null 2>&1
sleep 5
busy=$(ssh mvsdev "grep 'NSF813I' $LOG | tail -1")
echo "$tag: ASCB=$ascb ASID=$asid gap=${gap}s"
echo "$tag: outstanding? $(echo "$busy" | sed 's/.*NSF813I/NSF813I/')"

mark=$(ssh mvsdev "wc -l < $LOG")
zowe zos-console issue command "C TSTAPPDS" --cn nsfmeas >/dev/null 2>&1
sleep 4
ab=$(ssh mvsdev "tail -n +$mark $LOG" | grep -oE 'ABEND S222 U0000 - TIME=[0-9.]+' | tail -1 | sed 's/.*TIME=//')

sleep "$gap"
verdict=$(python3 rowwatch.py $((16#$asid)) "$ascb" 2 1.0 2>/dev/null | grep -oE 'LIVE|DEAD-row2-avail|DEAD-row3-mismatch' | tail -1)
send=$(ssh mvsdev "date '+%H.%M.%S'; printf 'X' | timeout 5 nc -u -w2 192.168.200.1 7799" | head -1)

hit=""
for i in $(seq 1 20); do
    hit=$(ssh mvsdev "tail -n +$mark $LOG" | grep -E 'NSF050I' | tail -1)
    [ -n "$hit" ] && break
    sleep 2
done
t050=$(echo "$hit" | grep -oE '^[A-F0-9]+ +[0-9]+\.[0-9]+\.[0-9]+' | awk '{print $2}')
echo "$tag: abend=$ab  verdict_at_send=$verdict  sent=$send(local)  nsf050i=$t050(mvs)"
if [ -n "$hit" ]; then echo "$tag: REAPED -- $(echo "$hit" | sed 's/.*NSF050I/NSF050I/')";
else echo "$tag: NO NSF050I -- guard did NOT reap"; fi
