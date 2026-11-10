#!/usr/bin/env bash
# Quick fight for SECS seconds of fighting (no screenshots), print the frame
# rate lines, optionally sample wait stacks. Extra environment from the caller.
# Usage: qf_fps.sh <work dir> [secs=25] [stackwait samples=0]
W=${1:?work dir}; SECS=${2:-25}; SW=${3:-0}; P="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$W"; : > "$W/pad.txt"; cd $P
GZ_MOVIES=0 GZ_QUICK_FIGHT=1 GZ_MOVIE_SKIP_FRAMES=20 GZ_QF_DELAY=60 GZ_SETTINGS_FILE=$W/s.ini RECOMP_PAD_LIVE=$W/pad.txt \
RECOMP_USB=1 RECOMP_KERNEL_LOG_BUDGET=200 RECOMP_PB_EXEC=1 RECOMP_PB_WALK=dma RECOMP_AC97_READY=1 RECOMP_WATCHDOG_SECS=300 \
  ./build/Release/godzilla_ste_recomp.exe > "$W/run.log" 2>&1 &
t=0; until grep -q QUICKFIGHT "$W/run.log" || [ $t -ge 120 ]; do sleep 1; t=$((t+1)); done
sleep ${LOAD_SECS:-42}
printf '%s\n' lup:1500 a:300 x:300 >> "$W/pad.txt"; sleep 5
N0=$(grep -c '\] [0-9.]* fps' "$W/run.log")
PID=$(tasklist | grep -i godzilla_ste | awk '{print $2}' | head -1)
if [ "$SW" -gt 0 ]; then py -3 recomp_config/tools/stackwait.py $PID build/Release/godzilla_ste_recomp.map $SW ${SWDEPTH:-3} 2>&1 | cut -c1-400 > "$W/stackwait.txt"; fi
sleep $SECS
grep '\] [0-9.]* fps' "$W/run.log" | tail -n +$((N0+1)) | awk '{printf "%s ", $2} END{print ""}'
taskkill //PID $PID //F > /dev/null 2>&1; sleep 1
