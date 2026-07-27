#!/usr/bin/env bash
#
# deploy.sh -- install/update the shim on a novacom-connected TouchPad.
#
# Injection: LD_PRELOAD is set in BluetoothMonitor's environment via the upstart
# job /etc/event.d/bluetooth (it fork-execs PmBtEngine, so the env is inherited
# while the exe path stays /usr/bin/PmBtEngine -- required for its ls-hubd role).
#
# IMPORTANT lesson learned: this webOS upstart (0.3.x) does NOT reliably reload a
# changed /etc/event.d job at runtime (kill -HUP 1 / initctl restart keep the
# stale in-memory definition, so BluetoothMonitor respawns WITHOUT the env).
# Only a reboot re-parses the job. Therefore:
#   * Updating just the .so  -> push it + kill PmBtEngine (BluetoothMonitor, which
#     already carries the env from boot, respawns PmBtEngine and loads the new .so).
#     NO reboot, NO monitor restart.
#   * Changing the upstart job (first install / env change) -> REBOOT to apply.
#
# Usage:
#   scripts/deploy.sh            # update .so (+ ensure job present); reload PmBtEngine
#   scripts/deploy.sh --setup    # (re)write the upstart job too -> then REBOOT
#
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
SO_LOCAL="${SO_LOCAL:-libpmbtgamepad.so}"
SO_REMOTE=/usr/lib/libpmbtgamepad.so
JOB=/etc/event.d/bluetooth
BAK=/etc/event.d/bluetooth.btshim-orig
LOG=/var/log/btshim.log
MON=/usr/bin/BluetoothMonitor
SETUP=0
[ "${1:-}" = "--setup" ] && SETUP=1
dev_sh() { $NOVACOM run file://bin/sh; }

[ -f "$SO_LOCAL" ] || { echo "build first: make"; exit 1; }

echo ">> remount / rw + push $SO_LOCAL"
echo 'mount -o remount,rw / >/dev/null 2>&1 && echo remounted' | dev_sh
$NOVACOM put file://"$SO_REMOTE" < "$SO_LOCAL"

if [ "$SETUP" = 1 ]; then
  echo ">> (re)writing upstart job (backup -> $BAK)"
  {
    echo "[ -f $BAK ] || cp $JOB $BAK"
    echo "cat > $JOB <<'JOBEOF'"
    echo 'description "Palm Bluetooth"'; echo
    echo 'start on stopped finish'; echo
    echo 'respawn'
    echo "exec /bin/sh -c 'export LD_PRELOAD=$SO_REMOTE; export WEBOS_BT_SHIM_LOG=$LOG; export WEBOS_BT_SHIM_DUMP=1; exec $MON'"
    echo 'JOBEOF'
    echo 'echo job-written'
  } | dev_sh
  echo ">> job written.  REBOOT the device now to apply it:"
  echo "     $NOVACOM run file://sbin/reboot"
  exit 0
fi

echo ">> reloading: kill PmBtEngine so BluetoothMonitor respawns it with the new .so"
echo 'rm -f '"$LOG"'; killall PmBtEngine 2>/dev/null; sleep 3; echo done' | dev_sh
echo ">> checking the shim actually mapped in..."
echo 'PE=$(pidof PmBtEngine); if grep -q libpmbtgamepad /proc/$PE/maps 2>/dev/null || grep -q loaded '"$LOG"' 2>/dev/null; then echo "OK: shim active"; else echo "NOT LOADED -- BluetoothMonitor lacks the env; run: scripts/deploy.sh --setup  then REBOOT"; fi' | dev_sh
