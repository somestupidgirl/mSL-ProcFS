//
// Copyright (c) 2026 Sunneva N. Mariu
//
// ProcFSMenu.swift
//
// A menu-bar (status-bar) app for procfs. Its menu shows live status - whether
// /proc is mounted, whether the procfsd daemon is running, and whether Linux
// presentation mode is on - and offers one-click toggles for each. The mutating
// actions need root, so they run through a single macOS administrator-auth
// prompt (NSAppleScript "with administrator privileges"); status is read
// unprivileged.
//
import Cocoa
import ServiceManagement

private let kMountPoint  = "/proc"
private let kDaemonLabel = "com.beako.procfsd"
private let kDaemonPlist = "/Library/LaunchDaemons/com.beako.procfsd.plist"
private let kLinuxConf   = "/var/db/procfs.linux"   // persisted procfs.linux mode (procfsd restores it)
private let kLinuxVerConf = "/var/db/procfs.linux_version"  // persisted spoof-version index

// Spoofable Linux kernel releases. Order MUST match procfs_linux_versions[] in
// the kext (index here + 1 == the procfs.linux_version sysctl value; 0 = None).
private let kLinuxVersions = ["6.12.0", "6.6.0", "6.1.0", "5.15.0", "5.10.0"]

// MARK: - Shell helpers

/// Run a tool and capture its stdout (unprivileged). Returns "" on failure.
private func shell(_ path: String, _ args: [String]) -> String {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: path)
    task.arguments = args
    let out = Pipe()
    task.standardOutput = out
    task.standardError = Pipe()
    do { try task.run() } catch { return "" }
    task.waitUntilExit()
    let data = out.fileHandleForReading.readDataToEndOfFile()
    return String(data: data, encoding: .utf8) ?? ""
}

/// Run a command as root via the standard macOS authorization prompt.
@discardableResult
private func runPrivileged(_ command: String) -> Bool {
    let source = "do shell script \"\(command)\" with administrator privileges"
    guard let script = NSAppleScript(source: source) else { return false }
    var err: NSDictionary?
    script.executeAndReturnError(&err)
    return err == nil
}

// MARK: - Status

private func isMounted() -> Bool {
    // mount(8) prints e.g. "procfs on /proc (procfs, local, ...)".
    return shell("/sbin/mount", []).contains(" on \(kMountPoint) (procfs")
}

private func daemonRunning() -> Bool {
    return !shell("/usr/bin/pgrep", ["-x", "procfsd"])
        .trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
}

private func linuxModeOn() -> Bool {
    // The oid exists only while the kext is loaded; absent -> off.
    return shell("/usr/sbin/sysctl", ["-n", "procfs.linux"])
        .trimmingCharacters(in: .whitespacesAndNewlines) == "1"
}

// Selected spoof-version index (0 = None/Darwin, 1..N = kLinuxVersions[idx-1]).
private func linuxVersionIndex() -> Int {
    return Int(shell("/usr/sbin/sysctl", ["-n", "procfs.linux_version"])
        .trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private var iconLight: NSImage?     // light-coloured art, shown on a dark menu bar
    private var iconDark: NSImage?      // dark-coloured art, shown on a light menu bar

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        iconLight = loadIcon("icon_light")
        iconDark  = loadIcon("icon_dark")
        setIcon()

        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu

        // On the very first launch, register to open at login so the icon comes
        // back automatically after a reboot (installer launches us once).
        registerLoginItemOnFirstLaunch()

        // Automatic update check on launch, when enabled in the preference pane.
        if ProcFSUpdater.checkOnStartup {
            let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
            ProcFSUpdater.check(currentVersion: v, silent: true)
        }
    }

    private func loadIcon(_ name: String) -> NSImage? {
        guard let url = Bundle.main.url(forResource: name, withExtension: "png"),
              let image = NSImage(contentsOf: url) else { return nil }
        image.size = NSSize(width: 18, height: 18)      // menu-bar sized
        return image
    }

    // Use the white (light-coloured) icon by default; fall back to the dark art,
    // then to a text title, if the white art is unavailable.
    private func setIcon() {
        guard let button = statusItem?.button else { return }
        if let image = iconLight ?? iconDark {
            button.image = image
            button.title = ""
        } else {
            button.image = nil
            button.title = "proc"
        }
    }

    // Rebuild the menu each time it opens so the status is always current.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        // Preferences (opens the System Settings pane) - at the very top.
        addAction(menu, "Preferences…", #selector(openPreferences))
        menu.addItem(.separator())

        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        addDisabled(menu, "ProcFS \(version)")
        menu.addItem(.separator())

        let mounted = isMounted()
        let daemon  = daemonRunning()
        let linux   = linuxModeOn()

        addDisabled(menu, "Mount: " + (mounted ? "mounted at \(kMountPoint)" : "not mounted"))
        addDisabled(menu, "Daemon: " + (daemon ? "running" : "stopped"))
        addDisabled(menu, "Linux mode: " + (linux ? "on" : "off"))
        if linux {
            let vi = linuxVersionIndex()
            let vlabel = (vi >= 1 && vi <= kLinuxVersions.count)
                ? "Linux \(kLinuxVersions[vi - 1])" : "Darwin (native)"
            addDisabled(menu, "Kernel version: " + vlabel)
        }
        menu.addItem(.separator())

        addAction(menu, mounted ? "Unmount \(kMountPoint)" : "Mount \(kMountPoint)",
                  #selector(toggleMount))

        let linuxItem = addAction(menu, "Linux compatibility", #selector(toggleLinux))
        linuxItem.state = linux ? .on : .off

        // The version-spoof dropdown is only offered while Linux compatibility is on.
        if linux {
            let verItem = NSMenuItem(title: "Spoof Linux Kernel Version",
                                     action: nil, keyEquivalent: "")
            let submenu = NSMenu()
            let cur = linuxVersionIndex()
            for (i, v) in kLinuxVersions.enumerated() {
                let it = NSMenuItem(title: "Linux \(v)",
                                    action: #selector(setLinuxVersion(_:)), keyEquivalent: "")
                it.target = self
                it.tag = i + 1
                it.state = (cur == i + 1) ? .on : .off
                submenu.addItem(it)
            }
            submenu.addItem(.separator())
            let none = NSMenuItem(title: "None (Darwin)",
                                  action: #selector(setLinuxVersion(_:)), keyEquivalent: "")
            none.target = self
            none.tag = 0
            none.state = (cur == 0) ? .on : .off
            submenu.addItem(none)
            verItem.submenu = submenu
            menu.addItem(verItem)
        }

        addAction(menu, daemon ? "Stop daemon" : "Start daemon", #selector(toggleDaemon))

        menu.addItem(.separator())
        if #available(macOS 13.0, *) {
            let loginItem = addAction(menu, "Open at Login", #selector(toggleLoginItem))
            loginItem.state = loginEnabled ? .on : .off
        }
        addAction(menu, "Quit", #selector(NSApplication.terminate(_:)), target: NSApp)
    }

    // MARK: menu builders

    private func addDisabled(_ menu: NSMenu, _ title: String) {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        menu.addItem(item)
    }

    @discardableResult
    private func addAction(_ menu: NSMenu, _ title: String, _ sel: Selector,
                           target: AnyObject? = nil) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: sel, keyEquivalent: "")
        item.target = target ?? self
        menu.addItem(item)
        return item
    }

    // MARK: actions

    // Open the ProcFS preference pane in System Settings.
    @objc private func openPreferences() {
        let path = "/Library/PreferencePanes/ProcFS.prefPane"
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }

    @objc private func toggleMount() {
        if isMounted() {
            runPrivileged("/sbin/umount \(kMountPoint)")
        } else {
            runPrivileged("/sbin/mount -t procfs procfs \(kMountPoint)")
        }
    }

    @objc private func toggleLinux() {
        // Apply live, and persist the choice so procfsd restores it after a
        // reboot / kext reload. Both run as root in one privileged step; the
        // file is written only if the sysctl set succeeds.
        let want = linuxModeOn() ? "0" : "1"
        _ = runPrivileged("/usr/sbin/sysctl -w procfs.linux=\(want) && "
                        + "/bin/echo \(want) > \(kLinuxConf)")
    }

    // Set the spoofed Linux kernel version (tag: 0 = None, 1..N = preset). Applied
    // live and persisted like Linux mode, so procfsd restores it across reboots.
    @objc private func setLinuxVersion(_ sender: NSMenuItem) {
        let idx = sender.tag
        _ = runPrivileged("/usr/sbin/sysctl -w procfs.linux_version=\(idx) && "
                        + "/bin/echo \(idx) > \(kLinuxVerConf)")
    }

    @objc private func toggleDaemon() {
        if daemonRunning() {
            runPrivileged("/bin/launchctl bootout system/\(kDaemonLabel)")
        } else {
            runPrivileged("/bin/launchctl bootstrap system \(kDaemonPlist)")
        }
    }

    // Whether the app is registered to open at login (SMAppService, no privilege).
    private var loginEnabled: Bool {
        if #available(macOS 13.0, *) {
            return SMAppService.mainApp.status == .enabled
        }
        return false
    }

    // Register "Open at Login" exactly once, on the first launch, so a fresh
    // install (which launches us once) auto-starts on later reboots. Tracked in
    // UserDefaults so it happens only once - a user who later turns it off is
    // respected and never silently re-enabled. Best-effort and silent: it can
    // fail when the app is run from outside /Applications, which is fine.
    private func registerLoginItemOnFirstLaunch() {
        guard #available(macOS 13.0, *) else { return }
        let key = "ProcFSDidAutoRegisterLoginItem"
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: key) else { return }
        defaults.set(true, forKey: key)
        if SMAppService.mainApp.status != .enabled {
            try? SMAppService.mainApp.register()
        }
    }

    @objc private func toggleLoginItem() {
        guard #available(macOS 13.0, *) else { return }
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
        } catch {
            let alert = NSAlert()
            alert.messageText = "Could not change the Open at Login setting."
            alert.informativeText = error.localizedDescription
            alert.runModal()
        }
    }
}
