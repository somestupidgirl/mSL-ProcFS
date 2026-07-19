//
// Copyright (c) 2026 Sunneva N. Mariu
//
// ProcFSMenu.swift
//
// A menu-bar (status-bar) app for ProcFS. Its menu shows live status - whether
// /proc is mounted, whether the procfsd daemon is running, and whether Linux
// Compatibility Mode is on - and offers one-click toggles for each. The mutating
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
private let kLinuxVersions = ["6.12.0", "6.6.0", "6.1.0", "5.15.0", "5.10.0", "2.5.47"]

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
    private var appearanceObserver: NSKeyValueObservation?
    private var menuOpen = false

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        iconLight = loadIcon("icon_light")
        iconDark  = loadIcon("icon_dark")
        setIcon()

        // Re-pick the art whenever the menu bar changes appearance: the General
        // settings light/dark switch, Auto at sunset, or a wallpaper change that
        // flips the translucent bar. Observing the button's effectiveAppearance
        // catches all of them, where watching AppleInterfaceStyle would miss the
        // last one.
        appearanceObserver = statusItem.button?.observe(\.effectiveAppearance) { [weak self] _, _ in
            self?.setIcon()
        }

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
        // Deliberately NOT a template image: a template is tinted from its alpha
        // channel, so the two colour variants would render identically and the
        // swap below would have no effect. The cost is that the highlighted
        // state (while the menu is open) no longer inverts automatically, which
        // menuWillOpen/menuDidClose compensate for.
        image.isTemplate = false
        return image
    }

    /// True when the menu bar is drawing dark, so the light-coloured art should
    /// be used. The status item's own button is asked rather than NSApp, because
    /// it is the view actually sitting in the menu bar.
    private var menuBarIsDark: Bool {
        guard let button = statusItem?.button else { return true }
        return button.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
    }

    // Pick the art that contrasts with the current menu bar: light art on a dark
    // bar, dark art on a light one. Falls back to whichever variant loaded, then
    // to a text title.
    private func setIcon() {
        guard let button = statusItem?.button else { return }
        // While the menu is open the button paints a dark highlight behind the
        // icon regardless of theme, so the light art is the readable one.
        let wantLight = menuOpen || menuBarIsDark
        let preferred = wantLight ? iconLight : iconDark
        if let image = preferred ?? iconLight ?? iconDark {
            button.image = image
            button.title = ""
        } else {
            button.image = nil
            button.title = "proc"
        }
    }

    // While the menu is open the button paints a dark highlight behind the icon
    // in either theme. A template image would invert itself; ordinary art has to
    // be swapped by hand.
    func menuWillOpen(_ menu: NSMenu) {
        menuOpen = true
        setIcon()
    }

    func menuDidClose(_ menu: NSMenu) {
        menuOpen = false
        setIcon()
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

        addDisabled(menu, "Mount: " + (mounted ? "Mounted at \(kMountPoint)" : "Not Mounted"))
        addDisabled(menu, "Daemon: " + (daemon ? "running" : "stopped"))
        addDisabled(menu, "Linux Mode: " + (linux ? "On" : "Off"))
        if linux {
            let vi = linuxVersionIndex()
            let vlabel = (vi >= 1 && vi <= kLinuxVersions.count)
                ? "Linux \(kLinuxVersions[vi - 1])" : "Darwin (native)"
            addDisabled(menu, "Kernel version: " + vlabel)
        }
        menu.addItem(.separator())

        // Eject sits in the state column rather than the title, so it lines up
        // with the checkmark on Linux Compatibility below.
        let mountItem = addAction(menu, mounted ? "Unmount \(kMountPoint)"
                                                : "Mount \(kMountPoint)",
                                  #selector(toggleMount))
        if mounted { setStateSymbol(mountItem, "eject.fill") }

        let linuxItem = addAction(menu, "Linux Compatibility", #selector(toggleLinux))
        linuxItem.state = linux ? .on : .off

        // The version-spoof dropdown is only offered while Linux Compatibility is on.
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

        addAction(menu, daemon ? "Running" : "Not Running", #selector(toggleDaemon))

        // Open at Login gets a section of its own. The separators are added
        // inside the availability check so macOS 12, which does not get the
        // item, does not end up with two adjacent separators either.
        if #available(macOS 13.0, *) {
            menu.addItem(.separator())
            let loginItem = addAction(menu, "Open at Login", #selector(toggleLoginItem))
            loginItem.state = loginEnabled ? .on : .off
        }

        // Both symbols go in the image column. macOS gives terminate: an exit
        // symbol there on its own, so Quit's is set explicitly too - relying on
        // the automatic one would leave its size and position outside our
        // control, and the two rows have to match.
        menu.addItem(.separator())
        let aboutItem = addAction(menu, "About ProcFS", #selector(showAbout))
        setImageSymbol(aboutItem, "info.circle")
        let quitItem = addAction(menu, "Quit", #selector(NSApplication.terminate(_:)),
                                 target: NSApp)
        setImageSymbol(quitItem, "power")
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

    /// Load an SF Symbol sized and tinted for a menu row.
    private func menuSymbol(_ symbol: String, pointSize: CGFloat) -> NSImage? {
        guard let image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
        else { return nil }
        let sized = image.withSymbolConfiguration(
            NSImage.SymbolConfiguration(pointSize: pointSize, weight: .regular)) ?? image
        sized.isTemplate = true     // tint with the menu, including when highlighted
        return sized
    }

    /// Put a symbol in an item's *state* column - the narrow slot where AppKit
    /// draws the checkmark on a toggle. Use this to line a symbol up with the
    /// checkmarks; onStateImage replaces the checkmark glyph, and switching the
    /// state on is what makes it draw.
    private func setStateSymbol(_ item: NSMenuItem, _ symbol: String) {
        guard let image = menuSymbol(symbol, pointSize: 12) else { return }
        item.onStateImage = image
        item.state = .on
    }

    /// Put a symbol in an item's *image* column, which sits between the state
    /// column and the title.
    ///
    /// Recent macOS fills this column in by itself for standard actions -
    /// terminate: acquires an exit symbol with no help from us. An item whose
    /// symbol went in the state column would therefore sit in a different
    /// column from its neighbour and be visibly out of line, so items grouped
    /// with such an action set their symbol here and match.
    private func setImageSymbol(_ item: NSMenuItem, _ symbol: String) {
        item.image = menuSymbol(symbol, pointSize: 14)
    }

    // MARK: actions

    // Standard About panel, populated from Info.plist (name, version, icon and
    // NSHumanReadableCopyright). A menu-bar app is not the active application,
    // so activate first or the panel opens behind whatever is in front.
    @objc private func showAbout() {
        NSApp.activate(ignoringOtherApps: true)
        NSApp.orderFrontStandardAboutPanel(options: [:])
    }

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
    // live and persisted like Linux Mode, so procfsd restores it across reboots.
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
