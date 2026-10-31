// NFSU2 launcher: the app's main executable.
//
// Contents/MacOS/NFSU2              this launcher
// Contents/MacOS/nfsu2-engine       the port (original look)
// Contents/MacOS/nfsu2-engine-hd    the port, HD build (optional)
//
// It finds and checks the game, offers the few choices a player needs, then
// starts the engine as a child process, steps out of the Dock and quits when the
// engine ends (with its exit status).  If the window is turned
// off and the game is ready it starts the engine right away (hold Option to see
// the window anyway).
//
// Environment (scripts and tests):
//   NFSU2_SKIP_LAUNCHER=1        start the engine without showing the window
//   NFSU2_ENGINE=original|hd     engine choice for this start
//   NFSU2_LAUNCHER_SNAPSHOT=x.png  draw the window into a PNG and quit (never shown)
//   NFSU2_LAUNCHER_STATE=ready|notfound|unsupported|embedded|copying|done   fake state for the snapshot
//   NFSU2_LAUNCHER_TEST=portable:<game folder>:<destination.app>   build a portable copy, quit
//   NFSU2_LAUNCHER_TEST=play     window off screen, press Play after a second (tests the
//                                normal path: window open -> Play -> engine)
//   NFSU2_LAUNCHER_TEST=mods:<game folder>:<id,id...>   build the data mods' files for
//                                these mods (turned on), quit
//   NFSU2_LAUNCHER_TEST=mods-download:<game folder>:<id>  download (checked), add, build, quit
//   NFSU2_LAUNCHER_TEST=mods-add:<game folder>:<id>:<file>  Add File... with this file, build, quit
//   NFSU2_MODS_STORE=<folder>    the data mods' folder (default ~/Library/Application Support/NFSU2/Mods)
import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model = LauncherModel()
    var window: NSWindow?
    let env = ProcessInfo.processInfo.environment

    func applicationDidFinishLaunching(_ note: Notification) {
        model.refreshNow()

        if let t = env["NFSU2_LAUNCHER_TEST"], t.hasPrefix("portable:") {
            let parts = t.dropFirst("portable:".count).split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { print("bad NFSU2_LAUNCHER_TEST"); exit(2) }
            model.startPortable(from: URL(fileURLWithPath: parts[0]), to: URL(fileURLWithPath: parts[1]))
            return
        }

        if let t = env["NFSU2_LAUNCHER_TEST"], t.hasPrefix("mods") {
            let parts = t.split(separator: ":", maxSplits: 3).map(String.init)
            guard parts.count >= 3, let exe = Locator.find(under: URL(fileURLWithPath: parts[1])) else {
                print("bad NFSU2_LAUNCHER_TEST"); exit(2)
            }
            model.game = Locator.check(GameLocation(exe: exe, embedded: false))
            let mods = DataMods.catalog.filter { parts[2].split(separator: ",").map(String.init).contains($0.id) }
            switch parts[0] {
            case "mods": model.modsOn = Set(mods.map { $0.id }); model.applyMods()
            case "mods-download" where mods.count == 1: model.downloadMod(mods[0])
            case "mods-add" where mods.count == 1 && parts.count == 4: model.addModFile(mods[0], URL(fileURLWithPath: parts[3]))
            default: print("bad NFSU2_LAUNCHER_TEST"); exit(2)
            }
            return
        }

        let snapshot = env["NFSU2_LAUNCHER_SNAPSHOT"]
        let option = NSEvent.modifierFlags.contains(.option)
        if snapshot == nil, model.isReady, !option,
           env["NFSU2_SKIP_LAUNCHER"] == "1" || !model.showAtStartup {
            model.play()
            if model.isPlaying { return }      // the engine runs; nothing to show
        }

        if let s = env["NFSU2_LAUNCHER_STATE"] { fakeState(s) }
        buildMenu()
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 788),
                         styleMask: [.titled, .closable, .miniaturizable],
                         backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.title = "Need for Speed Underground 2"
        w.isMovableByWindowBackground = true
        w.appearance = NSAppearance(named: .darkAqua)
        w.backgroundColor = NSColor(calibratedRed: 0.067, green: 0.071, blue: 0.082, alpha: 1)
        w.contentView = NSHostingView(rootView: LauncherView(model: model))
        w.center()
        w.delegate = self
        window = w

        if env["NFSU2_LAUNCHER_TEST"] == "play" {
            w.setFrameOrigin(NSPoint(x: -10000, y: -10000))
            w.orderBack(nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { self.model.play() }
            return
        }
        if let path = snapshot {
            w.setFrameOrigin(NSPoint(x: -10000, y: -10000))
            w.orderBack(nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
                guard let v = w.contentView, let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) else { exit(1) }
                v.cacheDisplay(in: v.bounds, to: rep)
                try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
                exit(0)
            }
            return
        }
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // Not while the engine runs: hiding the window on Play would otherwise quit us,
    // and open -W / session.sh would stop waiting for the game.
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { !model.isPlaying }
    // Closing the window while a copy is being made goes through the quit question.
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if model.isCopying { NSApp.terminate(nil); return false }
        return true
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        model.shouldTerminate()
    }

    private func fakeState(_ s: String) {
        let exe = URL(fileURLWithPath: "/Users/player/Games/Need for Speed Underground 2/SPEED2.EXE")
        switch s {
        case "ready": model.game = .ready(GameLocation(exe: exe, embedded: false))
        case "notfound": model.game = .notFound
        case "unsupported": model.game = .unsupported(GameLocation(exe: exe, embedded: false), 4_800_512)
        case "embedded": model.game = .ready(GameLocation(exe: exe, embedded: true))
        case "copying":
            model.job = LauncherModel.Job(phase: "Copying the game files…", done: 612_000_000, total: 1_430_000_000)
        case "mods":
            model.game = .ready(GameLocation(exe: exe, embedded: false))
            if let m = DataMods.catalog.first(where: { !model.modsHave.contains($0.id) }) {
                model.modBusy.insert(m.id)
                model.modNote[m.id] = "Downloading… \(formatBytes(m.size / 3)) of \(formatBytes(m.size))"
            }
        case "done":
            var j = LauncherModel.Job(phase: "", done: 1, total: 1)
            j.result = URL(fileURLWithPath: "/Applications/NFSU2 Portable.app")
            model.job = j
        default: break
        }
    }

    private func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About NFSU2", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide NFSU2", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit NFSU2", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        let winItem = NSMenuItem()
        main.addItem(winItem)
        let winMenu = NSMenu(title: "Window")
        winMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        winMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        winItem.submenu = winMenu
        NSApp.mainMenu = main
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
