#!/usr/bin/env bash
# unpatch-bt-app.sh -- restore the stock Bluetooth settings app.
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
D=/usr/palm/applications/com.palm.app.bluetoothtab/app/controllers
dev_sh() { $NOVACOM run file://bin/sh; }
{
  echo 'mount -o remount,rw / >/dev/null 2>&1'
  echo "cd $D"
  echo '[ -f DeviceClass.js.btshim-orig ] && mv -f DeviceClass.js.btshim-orig DeviceClass.js && echo A-restored || echo "A: no backup"'
  echo '[ -f bluetooth-assistant.js.btshim-orig ] && mv -f bluetooth-assistant.js.btshim-orig bluetooth-assistant.js && echo B-restored || echo "B: no backup"'
} | dev_sh
echo ">> restored.  Close and reopen the Bluetooth app."
