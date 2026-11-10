#!/usr/bin/env bash
# Full pipeline: disasm (with seeds) -> func_id -> abi -> lift -> build
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"; X=${XBOXRECOMP_DIR:-$P/third_party/xboxrecomp}; L=$P/build/logs; mkdir -p "$L"
CM="/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
cd $X
py -3 -m tools.disasm game_files/default.xbe --seed-functions "$P/recomp_config/seed_functions.json" > $L/regen_disasm.log 2>&1
py -3 -m tools.func_id game_files/default.xbe > $L/regen_funcid.log 2>&1
py -3 -m tools.abi_analysis game_files/default.xbe > $L/regen_abi.log 2>&1
py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir "$P/src/recomp/gen" --exclude-manual "$P/src/recomp_manual.c" > $L/regen_lift.log 2>&1
cd $P
"$CM" --build build --config Release --parallel > $L/regen_build.log 2>&1
echo "regen ok"
