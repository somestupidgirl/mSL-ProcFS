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
Double-click "Uninstall ProcFS.command" in this disk image. It opens a Terminal
window, asks for your administrator password, then unmounts /proc, unloads the
kext, stops the daemon and removes every installed file. (If macOS blocks it as
being from an unidentified developer, right-click it and choose "Open".) The
now-empty /proc mount point disappears after the next reboot.

The source tree's `sudo make uninstall` does the same thing.
