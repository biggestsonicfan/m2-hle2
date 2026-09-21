#!/bin/bash
# ES Tools > "Update m2hle": install-es.sh copies this to
# "/storage/.config/modules/Update m2hle.sh" (the file name is the menu entry).
# Installs the canary's m2hle and its RetroArch core, whichever are on the
# device, if the canary is newer.

. /etc/profile
clear
bash /storage/.local/bin/m2hle-update.sh --tool
