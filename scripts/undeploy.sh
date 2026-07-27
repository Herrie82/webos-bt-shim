#!/usr/bin/env bash
# undeploy.sh -- revert deploy.sh: restore the original PmBtEngine, drop the shim.
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
BIN=/usr/bin/PmBtEngine
dev_sh() { $NOVACOM run file://bin/sh; }

{
  echo 'mount -o remount,rw / >/dev/null 2>&1'
  echo "[ -f ${BIN}.real ] && mv -f ${BIN}.real ${BIN} && echo restored || echo 'nothing to restore'"
  echo 'rm -f /usr/lib/libpmbtgamepad.so && echo shim-removed'
  echo 'killall PmBtEngine 2>/dev/null; echo bt-restarted'
} | dev_sh
echo ">> reverted."
