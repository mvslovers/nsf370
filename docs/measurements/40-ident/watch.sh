#!/bin/sh
# Poll ONE ASCB repeatedly.  $1 = ascb hex, $2 = seconds, $3 = label
end=$(( $(date +%s) + $2 ))
while [ $(date +%s) -lt $end ]; do
  echo "--- $(date -u +%H:%M:%S)Z $3"
  python3 arm1.py "$1" 2>&1 | sed -n '/ASCBs/,$p' | tail -4
  sleep 4
done
