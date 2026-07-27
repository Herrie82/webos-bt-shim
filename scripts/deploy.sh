#!/usr/bin/env bash
#
# deploy.sh -- install the gamepad/mouse shim onto a novacom-connected TouchPad.
#
# We inject LD_PRELOAD via the *upstart* job that launches BluetoothMonitor,
# which fork-execs PmBtEngine, so the env is inherited by PmBtEngine while its
# executable path stays /usr/bin/PmBtEngine.  This matters: ls-hubd binds the
# com.palm.bluetooth Luna service to that exact path via a role file, so we must
# NOT rename the binary (an earlier wrapper approach did, and PmBtEngine was then
# denied its service name -> "Messaging Init failed 0x8100300").
#
# novacom's getopt eats dashed argv, so on-device commands are piped to /bin/sh.
#
# Usage:
#   scripts/deploy.sh            # install, translator active
#   scripts/deploy.sh --dump     # also enable WEBOS_BT_SHIM_DUMP=1 logging
#
set -euo pipefail

NOVACOM="${NOVACOM:-novacom}"
SO_LOCAL="${SO_LOCAL:-libpmbtgamepad.so}"
SO_REMOTE=/usr/lib/libpmbtgamepad.so
JOB=/etc/event.d/bluetooth
BAK=/etc/event.d/bluetooth.btshim-orig
LOG=/var/log/btshim.log
MON=/usr/bin/BluetoothMonitor
DUMP=0
[ "${1:-}" = "--dump" ] && DUMP=1

dev_sh() { $NOVACOM run file://bin/sh; }

[ -f "$SO_LOCAL" ] || { echo "build first: make"; exit 1; }

echo ">> device:"; $NOVACOM -l
echo ">> remount / read-write"
echo 'mount -o remount,rw / && echo remounted' | dev_sh

echo ">> push $SO_LOCAL -> $SO_REMOTE"
$NOVACOM put file://"$SO_REMOTE" < "$SO_LOCAL"

echo ">> rewrite upstart job $JOB (backup -> $BAK), dump=$DUMP"
{
  echo 'set -e'
  echo "[ -f $BAK ] || cp $JOB $BAK"
  echo "cat > $JOB <<'JOBEOF'"
  echo 'description "Palm Bluetooth"'
  echo ''
  echo 'start on stopped finish'
  echo ''
  echo 'respawn'
  if [ "$DUMP" = 1 ]; then
    echo "exec /bin/sh -c 'export LD_PRELOAD=$SO_REMOTE; export WEBOS_BT_SHIM_LOG=$LOG; export WEBOS_BT_SHIM_DUMP=1; exec $MON'"
  else
    echo "exec /bin/sh -c 'export LD_PRELOAD=$SO_REMOTE; export WEBOS_BT_SHIM_LOG=$LOG; exec $MON'"
  fi
  echo 'JOBEOF'
  echo 'echo job-written'
} | dev_sh

echo ">> reload upstart + restart BluetoothMonitor"
# kill -HUP 1 makes init re-read /etc/event.d (it caches job defs); then a full
# initctl stop/start cycle respawns BluetoothMonitor with the new exec line.
# (A bare `killall` respawns from the *cached* old definition -- no env.)
{
  echo 'kill -HUP 1 2>/dev/null; sleep 3'
  echo 'initctl stop bluetooth 2>/dev/null; killall PmBtEngine 2>/dev/null; killall BluetoothMonitor 2>/dev/null; sleep 1'
  echo 'initctl start bluetooth 2>/dev/null; sleep 3; echo restarted'
} | dev_sh

echo ">> installed.  log=$LOG  dump=$DUMP"
echo "   Toggle Bluetooth OFF then ON in Settings, pair a controller, then:"
echo "   scripts/capture.sh          # tail the dump log"
echo "   scripts/undeploy.sh         # revert"
