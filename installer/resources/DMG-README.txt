ProcFS for macOS
================

A /proc filesystem for macOS: a kernel extension that exposes running processes
and threads as files, with BSD- and Linux-compatible per-process information.

INSTALL
-------
1. Double-click the ProcFS installer package (.pkg) and follow the prompts.
   (The package is not signed with an Apple Developer ID; if macOS blocks it,
   right-click the .pkg and choose "Open", or allow it in
   System Settings -> Privacy & Security.)

2. Approve the kernel extension:
   System Settings -> Privacy & Security -> Allow the ProcFS extension.
   You may need to reboot once for the prompt to appear.

3. Restart your Mac. On the next boot /proc is created, the extension loads,
   and the procfsd daemon mounts /proc for all users.

USE
---
   ls /proc
   cat /proc/self/status

The ProcFS menu-bar app (in /Applications) shows mount/daemon/Linux-mode status
and offers one-click mount/unmount, Linux-compatibility, and daemon controls.

UNINSTALL
---------
Auto-load/auto-mount is armed via /var/db/procfs.enabled. To remove ProcFS,
delete the installed files (/Library/Extensions/procfs.kext,
/Library/Filesystems/procfs.fs, /usr/local/sbin/procfsd,
/usr/local/sbin/procfs_ksyms, /Library/LaunchDaemons/com.beako.procfsd.plist,
/Applications/ProcFS.app), remove /var/db/procfs.enabled and the "proc" line
from /etc/synthetic.conf, and reboot. (The source tree's `sudo make uninstall`
does all of this.)
