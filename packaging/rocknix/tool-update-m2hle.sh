#!/bin/bash
# ES Tools > "Update m2hle": install-es.sh copies this to
# "/storage/.config/modules/Update m2hle.sh" (the file name is the menu entry).
# Installs the canary's m2hle if it is newer than the one on the device.

. /etc/profile
clear
bash /storage/.local/bin/m2hle-update.sh --tool
