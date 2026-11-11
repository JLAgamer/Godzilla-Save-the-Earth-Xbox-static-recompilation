#!/usr/bin/env bash
# Quick fight, walk player 1 with PAD_LINE for WALK_SECS, then N times: trace a
# frame (GZ_GPU_TRACE_FILE) and screenshot it, GAP seconds apart.
# Usage: qf_trace.sh <work dir> [n=6] [gap=3]
W=${1:?work dir}; N=${2:-6}; GAP=${3:-3}; P="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$W"; : > "$W/pad.txt"; cd $P
GZ_MOVIES=0 GZ_QUICK_FIGHT=1 GZ_MOVIE_SKIP_FRAMES=20 GZ_QF_DELAY=60 GZ_SHOT_FILE=$W/shot GZ_GPU_TRACE_FILE=$W/trace.go RECOMP_PAD_LIVE=$W/pad.txt \
GZ_SAVE_DIR="${GZ_SAVE_DIR:-$LOCALAPPDATA\xboxrecomp_test}" RECOMP_USB=1 RECOMP_PB_INVENTORY=0 RECOMP_DISPATCH_GATE=${RECOMP_DISPATCH_GATE:-1} RECOMP_HEAP_RECLAIM=1 RECOMP_KERNEL_LOG_BUDGET=200 RECOMP_PB_EXEC=1 RECOMP_PB_WALK=dma RECOMP_AC97_READY=1 RECOMP_WATCHDOG_SECS=300 \
  ${GZ_EXE:-./build/Release/godzilla_ste_recomp.exe} > "$W/run.log" 2>&1 &
G=$!; t=0
until grep -q QUICKFIGHT "$W/run.log" || ! kill -0 $G 2>/dev/null || [ $t -ge 120 ]; do sleep 1; t=$((t+1)); done
sleep ${LOAD_SECS:-42}
echo "${PAD_LINE:-lup:60000}" >> "$W/pad.txt"; sleep ${WALK_SECS:-14}
k=0; while [ $k -lt $N ] && kill -0 $G 2>/dev/null; do
  touch "$W/shot"; t=0; until [ ! -f "$W/shot" ] || [ $t -ge 10 ]; do sleep 1; t=$((t+1)); done
  [ -f "$W/shot.bmp" ] && mv "$W/shot.bmp" "$W/shot_$k.bmp"
  touch "$W/trace.go"; sleep $GAP; k=$((k+1))
done
echo "alive: $(kill -0 $G 2>/dev/null && echo yes || echo no)"
kill $G 2>/dev/null; wait $G 2>/dev/null
