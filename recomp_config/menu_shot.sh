#!/usr/bin/env bash
# Boot, press through the main menu with fixed waits, and screenshot each step
# (GZ_SHOT_FILE). Extra environment comes from the caller.
# Usage: menu_shot.sh <work dir> <step>...   step = "<pad inputs>" e.g. "down:250" "a:400"
W=${1:?work dir}; shift; P="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$W"; : > "$W/pad.txt"; cd $P
GZ_MOVIES=0 GZ_MOVIE_SKIP_FRAMES=20 GZ_RENDER_SCALE=${GZ_RENDER_SCALE:-2} GZ_SHOT_FILE=$W/shot RECOMP_PAD_LIVE=$W/pad.txt \
RECOMP_USB=1 RECOMP_KERNEL_LOG_BUDGET=200 RECOMP_PB_EXEC=1 RECOMP_PB_WALK=dma RECOMP_AC97_READY=1 RECOMP_WATCHDOG_SECS=300 \
  ./build/Release/godzilla_ste_recomp.exe > "$W/run.log" 2>&1 &
G=$!; t=0
until [ "$(grep -c MOVIE "$W/run.log")" -ge 6 ] || ! kill -0 $G 2>/dev/null || [ $t -ge 120 ]; do sleep 1; t=$((t+1)); done
sleep ${MENU_WAIT:-9}; k=0
shot() { touch "$W/shot"; local t=0; until [ ! -f "$W/shot" ] || [ $t -ge 10 ]; do sleep 1; t=$((t+1)); done; [ -f "$W/shot.bmp" ] && mv "$W/shot.bmp" "$W/step_$1.bmp"; }
shot 0
for step in "$@"; do k=$((k+1)); [ "$step" != "wait" ] && echo "$step" | tr " " "
" >> "$W/pad.txt"; sleep ${STEP_WAIT:-4}; shot $k; done
# SHOT_AT="<epoch> <epoch>...": extra screenshots at absolute times (two instances in step).
for at in $SHOT_AT; do while [ $(date +%s) -lt $at ]; do sleep 0.2; done; shot at_$at; done
echo "alive: $(kill -0 $G 2>/dev/null && echo yes || echo no)"; grep -E "CRASH|TOURNAMENT" "$W/run.log" | head -3
kill $G 2>/dev/null; wait $G 2>/dev/null
for f in "$W"/step_*.bmp; do py -3 -I $P/recomp_config/tools/bmp2png.py "$f" "${f%.bmp}.png" >/dev/null 2>&1; done
