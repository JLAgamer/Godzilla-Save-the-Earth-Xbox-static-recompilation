#!/usr/bin/env bash
# Quick fight: boot the game and drive the menus into a Versus fight
# (Godzilla 2000 vs COM Gigan, Tokyo), then hand the controller to the player.
# Scripted presses are OR'd with the real pad, so the player's controller works
# throughout. Frames are dumped (to check each screen changed) only while
# navigating; once the fight HUD shows, dumping stops and the script just waits
# for the game to exit. Started by play_fight.bat, which sets the environment.
P=$(cd "$(dirname "$0")/.." && pwd)
W="${TEMP:-/tmp}/gz_quickfight"
FDIFF=$P/recomp_config/fdiff.py
BARS=$P/recomp_config/bars.py
mkdir -p "$W"; rm -f "$W"/f*.bmp; : > "$W/pad.txt"; touch "$W/dump.on"
cd "$P"
RECOMP_PAD_LIVE="$W/pad.txt" RECOMP_FB_DUMP="$W/f" RECOMP_FB_DUMP_FLIPS="$W/dump.on" \
  ./build/Release/godzilla_ste_recomp.exe > play_fight.log 2>&1 &
GPID=$!
( while kill -0 $GPID 2>/dev/null; do
    ls "$W" | grep -E '^f[0-9]+\.bmp$' | sort | head -n -60 | sed "s|^|$W/|" | xargs -r rm -f
    sleep 3
  done ) &
PRUNE=$!
newest() { ls "$W" | grep -E '^f[0-9]+\.bmp$' | sort | tail -1; }
nf()     { local n=$(newest | tr -dc 0-9 | sed 's/^0*//'); echo ${n:-0}; }
alive()  { kill -0 $GPID 2>/dev/null; }
waitf()  { local n0=$(nf) t=0; until [ $(nf) -ge $((n0+$1)) ] || ! alive || [ $t -ge ${2:-60} ]; do sleep 1; t=$((t+1)); done; }
press()  { printf '%s\n' "$@" >> "$W/pad.txt"; }
confirm() {
  local k=0 before diff
  while [ $k -lt 3 ] && alive; do
    before=$(newest); [ -n "$before" ] && cp "$W/$before" "$W/_before.bmp"
    press "$1"; waitf 25 60
    diff=$(py -3 -I "$FDIFF" "$W/_before.bmp" "$W/$(newest)" 2>/dev/null)
    [ "${diff:-0}" -gt 12 ] && return 0
    k=$((k+1))
  done
}
echo "Quick fight: driving the menus (Godzilla 2000 vs Gigan, Tokyo)..."
t=0; until [ "$(grep -c MOVIE play_fight.log)" -ge 6 ] || ! alive || [ $t -ge 120 ]; do sleep 1; t=$((t+1)); done
sleep 6
press down:250; waitf 8 30
confirm a:400; waitf 20 30      # main menu -> Versus -> Profiles
confirm a:400; waitf 20 30      # profile Continue -> Versus select
confirm a:400                   # Godzilla 2000
confirm start:400               # add COM
confirm a:400                   # Gigan
confirm start:400; waitf 30 30  # -> City Select
confirm a:400                   # Tokyo -> Loading
hud() { py -3 -I "$BARS" "$W/$(newest)" 2>/dev/null | awk '{print ($4>100 && $7>100) ? 1 : 0}'; }
t=0; until [ "$(hud)" = "1" ] || ! alive || [ $t -ge 180 ]; do sleep 1; t=$((t+1)); done
rm -f "$W/dump.on"
alive && echo "Quick fight: in the fight - the controller is yours. Close the game window to quit."
{ kill $PRUNE; wait $PRUNE; } 2>/dev/null
rm -f "$W"/f*.bmp "$W/_before.bmp"
wait $GPID 2>/dev/null
echo "Exited. Log: $P/play_fight.log"
