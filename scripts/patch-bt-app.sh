#!/usr/bin/env bash
#
# patch-bt-app.sh -- patch the stock webOS Bluetooth settings app
# (com.palm.app.bluetoothtab) so Bluetooth MICE and GAMEPADS can be discovered
# and paired through the HID path.
#
# Stock behaviour: only a *keyboard* Class-of-Device passes the "keyboard"
# pairing category and gets DEVICETYPE='Keyboard' (the only type that reaches
# the HID connect).  A mouse/gamepad is filtered out, so it never pairs as HID
# and PmBtEngine never opens an HID channel for it -> our shim never sees it.
#
# Two one-line edits make mice + gamepads take the exact same HID path as
# keyboards.  The app already ships isMouse()/isGamepad() helpers, so we just
# call them.  Backups: *.btshim-orig next to each file.  Idempotent.
#
set -euo pipefail
NOVACOM="${NOVACOM:-novacom}"
D=/usr/palm/applications/com.palm.app.bluetoothtab/app/controllers
dev_sh() { $NOVACOM run file://bin/sh; }

{
  echo 'set -e'
  echo 'mount -o remount,rw / >/dev/null 2>&1'
  echo "cd $D"
  # backups (once)
  echo '[ -f DeviceClass.js.btshim-orig ] || cp DeviceClass.js DeviceClass.js.btshim-orig'
  echo '[ -f bluetooth-assistant.js.btshim-orig ] || cp bluetooth-assistant.js bluetooth-assistant.js.btshim-orig'
  # Edit A: DeviceClass.js -- the "keyboard" discovery category also matches mouse/gamepad.
  echo "sed -i 's/            if ( isKeyboard(cod) )/            if ( isKeyboard(cod) || isMouse(cod) || isGamepad(cod) )/' DeviceClass.js"
  # Edit B: bluetooth-assistant.js -- a paired mouse/gamepad is typed 'Keyboard' (=> HID connect).
  echo "sed -i 's/else if (isKeyboard(payload.cod))/else if (isKeyboard(payload.cod) || isMouse(payload.cod) || isGamepad(payload.cod))/' bluetooth-assistant.js"
  echo 'echo "--- verify ---"'
  echo 'grep -n "isKeyboard(cod) || isMouse" DeviceClass.js || echo "EDIT A NOT APPLIED"'
  echo 'grep -n "isKeyboard(payload.cod) || isMouse" bluetooth-assistant.js || echo "EDIT B NOT APPLIED"'
} | dev_sh

echo ">> patched.  Fully close the Bluetooth app (swipe the card away) and reopen it,"
echo "   choose the 'Keyboard' category, put the mouse in pairing mode, and pair."
