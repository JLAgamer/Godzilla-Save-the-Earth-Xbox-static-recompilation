#!/usr/bin/env bash
# Boot the recomp and drive the menus into a Versus fight (Godzilla 2000 vs COM Gigan),
# then send fight inputs. Needs an existing profile (created once by hand: "AN").
# Usage: to_fight.sh <work dir> <sheet.py> <bars.py> <fdiff.py>
W=$1; SHEET=$2; BARS=$3; FDIFF=$4; P="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p $W; rm -f $W/f*.bmp $W/step_*.png; : > $W/pad.txt
# Frames are dumped (and read back from the GPU) only while dump.on exists:
# the fps window below removes it so the measurement is not slowed by dumping.
touch $W/dump.on
cd $P
GZ_MOVIES=0 GZ_GPU_TRACE_FILE=$W/trace.req GZ_RAMDUMP=$W/ram RECOMP_USB=1 RECOMP_PAD_LIVE=$W/pad.txt RECOMP_KERNEL_LOG_BUDGET=200 RECOMP_PB_EXEC=1 RECOMP_PB_WALK=dma \
RECOMP_AC97_READY=1 RECOMP_FB_DUMP=$W/f RECOMP_FB_DUMP_FLIPS=$W/dump.on RECOMP_WATCHDOG_SECS=900 \
  ./build/Release/godzilla_ste_recomp.exe > $W/run.log 2>&1 &
GPID=$!
# Frames are ~900 KB each: keep only the newest 300 while the game runs.
( while kill -0 $GPID 2>/dev/null; do
    ls $W | grep -E '^f[0-9]+\.bmp$' | sort | head -n -300 | sed "s|^|$W/|" | xargs -r rm -f
    sleep 5
  done ) &
PRUNE=$!
# Frame count = newest frame's number (pruning removes old files).
nf()     { local n=$(ls $W | grep -E '^f[0-9]+\.bmp$' | sort | tail -1 | tr -dc 0-9 | sed 's/^0*//'); echo ${n:-0}; }
newest() { ls $W | grep -E '^f[0-9]+\.bmp$' | sort | tail -1; }
alive()  { kill -0 $GPID 2>/dev/null && ! grep -q CRASH $W/run.log; }
waitf()  { local n0=$(nf) t=0; until [ $(nf) -ge $((n0+$1)) ] || ! alive || [ $t -ge ${2:-240} ]; do sleep 1; t=$((t+1)); done; }
snap()   { local last=$(newest); [ -n "$last" ] && py -3 -I $SHEET $W/step_$1.png $W/$last >/dev/null; echo "step $1 frame $last $(grep fps $W/run.log | tail -1)"; }
press()  { printf '%s\n' "$@" >> $W/pad.txt; }
# Press, then wait for the screen to change (mean diff > 12); retry up to 3 times.
confirm() {
  local k=0 before diff
  while [ $k -lt 3 ] && alive; do
    before=$(newest); cp $W/$before $W/_before.bmp
    press "$1"; waitf 25 90
    diff=$(py -3 -I $FDIFF $W/_before.bmp $W/$(newest))
    echo "  confirm $1 try $k diff $diff"
    [ "${diff:-0}" -gt 12 ] && return 0
    k=$((k+1))
  done
}
t=0; until [ "$(grep -c MOVIE $W/run.log)" -ge 6 ] || ! alive || [ $t -ge 300 ]; do sleep 1; t=$((t+1)); done
sleep 8; snap 01_menu
press down:250; waitf 8 60
confirm a:400;   waitf 20; snap 02_profiles     # main menu -> Versus -> Profiles
confirm a:400;   waitf 20; snap 03_versus       # profile Continue -> Versus select
# P1_RIGHT=n: move the P1 cursor right n times first (a different monster).
k=0; while [ $k -lt ${P1_RIGHT:-0} ]; do press right:250; sleep 1; k=$((k+1)); done
confirm a:400;   snap 04_p1                     # pick Godzilla 2000
confirm start:400; snap 05_com                  # add COM
confirm a:400;   snap 06_com_pick               # pick Gigan
confirm start:400; waitf 30; snap 07_city       # play -> City Select
touch $W/ram.req; waitf 16 30                   # GZ_RAMDUMP snapshot 0: City Select
confirm a:400;   snap 07b_loading               # pick city -> Loading
hud() { py -3 -I $BARS $W/$(newest) 2>/dev/null | awk '{print ($4>100 && $7>100) ? 1 : 0}'; }
t=0; until [ "$(hud)" = "1" ] || ! alive || [ $t -ge 900 ]; do sleep 2; t=$((t+2)); done
waitf 60 120; snap 08_fight
touch $W/ram.req; waitf 16 30                   # GZ_RAMDUMP snapshot 1: in the fight
echo "bars before: $(py -3 -I $BARS $W/$(newest))"
# SKY_TRACE=<skyblack.py>: fight until a frame's sky band is >= 15% black, then
# trace the next frame (catches the GPU-vertex "black boxes in the sky").
if [ -n "$SKY_TRACE" ]; then
  t=0; hit=""
  while [ $t -lt 150 ] && alive; do
    f=$(newest); p=$(py -3 -I $SKY_TRACE $W/$f 2>/dev/null)
    if [ "${p:-0}" -ge 15 ]; then
      hit=$f; cp $W/$f $W/sky_before.bmp; touch $W/trace.req; waitf 6 30
      cp $W/$(newest) $W/sky_after.bmp
      echo "sky trace at $f (${p}% black): $(grep -o "frame [0-9]* requested" $W/run.log | tail -1)"
      break
    fi
    press lup:400 rright:400 x:200; sleep 1; t=$((t+1))
  done
  [ -z "$hit" ] && echo "sky trace: no black sky seen in 150 s"
fi
# FPS_SECS (default 20): fight with frame dumps paused and report the GPU fps
# lines logged meanwhile (reports land every couple of seconds).
n0=$(grep -c "\[GPU\] [0-9.]* fps" $W/run.log); rm -f $W/dump.on
t=0; while [ $t -lt ${FPS_SECS:-20} ] && alive; do press lup:600 a:300 x:300 rright:600 b:300; sleep 3; t=$((t+3)); done
echo "fight fps (no dumps): $(grep "\[GPU\] [0-9.]* fps" $W/run.log | tail -n +$((n0+1)) | awk '{print $2}' | paste -sd' ')"
touch $W/dump.on; waitf 10 60
[ -z "$TRACE_LATE" ] && { touch $W/trace.req; waitf 20 60; }   # GPU back end traces one fight frame
press lup:3000 a:300 a:300 x:300 a:300 x:300 a:300 x:300 a:300; waitf 120 300; snap 09_after_inputs
# TRACE_LATE=1: trace mid-fight instead (camera on both monsters).
[ -n "$TRACE_LATE" ] && { echo "trace touched at $(newest)"; cp $W/$(newest) $W/trace_before.bmp; touch $W/trace.req; waitf 30 90; cp $W/$(newest) $W/trace_after.bmp; T=$(grep -o "frame [0-9]* requested" $W/run.log | tail -1); echo "trace $T"; }
# GZ_PROFILE: keep fighting until the sampler has written its file.
if [ -n "$GZ_PROFILE" ]; then t=0; until [ -f "$GZ_PROFILE" ] || ! alive || [ $t -ge 90 ]; do press a:300 x:300; sleep 3; t=$((t+3)); done; echo "profile: $([ -f "$GZ_PROFILE" ] && echo written || echo missing)"; fi
echo "bars after:  $(py -3 -I $BARS $W/$(newest))"
confirm start:400; snap 10_pause
echo "alive: $(alive && echo yes || echo no)"; grep -E "CRASH|Failed to resolve|not reached" $W/run.log | sort | uniq -c | head -5
kill $GPID 2>/dev/null; wait $GPID 2>/dev/null
{ kill $PRUNE; wait $PRUNE; } 2>/dev/null
ls $W | grep -E '^f[0-9]+\.bmp$' | sort | head -n -300 | sed "s|^|$W/|" | xargs -r rm -f
