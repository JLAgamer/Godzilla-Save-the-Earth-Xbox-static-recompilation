#!/usr/bin/env bash
# One quick fight per stage at the given frame rate: a screenshot, the fps
# lines and the pacing summary (frames / late frames / longest frame per
# second, GZ_PACE_LOG) for each.
# Usage: stage_sweep.sh <work dir> [rate=240] [stage ...]
W=${1:?work dir}; RATE=${2:-240}; shift; shift
P="$(cd "$(dirname "$0")/.." && pwd)"
STAGES=("$@")
[ ${#STAGES[@]} -eq 0 ] && STAGES=(New_Tokyo_Main_DAY New_Seattle_Main_DAY New_SF_Main_DAY New_Osaka_Main_DAY \
  New_London_Main_DAY New_LA_Main_DAY New_Boston_Main_DAY NewYork_DAY MonsterIsland_DAY Boxing_Ring_PS2_DAY New_Mothership)
mkdir -p "$W"
for st in "${STAGES[@]}"; do
  D="$W/$st"; mkdir -p "$D"
  GZ_QF_CITY=$st GZ_RATE=$RATE GZ_WIDE=1 GZ_PACE_LOG=1 RECOMP_HEAP_RECLAIM=1 GZ_SETTINGS_FILE="$D/s.ini" \
    bash "$P/recomp_config/qf_shot.sh" "$D" 2 ${SECS:-30} > "$D/out.txt" 2>&1
  for k in 0 1; do [ -f "$D/shot_$k.bmp" ] && py -3 "$P/recomp_config/tools/bmp2png.py" "$D/shot_$k.bmp" "$D/shot_$k.png" > /dev/null 2>&1 && rm -f "$D/shot_$k.bmp"; done
  # the 11 one-second pacing lines between the two screenshots (a screenshot stalls a frame)
  grep '\[PACE\]' "$D/run.log" | tail -14 | head -11 | awk -v s="$st" '{f+=$2; l+=$6; if ($9+0>m) m=$9+0; n++; if ($2+0<mn || n==1) mn=$2+0}
    END{ if (n) printf "%-22s avg %.0f fps, worst second %d, late frames %.1f%%, longest frame %.1f ms\n", s, f/n, mn, 100*l/f, m; else printf "%-22s NO DATA\n", s }'
  grep -E "alive|CRASH|exhaust|not reached" "$D/out.txt" "$D/run.log" | head -3 | cut -c1-160
done
