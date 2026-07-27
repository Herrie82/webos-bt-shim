#!/usr/bin/env bash
# undeploy.sh -- revert deploy.sh: restore the original PmBtEngine, drop the shim.
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
BIN=/usr/bin/PmBtEngine

echo ">> remount / read-write"
$NOVACOM run file://bin/mount "-o" "remount,rw" "/"

echo ">> restore original binary"
$NOVACOM run file://bin/sh "-c" \
  "[ -f ${BIN}.real ] && mv -f ${BIN}.real ${BIN} || echo 'nothing to restore'"

echo ">> remove shim"
$NOVACOM run file://bin/rm "-f" "/usr/lib/libpmbtgamepad.so"

echo ">> restart Bluetooth"
$NOVACOM run file://usr/bin/killall "PmBtEngine" || true

echo ">> reverted."
