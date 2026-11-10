#!/usr/bin/env bash
# Boot straight into a fight (GZ_QUICK_FIGHT), take full-resolution screenshots
# through GZ_SHOT_FILE, then quit. Extra environment (GZ_RENDER_SCALE=3 ...)
# comes from the caller.  Usage: qf_shot.sh <work dir> [shots=2] [fight secs=14]
W=${1:?work dir}; N=${2:-2}; SECS=${3:-14}; P="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$W"; : > "$W/pad.txt"; cd $P
# PRE_PAD="lt+rt:60000": a pad line written before the game starts (held through the launch).
[ -n "$PRE_PAD" ] && echo "$PRE_PAD" >> "$W/pad.txt"
GZ_MOVIES=0 GZ_QUICK_FIGHT=1 GZ_MOVIE_SKIP_FRAMES=20 GZ_QF_DELAY=60 GZ_SHOT_FILE=$W/shot RECOMP_PAD_LIVE=$W/pad.txt \
RECOMP_USB=1 RECOMP_PB_INVENTORY=0 RECOMP_HEAP_RECLAIM=1 RECOMP_KERNEL_LOG_BUDGET=200 RECOMP_PB_EXEC=1 RECOMP_PB_WALK=dma RECOMP_AC97_READY=1 RECOMP_WATCHDOG_SECS=300 \
  ./build/Release/godzilla_ste_recomp.exe > "$W/run.log" 2>&1 &
G=$!; t=0
until grep -q QUICKFIGHT "$W/run.log" || ! kill -0 $G 2>/dev/null || [ $t -ge 120 ]; do sleep 1; t=$((t+1)); done
# The level is in once the frame rate log shows the fight's draw load; simply wait.
sleep ${LOAD_SECS:-42}
k=0; while [ $k -lt $N ] && kill -0 $G 2>/dev/null; do
  printf '%s\n' lup:1500 a:300 x:300 >> "$W/pad.txt"; sleep $((SECS / N))
  touch "$W/shot"; t=0; until [ ! -f "$W/shot" ] || [ $t -ge 10 ]; do sleep 1; t=$((t+1)); done
  [ -f "$W/shot.bmp" ] && mv "$W/shot.bmp" "$W/shot_$k.bmp"; k=$((k+1))
done
echo "alive: $(kill -0 $G 2>/dev/null && echo yes || echo no); $(grep -c 'fps' "$W/run.log") fps lines; last: $(grep fps "$W/run.log" | tail -3 | awk '{print $2}' | paste -sd' ')"
grep -E "CRASH|error X|screenshot" "$W/run.log" | tail -4
kill $G 2>/dev/null; wait $G 2>/dev/null
