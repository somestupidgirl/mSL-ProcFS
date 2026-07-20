#!/bin/bash
#
# Uninstall ProcFS.command
#
# Double-clickable Terminal front-end to uninstall.sh: confirms, escalates to
# root, then hands over to the script that does the actual removal. Ships
# alongside uninstall.sh in the .dmg, so it needs no build tree.
#
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/uninstall.sh"

if [ ! -f "$SCRIPT" ]; then
    echo "Cannot find uninstall.sh next to this file - copy both out of the disk"
    echo "image, or run 'sudo make uninstall' from a source tree."
    echo "Press Return to close."
    read -r _
    exit 1
fi

printf 'This will remove ProcFS from this Mac. Continue? [y/N] '
read -r reply
case "$reply" in
    y|Y|yes|YES) ;;
    *) echo "Cancelled."; echo "Press Return to close."; read -r _; exit 0 ;;
esac

# A .command opens in Terminal as the user; sudo prompts for the password here.
sudo /bin/bash "$SCRIPT"
status=$?

echo ""
echo "Press Return to close."
read -r _
exit $status
