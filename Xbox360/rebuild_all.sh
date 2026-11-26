#!/bin/bash
# Full rebuild: names -> hooks -> generated code -> objects -> runtime -> XEX -> Tests/run.
set -e
cd "$(dirname "$0")/.."
C=Reference/LuigisMansionPC/decomp/config
python Tools/recomp/make_hooks.py Tools/recomp/GLMP01_names.txt Tools/recomp/hooks.txt
python Tools/recomp/ppc_recomp.py --dol GameData/GLMP01_rev0/sys/main.dol --symbols $C/GLMP01_00/symbols.txt \
    --names Tools/recomp/GLMP01_names.txt --hooks Tools/recomp/hooks.txt --localize Tools/recomp/localize.txt --localize-all --out Builds/gen
[ "$1" = "so-runtime" ] || powershell -NoProfile -ExecutionPolicy Bypass -File Xbox360/build_gen.ps1 | tail -3
powershell -NoProfile -ExecutionPolicy Bypass -File Xbox360/build_runtime.ps1 | tail -15
powershell -NoProfile -ExecutionPolicy Bypass -File Xbox360/build_gx.ps1 | tail -5
powershell -NoProfile -ExecutionPolicy Bypass -File Xbox360/build_xex.ps1 | tail -3
mkdir -p Tests/run && cp Builds/xex/default.xex Tests/run/
