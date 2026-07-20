#!/bin/bash
#
# uninstall.sh
#
# Removes every trace of ProcFS from the system: unmounts /proc, stops the
# daemon, unloads the kext and deletes the installed files. Must already be
# running as root, and takes no input - the confirmation and the privilege
# escalation belong to whoever calls it.
#
# This is the single source of truth for what an uninstall removes. Both
# front-ends use it: installer/uninstall.command (Terminal) and the
# Uninstall-ProcFS app (bundled as a resource). Keep it in step with the
# `uninstall` target in the top-level Makefile.
#
set -u

BUNDLE_ID=com.beako.filesystems.procfs
DAEMON_LABEL=com.beako.procfsd
DAEMON_PLIST=/Library/LaunchDaemons/com.beako.procfsd.plist
SYNTHETIC=/etc/synthetic.conf
MSL_DIR=/Applications/mSL

if [ "$(id -u)" -ne 0 ]; then
    echo "uninstall.sh must be run as root." >&2
    exit 1
fi

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
rm -rf /Library/PreferencePanes/ProcFS.prefPane
rm -f  /usr/local/sbin/procfsd /usr/local/sbin/procfs_ksyms
rm -f  /var/db/procfs.enabled /var/db/procfs.ksyms \
       /var/db/procfs.linux /var/db/procfs.linux_version

# The apps live under /Applications/mSL. The uninstaller is one of them, so it
# deletes itself here - callers run a copy of this script from outside the
# bundle, otherwise bash would lose the rest of the file mid-read.
rm -rf "$MSL_DIR/ProcFS.app"
rm -rf "$MSL_DIR/Uninstall-ProcFS.app"
# Take the mSL folder with it, but only when no other module is left in it.
rmdir "$MSL_DIR" 2>/dev/null || true

echo "  - removing 'proc' from $SYNTHETIC"
if [ -f "$SYNTHETIC" ]; then
    if grep -vxF 'proc' "$SYNTHETIC" > "$SYNTHETIC.tmp" 2>/dev/null; then
        mv "$SYNTHETIC.tmp" "$SYNTHETIC"
    else
        rm -f "$SYNTHETIC.tmp"
    fi
    [ -s "$SYNTHETIC" ] || rm -f "$SYNTHETIC"     # drop it if now empty
fi

echo "ProcFS has been uninstalled. The now-empty /proc mount point persists"
echo "until the next reboot."
exit 0
