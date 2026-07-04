#!/bin/bash
#
# Uninstall ProcFS.command
#
# Double-clickable uninstaller for ProcFS: unmounts /proc, unloads the kext,
# stops the daemon, and removes every installed file. Mirrors the source tree's
# `sudo make uninstall`, but stands alone so it can be run from the .dmg on any
# machine (no build tree required).
#
set -u

BUNDLE_ID=com.beako.filesystems.procfs
DAEMON_LABEL=com.beako.procfsd
DAEMON_PLIST=/Library/LaunchDaemons/com.beako.procfsd.plist
SYNTHETIC=/etc/synthetic.conf

# A .command opens in Terminal as the user; re-run as root (sudo prompts here).
if [ "$(id -u)" -ne 0 ]; then
    echo "The ProcFS uninstaller needs administrator privileges."
    exec sudo /bin/bash "$0" "$@"
fi

printf 'This will remove ProcFS from this Mac. Continue? [y/N] '
read -r reply
case "$reply" in
    y|Y|yes|YES) ;;
    *) echo "Cancelled."; echo "Press Return to close."; read -r _; exit 0 ;;
esac

echo "==> Uninstalling ProcFS"

echo "  - unmounting /proc"
mount | awk '/\(procfs[ ,]/ { print $3 }' | while read -r mp; do
    umount "$mp" 2>/dev/null || diskutil unmount force "$mp" 2>/dev/null || true
done

echo "  - stopping and removing the LaunchDaemon"
launchctl bootout "system/$DAEMON_LABEL" 2>/dev/null || true
launchctl disable "system/$DAEMON_LABEL" 2>/dev/null || true
rm -f "$DAEMON_PLIST"

echo "  - unloading the kext and clearing the staging cache"
kmutil unload -b "$BUNDLE_ID" 2>/dev/null || true
kmutil clear-staging 2>/dev/null || true

echo "  - removing installed files"
rm -rf /Library/Extensions/procfs.kext
rm -rf /Library/Filesystems/procfs.fs
rm -rf /Applications/ProcFS.app
rm -f  /usr/local/sbin/procfsd /usr/local/sbin/procfs_ksyms
rm -f  /var/db/procfs.enabled /var/db/procfs.ksyms /var/db/procfs.linux /var/db/procfs.linux_version

echo "  - removing 'proc' from $SYNTHETIC"
if [ -f "$SYNTHETIC" ]; then
    if grep -vxF 'proc' "$SYNTHETIC" > "$SYNTHETIC.tmp" 2>/dev/null; then
        mv "$SYNTHETIC.tmp" "$SYNTHETIC"
    else
        rm -f "$SYNTHETIC.tmp"
    fi
    [ -s "$SYNTHETIC" ] || rm -f "$SYNTHETIC"     # drop it if now empty
fi

echo ""
echo "ProcFS has been uninstalled. The now-empty /proc mount point persists"
echo "until the next reboot."
echo ""
echo "Press Return to close."
read -r _
exit 0
