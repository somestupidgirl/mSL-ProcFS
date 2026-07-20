//
// Copyright (c) 2026 Sunneva N. Mariu
//
// Uninstaller.swift
//
// Uninstall-ProcFS.app: a confirm-and-run wrapper around the uninstall.sh that
// ships in its Resources. The app holds no removal logic of its own, so there
// is exactly one list of what an uninstall touches and the Terminal front-end
// (installer/uninstall.command) and this app can never drift apart.
//
import Cocoa

@main
struct Uninstaller {
    static func main() {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.activate(ignoringOtherApps: true)
        run()
        app.terminate(nil)
    }

    private static func run() {
        guard let script = Bundle.main.path(forResource: "uninstall", ofType: "sh") else {
            alert(.critical, "Uninstaller is incomplete",
                  "uninstall.sh is missing from this app's Resources. Reinstall "
                  + "ProcFS, or run 'sudo make uninstall' from a source tree.")
            return
        }

        let confirm = NSAlert()
        confirm.alertStyle = .warning
        confirm.messageText = "Uninstall mSL/ProcFS?"
        confirm.informativeText = """
            This removes the ProcFS kernel extension, the procfsd daemon, the \
            menu-bar app, the preference pane and their settings, and unmounts \
            /proc.

            You will be asked for an administrator password. The now-empty /proc \
            mount point persists until the next reboot.
            """
        confirm.addButton(withTitle: "Uninstall")
        confirm.addButton(withTitle: "Cancel")
        guard confirm.runModal() == .alertFirstButtonReturn else { return }

        // The script deletes this app bundle, and bash reads a script as it
        // goes - running it in place would pull the remaining lines out from
        // under the interpreter. Run a copy from a temporary directory instead.
        let copy = NSTemporaryDirectory()
            + "procfs-uninstall-\(UUID().uuidString).sh"
        do {
            try FileManager.default.copyItem(atPath: script, toPath: copy)
        } catch {
            alert(.critical, "Could not start the uninstaller",
                  "Failed to stage the uninstall script: \(error.localizedDescription)")
            return
        }
        defer { try? FileManager.default.removeItem(atPath: copy) }

        switch runAsAdmin(copy) {
        case .cancelled:
            return
        case .success(let output):
            alert(.informational, "mSL/ProcFS has been uninstalled",
                  output.isEmpty
                  ? "Every installed component was removed."
                  : output)
        case .failure(let output):
            alert(.critical, "The uninstall did not finish",
                  output.isEmpty
                  ? "The uninstall script reported an error but produced no output."
                  : output)
        }
    }

    private enum Outcome {
        case success(String)
        case failure(String)
        case cancelled
    }

    /// Run a script as root. AppleScript's `with administrator privileges` is
    /// the supported way for an unprivileged app to ask for the system's
    /// authentication panel; AuthorizationExecuteWithPrivileges is long gone.
    private static func runAsAdmin(_ path: String) -> Outcome {
        let source = "do shell script \"/bin/bash \" & quoted form of "
            + "\"\(path)\" & \" 2>&1\" with administrator privileges"

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        task.arguments = ["-e", source]
        let out = Pipe(), err = Pipe()
        task.standardOutput = out
        task.standardError = err

        do {
            try task.run()
        } catch {
            return .failure("Could not run osascript: \(error.localizedDescription)")
        }
        let stdoutData = out.fileHandleForReading.readDataToEndOfFile()
        let stderrData = err.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()

        let stdoutText = String(decoding: stdoutData, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let stderrText = String(decoding: stderrData, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)

        if task.terminationStatus == 0 { return .success(stdoutText) }
        // -128 is AppleScript's "user cancelled", i.e. the password panel was
        // dismissed. Nothing has been removed at that point, so say nothing.
        if stderrText.contains("-128") { return .cancelled }
        return .failure(stderrText.isEmpty ? stdoutText : stderrText)
    }

    private static func alert(_ style: NSAlert.Style, _ message: String, _ detail: String) {
        let a = NSAlert()
        a.alertStyle = style
        a.messageText = message
        a.informativeText = detail
        a.addButton(withTitle: "OK")
        a.runModal()
    }
}
