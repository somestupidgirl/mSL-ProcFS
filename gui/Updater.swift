//
// Copyright (c) 2026 Sunneva N. Mariu
//
// Updater.swift
//
// Shared update check for the ProcFS menu-bar app and the System Settings
// preference pane. Queries the project's GitHub Releases for the latest tag,
// compares it against the caller's version, and - if newer - prompts to open
// the releases page. Compiled into both targets.
//
import AppKit

enum ProcFSUpdater {
    static let repo    = "somestupidgirl/mSL-ProcFS"
    static let repoURL = "https://github.com/somestupidgirl/mSL-ProcFS"

    // Preferences shared between the menu-bar app and the preference pane.
    static let prefsSuite      = "com.beako.filesystems.procfs"
    static let checkOnStartKey = "CheckForUpdatesOnStartup"

    static var checkOnStartup: Bool {
        get { UserDefaults(suiteName: prefsSuite)?.bool(forKey: checkOnStartKey) ?? false }
        set { UserDefaults(suiteName: prefsSuite)?.set(newValue, forKey: checkOnStartKey) }
    }

    /// Check GitHub for a newer release. `silent` suppresses the "up to date" and
    /// error alerts (used for the automatic on-startup check); a genuinely newer
    /// release always prompts.
    static func check(currentVersion: String, silent: Bool) {
        guard let url = URL(string: "https://api.github.com/repos/\(repo)/releases/latest") else {
            return
        }
        var req = URLRequest(url: url)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.timeoutInterval = 15

        URLSession.shared.dataTask(with: req) { data, _, err in
            var latest: String?
            if let data = data,
               let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let tag = obj["tag_name"] as? String {
                latest = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
            }
            DispatchQueue.main.async {
                present(current: currentVersion, latest: latest, error: err, silent: silent)
            }
        }.resume()
    }

    private static func present(current: String, latest: String?, error: Error?, silent: Bool) {
        guard let latest = latest, !latest.isEmpty else {
            if !silent {
                let a = NSAlert()
                a.messageText = "Could not check for updates."
                a.informativeText = error?.localizedDescription
                    ?? "No release information was returned by GitHub."
                a.runModal()
            }
            return
        }
        if isNewer(latest, than: current) {
            let a = NSAlert()
            a.messageText = "A new version of ProcFS is available."
            a.informativeText = "You have \(current); the latest release is \(latest). "
                              + "Open the releases page to download it?"
            a.addButton(withTitle: "Open Releases")
            a.addButton(withTitle: "Later")
            if a.runModal() == .alertFirstButtonReturn,
               let u = URL(string: "\(repoURL)/releases/latest") {
                NSWorkspace.shared.open(u)
            }
        } else if !silent {
            let a = NSAlert()
            a.messageText = "ProcFS is up to date."
            a.informativeText = "You have the latest version (\(current))."
            a.runModal()
        }
    }

    /// Numeric dotted-version comparison (1.0.10 > 1.0.2), tolerant of missing
    /// components (1.1 == 1.1.0).
    static func isNewer(_ latest: String, than current: String) -> Bool {
        let l = latest.split(separator: ".").map { Int($0) ?? 0 }
        let c = current.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(l.count, c.count) {
            let lv = i < l.count ? l[i] : 0
            let cv = i < c.count ? c[i] : 0
            if lv != cv { return lv > cv }
        }
        return false
    }
}
