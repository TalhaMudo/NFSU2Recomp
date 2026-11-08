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
//   NFSU2_LAUNCHER_SNAPSHOT_SETTINGS=graphics|more|...  with the above: the Settings window on that tab
//   NFSU2_LAUNCHER_STATE=ready|notfound|unsupported|embedded|copying|done   fake state for the snapshot
//   NFSU2_LAUNCHER_TEST=portable:<game folder>:<destination.app>   build a portable copy, quit
//                                (NFSU2_LAUNCHER_TEST_LEAVE_OUT=MOVIES,LANGPACKS: without those)
//   NFSU2_LAUNCHER_TEST=play     window off screen, press Play after a second (tests the
//                                normal path: window open -> Play -> engine)
//   NFSU2_LAUNCHER_TEST=mods:<game folder>:<step>;<step>...   Settings › Mods without the
//                                window, through the same code (Mods.swift), then quit:
//                                add=<archive or folder>  ul-add=<file>  ul-download
//                                on=<id>  off=<id>  remove=<id>  restore (every mod off)
//                                choose=<id>|<question text>|<answer>
//                                A step starting with "!" must fail.  Prints each mod and
//                                the built files; exit 1 if a step went the other way.
//   NFSU2_MODS_STORE=<folder>    where the mods are kept (default Application Support/NFSU2/Mods)
//   NFSU2_MODINSTALL=<file>      the mod installer (default: in the app, or out/binarius/modinstall)
//   NFSU2_UL_URL=<url>           Unlimiter's Download from here (file:// for tests)
import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model = LauncherModel()
    var window: NSWindow?
    var settingsWindow: NSWindow?
    let env = ProcessInfo.processInfo.environment

    /// NFSU2_LAUNCHER_TEST=mods:...: one step at a time, each when the previous one is done.
    func runModSteps(_ steps: [String], failed: Bool) {
        guard let first = steps.first else {
            for m in model.mods {
                print("mod \(m.id): \(m.on ? "on" : "off"), \(m.kind.rawValue), \(m.name)"
                      + (m.prompts.isEmpty ? "" : ", " + m.prompts.map { "\($0.description) -> \($0.choice)" }.joined(separator: "; "))
                      + (m.issues.isEmpty ? "" : ", \(m.issues.count) issue(s)")
                      + (model.modNote[m.id].map { " [\($0)]" } ?? ""))
            }
            let files = Mods.allFiles(Mods.files).map { Mods.relative($0, to: Mods.files) }.filter { $0 != ".deleted" }
            let del = (try? String(contentsOf: Mods.files.appendingPathComponent(".deleted"), encoding: .utf8))?
                .split(separator: "\n").count ?? 0
            let built = model.location.map { Mods.isBuilt(model.activeMods, game: $0) } ?? false
            print("files: \(files.count) (\(files.prefix(6).joined(separator: ", "))\(files.count > 6 ? ", ..." : "")), removed: \(del), up to date: \(built)")
            exit(failed ? 1 : 0)
        }
        let rest = Array(steps.dropFirst())
        let mustFail = first.hasPrefix("!")
        let step = mustFail ? String(first.dropFirst()) : first
        let kv = step.split(separator: "=", maxSplits: 1).map(String.init)
        let arg = kv.count > 1 ? kv[1] : ""
        let next: (String?) -> Void = { err in
            print("step \(step): " + (err.map { "failed: \($0)" } ?? "ok"))
            self.runModSteps(rest, failed: failed || (err != nil) != mustFail)
        }
        func mod(_ id: String) -> InstalledMod? { model.mods.first { $0.id == id } }
        switch kv[0] {
        case "add": model.addModFile(URL(fileURLWithPath: arg), done: next)
        case "ul-add": model.addModFile(URL(fileURLWithPath: arg), row: Mods.Unlimiter.id, done: next)
        case "ul-download": model.downloadUnlimiter(done: next)
        case "on", "off":
            guard let m = mod(arg) else { next("no mod \(arg)"); return }
            model.modsDoneHook = next; model.setMod(m, kv[0] == "on")
        case "remove":
            guard let m = mod(arg) else { next("no mod \(arg)"); return }
            model.modsDoneHook = next; model.removeMod(m)
        case "restore":
            model.modsDoneHook = next; model.restoreOriginalGame()
        case "choose":
            let p = arg.split(separator: "|", maxSplits: 2).map(String.init)
            guard p.count == 3, let m = mod(p[0]), let q = m.prompts.first(where: { $0.description.contains(p[1]) }) else {
                next("no such question"); return }
            model.modsDoneHook = next; model.setChoice(m, q, p[2])
        default: print("bad step \(step)"); exit(2)
        }
    }

    func applicationDidFinishLaunching(_ note: Notification) {
        model.refreshNow()

        if let t = env["NFSU2_LAUNCHER_TEST"], t.hasPrefix("portable:") {
            let parts = t.dropFirst("portable:".count).split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { print("bad NFSU2_LAUNCHER_TEST"); exit(2) }
            model.startPortable(from: URL(fileURLWithPath: parts[0]), to: URL(fileURLWithPath: parts[1]),
                                leaveOut: Set((env["NFSU2_LAUNCHER_TEST_LEAVE_OUT"] ?? "")
                                                .split(separator: ",").map(String.init)))
            return
        }

        if let t = env["NFSU2_LAUNCHER_TEST"], t.hasPrefix("mods:") {
            let parts = t.dropFirst("mods:".count).split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let exe = Locator.find(under: URL(fileURLWithPath: parts[0])) else {
                print("bad NFSU2_LAUNCHER_TEST"); exit(2)
            }
            model.game = Locator.check(GameLocation(exe: exe, embedded: false))
            runModSteps(parts[1].split(separator: ";").map(String.init), failed: false)
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
        let w = NSWindow(contentRect: NSRect(origin: .zero, size: LauncherView.size),
                         styleMask: [.titled, .closable, .miniaturizable],
                         backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.title = "Need for Speed Underground 2"
        w.isMovableByWindowBackground = true
        w.appearance = NSAppearance(named: .darkAqua)
        w.backgroundColor = NSColor(calibratedRed: 0.067, green: 0.071, blue: 0.082, alpha: 1)
        w.contentView = NSHostingView(rootView: LauncherView(model: model) { [weak self] in self?.showSettings(nil) })
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
            var shot = w
            if let name = env["NFSU2_LAUNCHER_SNAPSHOT_SETTINGS"] {
                guard let t = SettingsTab(rawValue: name), SettingsTab.shown.contains(t) else {
                    print("NFSU2_LAUNCHER_SNAPSHOT_SETTINGS: one of " + SettingsTab.shown.map(\.rawValue).joined(separator: ", "))
                    exit(2)
                }
                shot = makeSettingsWindow(model: model, tab: t)
            }
            shot.setFrameOrigin(NSPoint(x: -10000, y: -10000))
            shot.orderBack(nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
                guard let v = shot.contentView, let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) else { exit(1) }
                v.cacheDisplay(in: v.bounds, to: rep)
                try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
                exit(0)
            }
            return
        }
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    /// Settings… in the window and the app menu (Cmd-,).
    @objc func showSettings(_ sender: Any?) {
        guard !model.isPlaying else { return }
        if settingsWindow == nil { settingsWindow = makeSettingsWindow(model: model, tab: SettingsTab.shown[0]) }
        settingsWindow?.makeKeyAndOrderFront(nil)
    }

    // Not while the engine runs: hiding the window on Play would otherwise quit us,
    // and open -W / session.sh would stop waiting for the game.
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { !model.isPlaying }
    // Closing the window while a copy is being made goes through the quit question.
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if model.isCopying { NSApp.terminate(nil); return false }
        return true
    }
    // Settings goes with the main window (and the app with them).
    func windowWillClose(_ note: Notification) { settingsWindow?.close() }

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
        appMenu.addItem(withTitle: "Settings…", action: #selector(showSettings(_:)), keyEquivalent: ",").target = self
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
