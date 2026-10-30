// Launcher state and actions: find/check the game, start the engine, the tools.
import AppKit
import Darwin

final class LauncherModel: ObservableObject {
    enum Engine: String { case original, hd }

    struct Job {
        var phase: String
        var done: Int64 = 0
        var total: Int64 = 0
        var result: URL? = nil
        var error: String? = nil
        var fraction: Double { total > 0 ? min(1, Double(done) / Double(total)) : 0 }
    }

    @Published var game: GameState = .searching
    @Published var engine: Engine { didSet { defaults.set(engine.rawValue, forKey: Prefs.engine); engineForced = false } }
    private var engineForced = false     // NFSU2_ENGINE: this start only, not saved
    @Published var fullscreen: Bool { didSet { defaults.set(fullscreen, forKey: Prefs.fullscreen) } }
    @Published var showAtStartup: Bool { didSet { defaults.set(showAtStartup, forKey: Prefs.showAtStartup) } }
    @Published var wideHUD: Bool { didSet { defaults.set(wideHUD, forKey: Prefs.wideHUD) } }
    private var setHUDPOS = false        // HUDPOS was set by us (not by the caller)
    @Published var job: Job? = nil
    @Published var notice: String? = nil
    // Data mods (DataMods.swift): which are turned on, what each one's row says while
    // it is being fetched or failed, and the overlay build in progress.
    @Published var modsOn: Set<String> { didSet { defaults.set(Array(modsOn).sorted(), forKey: Prefs.dataMods) } }
    @Published var modNote: [String: String] = [:]
    @Published var modBusy: Set<String> = []
    @Published var modsApplying: String? = nil
    @Published private(set) var modsHave: Set<String> = []
    let modStore: URL
    private var modsPending = false, playAfterMods = false

    private let defaults = UserDefaults.standard
    private var builder: PortableBuilder?
    private var engineWatch: DispatchSourceProcess?
    var isPlaying: Bool { engineWatch != nil }
    var isCopying: Bool { builder != nil }
    private var quitWhenCopyStops = false

    /// Engines for the found SPEED2.EXE (European ones until it is known).
    func hasEngine(_ e: Engine) -> Bool {
        FileManager.default.isExecutableFile(atPath: Paths.engine(location?.build, hd: e == .hd).path)
    }
    var canChooseEngine: Bool { hasEngine(.original) && hasEngine(.hd) }

    init() {
        let fm = FileManager.default
        let hasOriginal = fm.isExecutableFile(atPath: Paths.engine(nil, hd: false).path)
        let hasHD = fm.isExecutableFile(atPath: Paths.engine(nil, hd: true).path)
        defaults.register(defaults: [Prefs.showAtStartup: true])
        var e = Engine(rawValue: defaults.string(forKey: Prefs.engine) ?? "") ?? .original
        if let env = ProcessInfo.processInfo.environment["NFSU2_ENGINE"], let f = Engine(rawValue: env) { e = f }
        if e == .hd && !hasHD { e = .original }
        if e == .original && !hasOriginal && hasHD { e = .hd }
        engine = e
        engineForced = ProcessInfo.processInfo.environment["NFSU2_ENGINE"] != nil
        fullscreen = defaults.bool(forKey: Prefs.fullscreen)
        showAtStartup = defaults.bool(forKey: Prefs.showAtStartup)
        wideHUD = defaults.bool(forKey: Prefs.wideHUD)
        modsOn = Set(defaults.stringArray(forKey: Prefs.dataMods) ?? [])
        let st = ProcessInfo.processInfo.environment["NFSU2_MODS_STORE"] ?? ""
        modStore = st.isEmpty ? DataMods.defaultStore : URL(fileURLWithPath: st)
        refreshMods()
    }

    var location: GameLocation? {
        switch game {
        case .ready(let l), .unsupported(let l, _): return l
        default: return nil
        }
    }
    var isReady: Bool { if case .ready = game { return true }; return false }

    /// Synchronous: used once at startup, before deciding whether to show the window.
    func refreshNow() {
        game = Locator.locate().map(Locator.check) ?? .notFound
    }

    func refresh() {
        game = .searching
        DispatchQueue.global(qos: .userInitiated).async {
            let s = Locator.locate().map(Locator.check) ?? .notFound
            DispatchQueue.main.async { self.game = s }
        }
    }

    // MARK: game folder

    func chooseFolder() {
        let p = NSOpenPanel()
        p.canChooseFiles = false
        p.canChooseDirectories = true
        p.allowsMultipleSelection = false
        p.prompt = "Choose"
        p.message = "Choose the Need for Speed Underground 2 folder (the one with SPEED2.EXE in it)."
        guard p.runModal() == .OK, let dir = p.url else { return }
        guard let exe = Locator.find(under: dir) else {
            alert("There is no SPEED2.EXE in this folder",
                  "Choose the folder where the game is installed. It contains SPEED2.EXE and folders such as CARS, TRACKS and SOUND.")
            return
        }
        defaults.set(exe.deletingLastPathComponent().path, forKey: Prefs.gameFolder)
        game = .searching
        DispatchQueue.global(qos: .userInitiated).async {
            let s = Locator.check(GameLocation(exe: exe, embedded: false))
            DispatchQueue.main.async { self.game = s }
        }
    }

    func revealGame() {
        guard let l = location else { return }
        NSWorkspace.shared.activateFileViewerSelecting([l.embedded ? Paths.bundle : l.exe])
    }

    // MARK: play

    func play() {
        guard case .ready(let loc) = game else { return }
        let env = ProcessInfo.processInfo.environment
        // Data mods: their files must be built for this game folder first.
        let mods = activeMods
        if modsApplying != nil || (!mods.isEmpty && !DataMods.isBuilt(mods, game: loc.folder, store: modStore)) {
            playAfterMods = true
            if modsApplying == nil { applyMods() }
            return
        }
        if !mods.isEmpty { setenv("NFSU2_MODS", DataMods.overlay(modStore).path, 0) }
        let fromEnv = !(env["NFSU2_DATA"] ?? "").isEmpty      // a one-off override: don't remember it
        if !loc.embedded && !fromEnv { defaults.set(loc.folder.path, forKey: Prefs.gameFolder) }
        if !engineForced { defaults.set(engine.rawValue, forKey: Prefs.engine) }
        defaults.synchronize()
        if !fromEnv { setenv("NFSU2_DATA", loc.exe.path, 1) }
        // HUD at the screen edges (engine: metal.m HUD ANCHORING).  A HUDPOS given by
        // the caller wins; otherwise it follows the switch.
        if setHUDPOS || env["HUDPOS"] == nil {
            if wideHUD { setenv("HUDPOS", "1", 1); setHUDPOS = true }
            else if setHUDPOS { unsetenv("HUDPOS"); setHUDPOS = false }
        }
        var e = engine
        if !hasEngine(e) { e = (e == .hd) ? .original : .hd }
        let path = Paths.engine(loc.build, hd: e == .hd).path
        // A child process, not exec(): macOS attributes the child's file access to
        // this launcher (the "responsible" process), so folder permissions the
        // player granted here (Downloads, Documents...) are not asked for again.
        var pid: pid_t = 0
        var argv: [UnsafeMutablePointer<CChar>?] = [strdup(path), nil]
        let rc = posix_spawn(&pid, path, nil, nil, &argv, environ)
        free(argv[0])
        guard rc == 0 else {
            alert("The game could not be started", "\(path)\n\n\(String(cString: strerror(rc)))")
            return
        }
        // When the engine ends, end with its exit status.  (Watch first: isPlaying
        // must be true before the window goes away.)
        let src = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit, queue: .main)
        src.setEventHandler {
            var st: Int32 = 0
            waitpid(pid, &st, 0)
            let code = (st & 0x7f) == 0 ? (st >> 8) & 0xff : 128 + (st & 0x7f)
            exit(code)
        }
        src.resume()
        engineWatch = src
        // Step aside: no window, no Dock icon of our own; the engine has its own.
        for w in NSApp.windows { w.orderOut(nil) }
        NSApp.setActivationPolicy(.accessory)
    }

    // MARK: data mods

    var activeMods: [DataMods.Mod] { DataMods.catalog.filter { modsOn.contains($0.id) && modsHave.contains($0.id) } }

    func refreshMods() {
        modsHave = Set(DataMods.catalog.filter { DataMods.source($0, modStore) != nil }.map { $0.id })
    }

    func setMod(_ m: DataMods.Mod, _ on: Bool) {
        if on { modsOn.insert(m.id) } else { modsOn.remove(m.id) }
        applyMods()
    }

    /// Download: from the mod's page, checked against the tested release; then turned on.
    func downloadMod(_ m: DataMods.Mod) {
        modBusy.insert(m.id)
        modNote[m.id] = "Downloading…"
        let store = modStore
        DispatchQueue.global(qos: .userInitiated).async {
            var err: String? = nil
            do {
                try DataMods.download(m, store: store) { n in
                    DispatchQueue.main.async {
                        self.modNote[m.id] = "Downloading… \(formatBytes(n)) of \(formatBytes(m.size))"
                    }
                }
            } catch let e as DataMods.Failure { err = e.message } catch { err = error.localizedDescription }
            DispatchQueue.main.async { self.modAdded(m, err) }
        }
    }

    /// Add File...: a copy of the mod the player already has (the archive or its folder).
    func importMod(_ m: DataMods.Mod) {
        let p = NSOpenPanel()
        p.canChooseFiles = true
        p.canChooseDirectories = true
        p.allowsMultipleSelection = false
        p.prompt = "Add"
        p.message = "Choose the \(m.name) archive from nfsmods.xyz (or the folder it was extracted to)."
        guard p.runModal() == .OK, let u = p.url else { return }
        addModFile(m, u)
    }

    func addModFile(_ m: DataMods.Mod, _ u: URL) {
        modBusy.insert(m.id)
        modNote[m.id] = "Adding…"
        let store = modStore, game = location?.folder ?? URL(fileURLWithPath: "/")
        DispatchQueue.global(qos: .userInitiated).async {
            var err: String? = nil
            do { try DataMods.importCopy(u, as: m, game: game, store: store) }
            catch let e as DataMods.Failure { err = e.message } catch { err = error.localizedDescription }
            DispatchQueue.main.async { self.modAdded(m, err) }
        }
    }

    private func modAdded(_ m: DataMods.Mod, _ err: String?) {
        modBusy.remove(m.id)
        refreshMods()
        if let e = err { modNote[m.id] = e } else { modNote[m.id] = nil; modsOn.insert(m.id); applyMods() }
        if ProcessInfo.processInfo.environment["NFSU2_LAUNCHER_TEST"] != nil {
            print(err.map { "mod \(m.id) FAILED: \($0)" } ?? "mod \(m.id) added")
            if err != nil { exit(1) }
        }
    }

    func removeMod(_ m: DataMods.Mod) {
        DataMods.remove(m, store: modStore)
        modNote[m.id] = nil
        modsOn.remove(m.id)
        refreshMods()
        applyMods()
    }

    func openModPage(_ m: DataMods.Mod) { NSWorkspace.shared.open(m.page) }

    /// (Re)builds the mod files for the turned-on mods, off the main thread.
    func applyMods() {
        guard let folder = location?.folder else { return }
        if modsApplying != nil { modsPending = true; return }
        let mods = activeMods, store = modStore
        if !mods.isEmpty && DataMods.isBuilt(mods, game: folder, store: store) { modsDone(nil); return }
        modsApplying = mods.isEmpty ? "Removing the mod files…" : "Applying mods…"
        DispatchQueue.global(qos: .userInitiated).async {
            var err: String? = nil
            do {
                try DataMods.build(mods, game: folder, store: store) { s in
                    DispatchQueue.main.async { self.modsApplying = s }
                }
            } catch let e as DataMods.Failure { err = e.message } catch { err = error.localizedDescription }
            DispatchQueue.main.async { self.modsDone(err) }
        }
    }

    private func modsDone(_ err: String?) {
        modsApplying = nil
        if let e = err {
            // the mod that failed is turned off again, with the reason in its row
            if let m = activeMods.first(where: { e.hasPrefix($0.name + ":") }) {
                modsOn.remove(m.id); modNote[m.id] = e
            } else { notice = "The mods could not be applied: \(e)" }
            playAfterMods = false
        }
        if modsPending { modsPending = false; applyMods(); return }
        if ProcessInfo.processInfo.environment["NFSU2_LAUNCHER_TEST"]?.hasPrefix("mods") == true {
            print(err.map { "mods FAILED: \($0)" } ?? "mods applied: \(activeMods.map { $0.id }.joined(separator: ","))")
            exit(err == nil ? 0 : 1)
        }
        if playAfterMods && err == nil { playAfterMods = false; play() }
    }

    // MARK: tools

    func openSaves() {
        try? FileManager.default.createDirectory(at: Paths.saves, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Paths.saves)
    }

    func openLogs() {
        try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Paths.logs)
    }

    func reportProblem() {
        NSWorkspace.shared.open(URL(string: "https://github.com/TalhaMudo/NFSU2Recomp/issues")!)
    }

    func resetGraphics() {
        let a = NSAlert()
        a.messageText = "Reset the graphics settings?"
        a.informativeText = "The next time it starts, the game picks graphics settings for this Mac again, as on its first start. Career saves are not affected."
        a.addButton(withTitle: "Reset")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        do { try Registry.resetGraphics(); notice = "Graphics settings reset." }
        catch { alert("Could not reset the settings", error.localizedDescription) }
    }

    // MARK: portable copy

    func makePortable() {
        guard case .ready(let loc) = game else { return }
        let p = NSSavePanel()
        p.title = "Make a Portable Copy"
        p.message = "A copy of this app with the game files inside. It runs on its own, without the separate game folder. It contains the game, so keep it for yourself."
        p.prompt = "Make Copy"
        p.nameFieldStringValue = "NFSU2 Portable.app"
        p.canCreateDirectories = true
        p.isExtensionHidden = false
        p.directoryURL = FileManager.default.isWritableFile(atPath: "/Applications")
            ? URL(fileURLWithPath: "/Applications")
            : FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first
        guard p.runModal() == .OK, var dest = p.url else { return }
        if dest.pathExtension.lowercased() != "app" { dest.appendPathExtension("app") }
        if dest.standardizedFileURL == Paths.bundle.standardizedFileURL {
            alert("Choose another name", "The copy can't replace the app that is running.")
            return
        }
        startPortable(from: loc.folder, to: dest)
    }

    func startPortable(from game: URL, to dest: URL) {
        let b = PortableBuilder(sourceApp: Paths.bundle, gameFolder: game, destination: dest)
        builder = b
        job = Job(phase: "Starting…")
        DispatchQueue.global(qos: .userInitiated).async {
            var res: URL? = nil, err: String? = nil
            do {
                res = try b.build { pr in
                    DispatchQueue.main.async {
                        guard self.job?.result == nil, self.job?.error == nil else { return }
                        self.job = Job(phase: pr.phase, done: pr.done, total: pr.total)
                    }
                }
            } catch PortableBuilder.Failure.cancelled {
                err = nil
            } catch PortableBuilder.Failure.noSpace(let need, let free) {
                err = "Not enough free space: \(formatBytes(need)) needed, \(formatBytes(free)) free."
            } catch PortableBuilder.Failure.step(let s) {
                err = s
            } catch {
                err = error.localizedDescription
            }
            DispatchQueue.main.async {
                self.builder = nil
                if self.quitWhenCopyStops { NSApp.reply(toApplicationShouldTerminate: true); return }
                if res == nil && err == nil { self.job = nil; return }    // cancelled
                var j = self.job ?? Job(phase: "")
                j.result = res; j.error = err
                if res != nil { j.done = j.total }
                self.job = j
                if let r = res, ProcessInfo.processInfo.environment["NFSU2_LAUNCHER_TEST"] != nil {
                    print("portable copy: \(r.path)"); NSApp.terminate(nil)
                }
                if let e = err, ProcessInfo.processInfo.environment["NFSU2_LAUNCHER_TEST"] != nil {
                    print("portable copy FAILED: \(e)"); exit(1)
                }
            }
        }
    }

    func cancelPortable() { builder?.cancel() }

    /// Quitting while a copy is being made: stop it (the half-made copy is
    /// removed) and quit once the copy thread has cleaned up.
    func shouldTerminate() -> NSApplication.TerminateReply {
        guard isCopying else { return .terminateNow }
        let a = NSAlert()
        a.messageText = "A portable copy is being made"
        a.informativeText = "Quitting stops it; the unfinished copy is removed."
        a.addButton(withTitle: "Keep Copying")
        a.addButton(withTitle: "Stop and Quit")
        guard a.runModal() == .alertSecondButtonReturn else { return .terminateCancel }
        quitWhenCopyStops = true
        builder?.cancel()
        return .terminateLater
    }
    func closeJob() { job = nil }
    func revealResult() {
        if let r = job?.result { NSWorkspace.shared.activateFileViewerSelecting([r]) }
        job = nil
    }

    func alert(_ title: String, _ text: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        a.addButton(withTitle: "OK")
        a.runModal()
    }
}
