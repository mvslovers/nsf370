#!/bin/sh
# M5-2c1 stage a: poll the app registry with host timestamps.
# $1 = label, $2 = number of polls, $3 = seconds between polls
LABEL="$1"; N="${2:-20}"; GAP="${3:-15}"
i=0
while [ $i -lt "$N" ]; do
  TS=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
  OUT=$(zowe zos-console issue command "F NSFS,APPS" 2>&1 | tr '\n' '|')
  echo "$TS $LABEL $OUT"
  i=$((i+1))
  [ $i -lt "$N" ] && sleep "$GAP"
done
