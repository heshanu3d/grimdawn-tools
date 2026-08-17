#!/usr/bin/env bash
R="/d/Game/grimdawn.Build.24346246/dpyes-ext"
echo "[watcher] waiting for Grim Dawn to exit (up to 15 min)..."
for i in $(seq 1 180); do
  if ! tasklist 2>/dev/null | grep -qi "grim dawn"; then break; fi
  sleep 5
done
if tasklist 2>/dev/null | grep -qi "grim dawn"; then echo "[watcher] TIMEOUT: game still running after 15min, abort."; exit 1; fi
echo "[watcher] game exited. swapping DLL -> phase-3 (UI build)."
mv "$R/dpyes_ext.dll" "$R/dpyes_ext_phase2.dll" 2>/dev/null
mv "$R/dpyes_ext_new.dll" "$R/dpyes_ext.dll"
rm -f "$R/dpyes_ext.log"
cd "/d/Game/grimdawn.Build.24346246/工具"
echo "[watcher] relaunching + injecting phase-3 DLL..."
python inject_gdassistant.py --dll "$R/dpyes_ext.dll" --launch-x64 --target-x64 2>&1 | tail -4
sleep 10
echo "[watcher] log:"; cat "$R/dpyes_ext.log" 2>/dev/null
