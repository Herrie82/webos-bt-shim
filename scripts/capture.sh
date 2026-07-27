#!/usr/bin/env bash
# capture.sh -- stream the shim log off the device (descriptor + report dumps).
# Pair a gamepad/mouse while this runs to capture its HID report descriptor and
# raw reports, which validates the RE offsets and the exact report layout.
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
LOG=/var/log/btshim.log
echo ">> tailing $LOG (Ctrl-C to stop).  Pair/press a device now."
$NOVACOM run file://usr/bin/tail "-n" "+1" "-f" "$LOG"
