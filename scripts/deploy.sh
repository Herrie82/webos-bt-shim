#!/usr/bin/env bash
#
# deploy.sh -- install the gamepad/mouse shim onto a novacom-connected TouchPad.
#
# Strategy: rename /usr/bin/PmBtEngine -> PmBtEngine.real and drop a tiny wrapper
# in its place that sets LD_PRELOAD before exec'ing the real binary.  This works
# no matter who launches it (BluetoothMonitor respawn or dbus/ls2 activation) and
# is trivially reversible (scripts/undeploy.sh).
#
# Usage:
#   scripts/deploy.sh            # install, translator active
#   scripts/deploy.sh --dump     # also enable WEBOS_BT_SHIM_DUMP=1 (descriptor +
#                                #   raw report logging to /var/log/btshim.log)
#
set -euo pipefail

NOVACOM="${NOVACOM:-novacom}"
SO_LOCAL="${SO_LOCAL:-libpmbtgamepad.so}"
SO_REMOTE=/usr/lib/libpmbtgamepad.so
BIN=/usr/bin/PmBtEngine
LOG=/var/log/btshim.log
DUMP=0
[ "${1:-}" = "--dump" ] && DUMP=1

run() { $NOVACOM run file://"$1" ${2:+"$2"} ${3:+"$3"} ${4:+"$4"} ${5:+"$5"}; }

echo ">> device:"; $NOVACOM -l
echo ">> remount / read-write"
$NOVACOM run file://bin/mount "-o" "remount,rw" "/"

echo ">> push $SO_LOCAL -> $SO_REMOTE"
[ -f "$SO_LOCAL" ] || { echo "build first: make"; exit 1; }
$NOVACOM put file://"$SO_REMOTE" < "$SO_LOCAL"

echo ">> install wrapper (idempotent)"
# Only move the real binary aside once.
$NOVACOM run file://bin/sh "-c" \
  "[ -f ${BIN}.real ] || mv ${BIN} ${BIN}.real"

# Write the wrapper via a heredoc pushed over novacom put.
WRAP=$(mktemp)
cat > "$WRAP" <<EOF
#!/bin/sh
export LD_PRELOAD=$SO_REMOTE
export WEBOS_BT_SHIM_LOG=$LOG
$( [ "$DUMP" = 1 ] && echo 'export WEBOS_BT_SHIM_DUMP=1' )
exec ${BIN}.real "\$@"
EOF
$NOVACOM put file://"$BIN" < "$WRAP"
rm -f "$WRAP"
$NOVACOM run file://bin/chmod "755" "$BIN"

echo ">> restart Bluetooth (respawns PmBtEngine through the wrapper)"
$NOVACOM run file://usr/bin/killall "PmBtEngine" || true
$NOVACOM run file://usr/bin/killall "PmBtEngine.real" || true
# BluetoothMonitor / upstart respawns it automatically.

echo ">> done.  dump=$DUMP  log=$LOG"
echo "   watch:   scripts/capture.sh"
echo "   revert:  scripts/undeploy.sh"
