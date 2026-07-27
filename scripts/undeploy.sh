#!/usr/bin/env bash
# undeploy.sh -- revert deploy.sh: restore the stock upstart job, drop the shim.
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
JOB=/etc/event.d/bluetooth
BAK=/etc/event.d/bluetooth.btshim-orig
dev_sh() { $NOVACOM run file://bin/sh; }

{
  echo 'mount -o remount,rw / >/dev/null 2>&1'
  echo "if [ -f $BAK ]; then mv -f $BAK $JOB && echo job-restored; else echo 'no backup'; fi"
  # Legacy wrapper cleanup (in case an old deploy renamed the binary).
  echo '[ -f /usr/bin/PmBtEngine.real ] && mv -f /usr/bin/PmBtEngine.real /usr/bin/PmBtEngine && echo bin-restored || true'
  echo 'rm -f /usr/lib/libpmbtgamepad.so && echo shim-removed'
  echo 'kill -HUP 1 2>/dev/null; killall PmBtEngine 2>/dev/null; killall BluetoothMonitor 2>/dev/null; echo bt-restarted'
} | dev_sh
echo ">> reverted."
