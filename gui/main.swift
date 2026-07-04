//
// Copyright (c) 2026 Sunneva N. Mariu
//
// main.swift
//
// Entry point for the ProcFS menu-bar app. Kept separate from ProcFSMenu.swift
// because top-level executable code is only allowed in a file named main.swift
// once the module has more than one source file (ProcFSMenu.swift + Updater.swift).
//
import Cocoa

let app = NSApplication.shared
app.setActivationPolicy(.accessory)      // menu-bar only, no Dock icon
let delegate = AppDelegate()
app.delegate = delegate
app.run()
