//
// Copyright (c) 2026 Sunneva N. Mariu
//
// ProcFSPref.swift
//
// A System Settings preference pane (NSPreferencePane) for ProcFS. Mirrors the
// menu-bar app's controls - startup, daemon, Linux compatibility, version spoof
// - plus update checking, in the layout of Apple's stock panes. Mutating actions
// that need root go through the standard administrator-authorization prompt;
// status is read unprivileged. Opened from the menu-bar app's "Preferences" item.
//
import AppKit
import PreferencePanes
import ServiceManagement

private let kMountPoint     = "/proc"
private let kDaemonLabel    = "com.beako.procfsd"
private let kDaemonPlist    = "/Library/LaunchDaemons/com.beako.procfsd.plist"
private let kLinuxConf      = "/var/db/procfs.linux"
private let kLinuxVerConf   = "/var/db/procfs.linux_version"
private let kMenuApp        = "/Applications/ProcFS.app"
private let kLoginLabel     = "com.beako.procfs.gui"
private let kLoginAgent     = (NSHomeDirectory() as NSString)
    .appendingPathComponent("Library/LaunchAgents/com.beako.procfs.gui.plist")

// Spoofable Linux kernel releases. Order MUST match procfs_linux_versions[] in
// the kext (index here + 1 == the procfs.linux_version sysctl value; 0 = None).
private let kLinuxVersions = ["6.12.0", "6.6.0", "6.1.0", "5.15.0", "5.10.0"]

private let kDescription = """
A process file system, common to BSD and Linux systems, that exposes running \
processes and threads as a pseudo-filesystem with per-process information, \
mounted at /proc. In addition to visualizing running processes, ProcFS allows \
some measure of control over the processes to privileged users. The system also \
contains a Linux-compatibility layer that emulates features commonly found on \
the Linux ProcFS, intended for seamlessly running Linux scripts on macOS that \
rely on Linux' ProcFS.
"""

// MARK: - Shell helpers

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

@discardableResult
private func runPrivileged(_ command: String) -> Bool {
    let source = "do shell script \"\(command)\" with administrator privileges"
    guard let script = NSAppleScript(source: source) else { return false }
    var err: NSDictionary?
    script.executeAndReturnError(&err)
    return err == nil
}

private func daemonRunning() -> Bool {
    !shell("/usr/bin/pgrep", ["-x", "procfsd"])
        .trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
}

private func linuxModeOn() -> Bool {
    shell("/usr/sbin/sysctl", ["-n", "procfs.linux"])
        .trimmingCharacters(in: .whitespacesAndNewlines) == "1"
}

private func linuxVersionIndex() -> Int {
    Int(shell("/usr/sbin/sysctl", ["-n", "procfs.linux_version"])
        .trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
}

private func startupEnabled() -> Bool {
    FileManager.default.fileExists(atPath: kLoginAgent)
}

// MARK: - Preference pane

@objc(ProcFSPref)
final class ProcFSPref: NSPreferencePane {

    private var startupSwitch: NSSwitch!
    private var daemonButton: NSButton!
    private var linuxSwitch: NSSwitch!
    private var versionPopup: NSPopUpButton!
    private var updateSwitch: NSSwitch!

    private var bundleVersion: String {
        Bundle(for: type(of: self)).infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    }

    override func loadMainView() -> NSView {
        let view = NSView(frame: NSRect(x: 0, y: 0, width: 620, height: 620))

        let content = NSStackView()
        content.orientation = .vertical
        content.alignment = .leading
        content.spacing = 16
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 20),
            content.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -20),
            content.topAnchor.constraint(equalTo: view.topAnchor, constant: 20),
            content.bottomAnchor.constraint(lessThanOrEqualTo: view.bottomAnchor, constant: -18),
        ])

        content.addArrangedSubview(card(makeHeader()))
        content.addArrangedSubview(sectionLabel("Settings"))
        content.addArrangedSubview(card(makeSettings()))
        content.addArrangedSubview(makeFooter())

        // Every group stretches to the full content width, like System Settings.
        content.arrangedSubviews.forEach {
            $0.widthAnchor.constraint(equalTo: content.widthAnchor).isActive = true
        }

        mainView = view
        return view
    }

    override func didSelect() { refresh() }

    // MARK: sections

    // Card fill roughly matching System Settings' grouped boxes: a white card on
    // the light-grey window in Aqua, a subtle raised overlay in Dark Aqua. Uses a
    // dynamic NSColor so NSBox re-renders on an appearance change.
    private let cardFill = NSColor(name: nil) { ap in
        ap.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            ? NSColor(white: 1.0, alpha: 0.09)
            : NSColor.white
    }

    /// Wrap `content` in a rounded System-Settings-style card with inset padding.
    private func card(_ content: NSView, padding: CGFloat = 16) -> NSView {
        let box = NSBox()
        box.boxType = .custom
        box.titlePosition = .noTitle
        box.borderWidth = 0
        box.cornerRadius = 10
        box.fillColor = cardFill
        box.contentView = content
        box.contentViewMargins = NSSize(width: padding, height: padding)
        return box
    }

    private func makeHeader() -> NSView {
        // App-style rounded (squircle-ish) icon, like the stock panes.
        let icon = NSImageView()
        icon.imageScaling = .scaleProportionallyUpOrDown
        if let url = Bundle(for: type(of: self)).url(forResource: "appicon", withExtension: "png") {
            icon.image = NSImage(contentsOf: url)
        }
        icon.wantsLayer = true
        icon.layer?.cornerRadius = 10
        icon.layer?.masksToBounds = true
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 48).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 48).isActive = true

        let title = NSTextField(labelWithString: "ProcFS")
        title.font = NSFont.systemFont(ofSize: 17, weight: .semibold)

        let desc = NSTextField(wrappingLabelWithString: kDescription)
        desc.font = NSFont.systemFont(ofSize: 12)
        desc.textColor = .secondaryLabelColor
        desc.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let text = NSStackView(views: [title, desc])
        text.orientation = .vertical
        text.alignment = .leading
        text.spacing = 4
        text.setHuggingPriority(.defaultLow, for: .horizontal)

        let header = NSStackView(views: [icon, text])
        header.orientation = .horizontal
        header.alignment = .top
        header.spacing = 14
        return header
    }

    private func makeSettings() -> NSView {
        startupSwitch = NSSwitch()
        startupSwitch.target = self; startupSwitch.action = #selector(toggleStartup(_:))

        daemonButton = NSButton(title: "Start", target: self, action: #selector(toggleDaemon(_:)))
        daemonButton.bezelStyle = .rounded

        linuxSwitch = NSSwitch()
        linuxSwitch.target = self; linuxSwitch.action = #selector(toggleLinux(_:))

        versionPopup = NSPopUpButton()
        for v in kLinuxVersions { versionPopup.addItem(withTitle: "Linux \(v)") }
        versionPopup.menu?.addItem(.separator())
        versionPopup.addItem(withTitle: "None (Darwin)")
        versionPopup.target = self; versionPopup.action = #selector(changeVersion(_:))

        updateSwitch = NSSwitch()
        updateSwitch.target = self; updateSwitch.action = #selector(toggleUpdateStartup(_:))

        let checkButton = NSButton(title: "Check Now", target: self, action: #selector(checkNow(_:)))
        checkButton.bezelStyle = .rounded

        let rows = NSStackView(views: [
            row("Run on System Startup", startupSwitch),
            row("Daemon", daemonButton),
            row("Linux Compatibility Mode", linuxSwitch),
            row("Spoof Linux Kernel Version", versionPopup),
            row("Check for Updates on Startup", updateSwitch),
            row("Check for Updates", checkButton),
        ])
        rows.orientation = .vertical
        rows.alignment = .leading
        rows.spacing = 12
        // Each row spans the card so the controls line up at the right edge.
        rows.arrangedSubviews.forEach {
            $0.widthAnchor.constraint(equalTo: rows.widthAnchor).isActive = true
        }
        return rows
    }

    private func makeFooter() -> NSView {
        let link = NSButton(title: "ProcFS v\(bundleVersion): \(ProcFSUpdater.repoURL)",
                            target: self, action: #selector(openRepo(_:)))
        link.isBordered = false
        link.contentTintColor = .linkColor
        link.alignment = .left
        (link.cell as? NSButtonCell)?.highlightsBy = []

        let copyright = NSTextField(labelWithString: "Copyright (c) 2026 Sunneva N. Mariu")
        copyright.font = NSFont.systemFont(ofSize: 11)
        copyright.textColor = .secondaryLabelColor

        let footer = NSStackView(views: [link, copyright])
        footer.orientation = .vertical
        footer.alignment = .leading
        footer.spacing = 4
        return footer
    }

    // MARK: row/section helpers

    private func row(_ label: String, _ control: NSView) -> NSView {
        let text = NSTextField(labelWithString: label)
        text.font = NSFont.systemFont(ofSize: 13)
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let stack = NSStackView(views: [text, spacer, control])
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        return stack
    }

    private func sectionLabel(_ s: String) -> NSView {
        // A System-Settings-style group header: small, semibold, secondary, with a
        // little left inset so it lines up inside the card's rounded edge.
        let l = NSTextField(labelWithString: s)
        l.font = NSFont.systemFont(ofSize: 12, weight: .semibold)
        l.textColor = .secondaryLabelColor
        let holder = NSStackView(views: [l])
        holder.orientation = .horizontal
        holder.edgeInsets = NSEdgeInsets(top: 2, left: 6, bottom: 0, right: 0)
        return holder
    }

    // MARK: state

    private func refresh() {
        startupSwitch.state = startupEnabled() ? .on : .off
        let running = daemonRunning()
        daemonButton.title = running ? "Stop" : "Start"
        let linux = linuxModeOn()
        linuxSwitch.state = linux ? .on : .off
        // popup: 1..N -> release rows (index-1); 0 -> the trailing "None" item.
        let vi = linuxVersionIndex()
        versionPopup.selectItem(at: (vi >= 1 && vi <= kLinuxVersions.count)
                                    ? vi - 1 : versionPopup.numberOfItems - 1)
        versionPopup.isEnabled = linux
        updateSwitch.state = ProcFSUpdater.checkOnStartup ? .on : .off
    }

    // MARK: actions

    @objc private func toggleStartup(_ sender: NSSwitch) {
        if sender.state == .on {
            let plist = """
            <?xml version="1.0" encoding="UTF-8"?>
            <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
            <plist version="1.0"><dict>
              <key>Label</key><string>\(kLoginLabel)</string>
              <key>ProgramArguments</key>
              <array><string>/usr/bin/open</string><string>\(kMenuApp)</string></array>
              <key>RunAtLoad</key><true/>
              <key>LimitLoadToSessionType</key><string>Aqua</string>
            </dict></plist>
            """
            try? FileManager.default.createDirectory(
                atPath: (kLoginAgent as NSString).deletingLastPathComponent,
                withIntermediateDirectories: true)
            try? plist.write(toFile: kLoginAgent, atomically: true, encoding: .utf8)
            _ = shell("/bin/launchctl", ["bootstrap", "gui/\(getuid())", kLoginAgent])
        } else {
            _ = shell("/bin/launchctl", ["bootout", "gui/\(getuid())/\(kLoginLabel)"])
            try? FileManager.default.removeItem(atPath: kLoginAgent)
        }
        refresh()
    }

    @objc private func toggleDaemon(_ sender: NSButton) {
        if daemonRunning() {
            runPrivileged("/bin/launchctl bootout system/\(kDaemonLabel)")
        } else {
            runPrivileged("/bin/launchctl bootstrap system \(kDaemonPlist)")
        }
        refresh()
    }

    @objc private func toggleLinux(_ sender: NSSwitch) {
        let want = sender.state == .on ? "1" : "0"
        _ = runPrivileged("/usr/sbin/sysctl -w procfs.linux=\(want) && "
                        + "/bin/echo \(want) > \(kLinuxConf)")
        refresh()
    }

    @objc private func changeVersion(_ sender: NSPopUpButton) {
        // Rows 0..N-1 map to sysctl 1..N; the trailing "None" item maps to 0.
        let sel = sender.indexOfSelectedItem
        let idx = (sel >= 0 && sel < kLinuxVersions.count) ? sel + 1 : 0
        _ = runPrivileged("/usr/sbin/sysctl -w procfs.linux_version=\(idx) && "
                        + "/bin/echo \(idx) > \(kLinuxVerConf)")
        refresh()
    }

    @objc private func toggleUpdateStartup(_ sender: NSSwitch) {
        ProcFSUpdater.checkOnStartup = (sender.state == .on)
    }

    @objc private func checkNow(_ sender: NSButton) {
        ProcFSUpdater.check(currentVersion: bundleVersion, silent: false)
    }

    @objc private func openRepo(_ sender: NSButton) {
        if let u = URL(string: ProcFSUpdater.repoURL) { NSWorkspace.shared.open(u) }
    }
}
