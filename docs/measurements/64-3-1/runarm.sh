#!/bin/zsh
# 64-3-1 Stage B gate -- one arm.  $1 = arm name, $2 = seconds.
# ONE sampler, started fresh, with any stray killed first: arm 1 was
# contaminated by two concurrent samplers writing one file.
set -e
SP="$(dirname "$0")"
ARM="$1"; SECS="${2:-480}"
cd /Users/mike/repos/mvs/nsf370
set -a; . ./.env; set +a
pkill -f nsfswatch.py 2>/dev/null || true
sleep 1
[ "$(pgrep -fc nsfswatch.py 2>/dev/null || echo 0)" -eq 0 ] || { echo "stray sampler still up"; exit 1; }
rm -f "$SP/gate-$ARM.log"
python3 -u "$SP/nsfswatch.py" NSFS "$SECS" > "$SP/gate-$ARM.log" 2>&1 &
SAMP=$!
echo "arm=$ARM sampler pid=$SAMP secs=$SECS"
sleep 25
for r in $(seq 1 14); do
  zowe zos-jobs submit local-file "$SP/TSTRQXCA.jcl" >/dev/null 2>&1 &
  zowe zos-jobs submit local-file "$SP/TSTRQXCB.jcl" >/dev/null 2>&1 &
  wait
  sleep 18
done
echo "rounds submitted; waiting for sampler"
wait $SAMP
echo "=== ARM $ARM COMPLETE ==="
tail -2 "$SP/gate-$ARM.log"
