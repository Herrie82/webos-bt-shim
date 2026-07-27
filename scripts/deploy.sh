#!/usr/bin/env bash
#
# deploy.sh -- install the gamepad/mouse shim onto a novacom-connected TouchPad.
#
# Strategy: rename /usr/bin/PmBtEngine -> PmBtEngine.real and drop a tiny wrapper
# in its place that sets LD_PRELOAD before exec'ing the real binary.  Works no
# matter who launches it (BluetoothMonitor respawn or dbus/ls2 activation) and is
# trivially reversible (scripts/undeploy.sh).
#
# NOTE: `novacom run` mangles dashed argv (its own getopt eats -x flags), so all
# on-device commands are piped to /bin/sh over stdin instead.
#
# Usage:
#   scripts/deploy.sh            # install, translator active
#   scripts/deploy.sh --dump     # also enable WEBOS_BT_SHIM_DUMP=1 logging
#
set -euo pipefail

NOVACOM="${NOVACOM:-novacom}"
SO_LOCAL="${SO_LOCAL:-libpmbtgamepad.so}"
SO_REMOTE=/usr/lib/libpmbtgamepad.so
BIN=/usr/bin/PmBtEngine
LOG=/var/log/btshim.log
DUMP=0
[ "${1:-}" = "--dump" ] && DUMP=1

dev_sh() { $NOVACOM run file://bin/sh; }   # reads script from stdin

[ -f "$SO_LOCAL" ] || { echo "build first: make"; exit 1; }

echo ">> device:"; $NOVACOM -l

echo ">> remount / read-write"
echo 'mount -o remount,rw / && echo remounted' | dev_sh

echo ">> push $SO_LOCAL -> $SO_REMOTE"
$NOVACOM put file://"$SO_REMOTE" < "$SO_LOCAL"

echo ">> install wrapper (idempotent) dump=$DUMP"
{
  echo 'set -e'
  echo "[ -f ${BIN}.real ] || mv ${BIN} ${BIN}.real"
  echo "cat > ${BIN} <<'WRAP'"
  echo '#!/bin/sh'
  echo "export LD_PRELOAD=$SO_REMOTE"
  echo "export WEBOS_BT_SHIM_LOG=$LOG"
  [ "$DUMP" = 1 ] && echo 'export WEBOS_BT_SHIM_DUMP=1'
  echo "exec ${BIN}.real \"\$@\""
  echo 'WRAP'
  echo "chmod 755 ${BIN}"
  echo "echo wrapper-installed"
} | dev_sh

echo ">> restart Bluetooth (BluetoothMonitor respawns PmBtEngine via the wrapper)"
echo 'killall PmBtEngine 2>/dev/null; killall PmBtEngine.real 2>/dev/null; echo done' | dev_sh

echo ">> installed.  log=$LOG  dump=$DUMP"
echo "   Enable Bluetooth + pair a controller, then:  scripts/capture.sh"
echo "   Revert with:  scripts/undeploy.sh"
