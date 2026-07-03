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

private let kMountPoint  = "/proc"
private let kDaemonLabel = "com.beako.procfsd"
private let kDaemonPlist = "/Library/LaunchDaemons/com.beako.procfsd.plist"

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

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private var iconLight: NSImage?     // light-coloured art, shown on a dark menu bar
    private var iconDark: NSImage?      // dark-coloured art, shown on a light menu bar

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        iconLight = loadIcon("icon_light")
        iconDark  = loadIcon("icon_dark")
        updateIcon()

        // The menu bar can be light or dark independently of the app; re-pick the
        // icon whenever the system appearance changes so it stays legible.
        DistributedNotificationCenter.default.addObserver(
            self, selector: #selector(updateIcon),
            name: NSNotification.Name("AppleInterfaceThemeChangedNotification"), object: nil)

        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
    }

    private func loadIcon(_ name: String) -> NSImage? {
        guard let url = Bundle.main.url(forResource: name, withExtension: "png"),
              let image = NSImage(contentsOf: url) else { return nil }
        image.size = NSSize(width: 18, height: 18)      // menu-bar sized
        return image
    }

    // Choose the icon that contrasts with the current menu-bar appearance. Runs
    // again on the next runloop tick, since effectiveAppearance can lag the
    // theme-change notification by a cycle.
    @objc private func updateIcon() {
        setIcon()
        DispatchQueue.main.async { [weak self] in self?.setIcon() }
    }

    private func setIcon() {
        guard let button = statusItem?.button else { return }
        let isDark = button.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        if let image = isDark ? iconLight : iconDark {
            button.image = image
            button.title = ""
        } else {
            button.image = nil
            button.title = "proc"                       // fallback if art missing
        }
    }

    // Rebuild the menu each time it opens so the status is always current.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        addDisabled(menu, "ProcFS \(version)")
        menu.addItem(.separator())

        let mounted = isMounted()
        let daemon  = daemonRunning()
        let linux   = linuxModeOn()

        addDisabled(menu, "Mount: " + (mounted ? "mounted at \(kMountPoint)" : "not mounted"))
        addDisabled(menu, "Daemon: " + (daemon ? "running" : "stopped"))
        addDisabled(menu, "Linux mode: " + (linux ? "on" : "off"))
        menu.addItem(.separator())

        addAction(menu, mounted ? "Unmount \(kMountPoint)" : "Mount \(kMountPoint)",
                  #selector(toggleMount))

        let linuxItem = addAction(menu, "Linux compatibility", #selector(toggleLinux))
        linuxItem.state = linux ? .on : .off

        addAction(menu, daemon ? "Stop daemon" : "Start daemon", #selector(toggleDaemon))

        menu.addItem(.separator())
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

    @objc private func toggleMount() {
        if isMounted() {
            runPrivileged("/sbin/umount \(kMountPoint)")
        } else {
            runPrivileged("/sbin/mount -t procfs procfs \(kMountPoint)")
        }
    }

    @objc private func toggleLinux() {
        runPrivileged("/usr/sbin/sysctl -w procfs.linux=\(linuxModeOn() ? "0" : "1")")
    }

    @objc private func toggleDaemon() {
        if daemonRunning() {
            runPrivileged("/bin/launchctl bootout system/\(kDaemonLabel)")
        } else {
            runPrivileged("/bin/launchctl bootstrap system \(kDaemonPlist)")
        }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)      // menu-bar only, no Dock icon
let delegate = AppDelegate()
app.delegate = delegate
app.run()
