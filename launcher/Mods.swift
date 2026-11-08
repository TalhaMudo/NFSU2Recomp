// Settings › Mods: PC mods for the game's data, and NFSU2 Unlimiter.
//
// What a player adds (Add Mod…: an archive or a folder) is one of
//   - a Binary / Binarius mod: a launch file ("[VERSN1]" + JSON; install.end,
//     *.endlauncher) that names an endscript and the game files it edits.  It is run
//     with modinstall (tools/mods/binarius: Binarius' own libraries, no window);
//   - plain game files: folders such as TRACKS, GLOBAL, CARS, copied over the game's.
// A mod with a plugin (.asi) is taken only when the engine has that plugin's code
// (every plugin has to be translated ahead of time; NFSU2 Unlimiter 4.0.0.802 is, in
// the North American engine).  Unlimiter has a row of its own with Download.
//
// The game folder is never changed.  Applying the turned-on mods:
//   1. the game folder is cloned (APFS: instant, no space) into a temporary folder
//      on its own volume;
//   2. the mods are installed into the clone, Unlimiter first, then the others in the
//      order they were added; a script mod runs on a fresh copy of itself (scripts
//      move their files out of it);
//   3. the files that differ from the game's (size or date) move to
//      ~/Library/Application Support/NFSU2/Mods/Files at the same paths, the game
//      files that are gone are listed in Files/.deleted, the clone is deleted.
// The engine reads Files before the game folder (NFSU2_MODS, runtime/fileio.c).
// A stamp (the mods, their answers, the game's SPEED2.EXE) says whether Files is up
// to date; Play applies first when it isn't.
import AppKit
import CryptoKit
import Foundation

/// A question a mod's script asked while it was installed (modinstall --report).
struct ModPrompt: Codable, Equatable {
    var kind: String            // "combobox" | "checkbox"
    var description: String
    var options: [String]
    var choice: String
    var auto: Bool
}

struct ModIssue: Codable, Equatable {
    var file: String?
    var line: Int?
    var command: String?
    var message: String?
}

struct ModReport: Codable {
    var saved: Bool
    var attempts: Int?
    var failure: String?
    var prompts: [ModPrompt]
    var errors: [ModIssue]
}

struct InstalledMod: Codable, Equatable, Identifiable {
    enum Kind: String, Codable { case script, files }
    var id: String              // its folder in Mods/Library
    var name: String
    var source: String          // the archive or folder it was added from (its name)
    var kind: Kind
    var path: String            // script: the launch file; files: the folder holding TRACKS, GLOBAL...
    var plugin: String?         // the .asi it brings (only ones the engine has code for)
    var on: Bool = true
    var choices: [String: String] = [:]     // question -> answer, set by the player
    var prompts: [ModPrompt] = []           // as asked in the last install
    var issues: [ModIssue] = []             // errors the last install reported (it still saved)
}

enum Mods {
    struct Failure: Error { let message: String }

    // MARK: Unlimiter

    /// NFSU2 Unlimiter: the release the engine's plugin code was translated from.
    enum Unlimiter {
        static let id = "unlimiter"
        static let name = "NFSU2 Unlimiter"
        static let author = "nlgxzef"
        static let version = "4.0.0.802"
        static let page = URL(string: "https://github.com/nlgxzef/NFSU2Unlimiter")!
        static var download: URL {
            if let u = ProcessInfo.processInfo.environment["NFSU2_UL_URL"], let url = URL(string: u) { return url }  // tests
            return URL(string: "https://github.com/nlgxzef/NFSU2Unlimiter/releases/download/v4.0.0.0802/NFSU2-ULv4.0.0.802.zip")!
        }
        static let size: Int64 = 4_634_978
        static let zipSHA256 = "d4b1c782a2db7ba4f3ce9ee5a091a971b01d0919a99042d780987ae23ae877ad"
        static let plugin = "NFSU2Unlimiter.asi"
        static let pluginSHA256 = "513a0b841da4d47268f677f9714e0039479c1aa32e6ad113b8ec92526ebd284d"
        /// Its Read Me's recommendations; the launcher always installs fresh.
        static let presets: [String: String] = [
            "Please choose what to do.": "New Installation",
            "Please choose which paint groups to use.": "Enhanced",
            "Please choose which rim brands to use.": "Enhanced",
            "Fix wheel texture attributes?": "yes",
            "Do you have a tire mod installed?": "no",
        ]
        /// Not offered to the player: the launcher installs onto an untouched clone.
        static let hidden = ["Please choose what to do."]
    }

    /// Plugins the engines have code for (runtime/asi.c), by file name -> SHA-256.
    static let knownPlugins = [Unlimiter.plugin.lowercased(): Unlimiter.pluginSHA256]

    /// Folders of the game a plain-file mod is made of.
    static let gameFolders: Set<String> = ["global", "tracks", "cars", "frontend", "languages", "sound",
                                           "movies", "scripts"]

    // MARK: places

    static var store: URL {
        if let s = ProcessInfo.processInfo.environment["NFSU2_MODS_STORE"], !s.isEmpty { return URL(fileURLWithPath: s) }
        return Paths.home.appendingPathComponent("Library/Application Support/NFSU2/Mods")
    }
    static var library: URL { store.appendingPathComponent("Library") }
    static var files: URL { store.appendingPathComponent("Files") }
    static var index: URL { store.appendingPathComponent("mods.json") }
    static var stampFile: URL { store.appendingPathComponent("built.json") }

    /// modinstall: next to the launcher in the app, out/binarius/modinstall when run from out/.
    static var installer: URL? {
        let fm = FileManager.default
        var c: [URL] = []
        if let e = ProcessInfo.processInfo.environment["NFSU2_MODINSTALL"], !e.isEmpty { c.append(URL(fileURLWithPath: e)) }
        c.append(Paths.resources.appendingPathComponent("modinstall/modinstall"))
        let exeDir = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        c.append(exeDir.appendingPathComponent("binarius/modinstall/modinstall"))
        return c.first { fm.isExecutableFile(atPath: $0.path) }
    }

    /// Does this engine have Unlimiter's code?  (runtime/asi.c names the plugin file.)
    static func engineHasUnlimiter(_ engine: URL) -> Bool {
        guard let d = try? Data(contentsOf: engine, options: .alwaysMapped) else { return false }
        return d.range(of: Data(Unlimiter.pluginSHA256.utf8)) != nil
    }

    // MARK: list

    static func load() -> [InstalledMod] {
        guard let d = try? Data(contentsOf: index),
              let l = try? JSONDecoder().decode([InstalledMod].self, from: d) else { return [] }
        return l.filter { FileManager.default.fileExists(atPath: library.appendingPathComponent($0.id).path) }
    }

    static func save(_ mods: [InstalledMod]) {
        try? FileManager.default.createDirectory(at: store, withIntermediateDirectories: true)
        let e = JSONEncoder(); e.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let d = try? e.encode(mods) { try? d.write(to: index, options: .atomic) }
    }

    // MARK: adding

    /// Unpacks (or copies) a mod into the library and works out what it is.  The
    /// player's file or folder is only read.
    static func add(_ src: URL, existing: [InstalledMod]) throws -> InstalledMod {
        let fm = FileManager.default
        try fm.createDirectory(at: library, withIntermediateDirectories: true)
        let staging = store.appendingPathComponent("Adding-" + UUID().uuidString)
        defer { try? fm.removeItem(at: staging) }
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: src.path, isDirectory: &isDir) else { throw Failure(message: "\(src.lastPathComponent) is not there any more.") }
        if isDir.boolValue {
            try fm.copyItem(at: src, to: staging)
        } else {
            try fm.createDirectory(at: staging, withIntermediateDirectories: true)
            // macOS's bsdtar: zip (also LZMA), 7z, rar, tar.  It refuses entries with ".." or absolute paths.
            let (rc, out) = run(URL(fileURLWithPath: "/usr/bin/tar"), ["-xf", src.path, "-C", staging.path])
            if rc != 0 {
                throw Failure(message: "\(src.lastPathComponent) could not be opened as an archive (zip, 7z, rar). \(out.split(separator: "\n").first ?? "")")
            }
        }
        // nothing that points outside: no symbolic links at all
        if let e = fm.enumerator(at: staging, includingPropertiesForKeys: [.isSymbolicLinkKey]) {
            for case let u as URL in e where (try? u.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true {
                throw Failure(message: "\(src.lastPathComponent) contains a symbolic link (\(u.lastPathComponent)); it is not added.")
            }
        }
        let all = allFiles(staging)

        // plugins: only ones the engine has code for
        var plugin: String? = nil
        for u in all where u.pathExtension.lowercased() == "asi" {
            let name = u.lastPathComponent
            guard let want = knownPlugins[name.lowercased()] else {
                throw Failure(message: "It brings the plugin \(name). Plugins are Windows code and each one has to be translated for the Mac first; this version has NFSU2 Unlimiter \(Unlimiter.version) only.")
            }
            guard sha256(u) == want else {
                throw Failure(message: "It brings \(name) in another version than \(Unlimiter.version), the one this Mac version has the code for.")
            }
            plugin = name
        }

        // a launch file, the shallowest one (an .endlauncher before an .end): an
        // .endlauncher, or an .end that is "[VERSN1]" + JSON or the JSON alone (an
        // .end that is a script starts with "[VERSN2]" or a command)
        let launches = all.filter { u in
            let ext = u.pathExtension.lowercased()
            if ext == "endlauncher" { return true }
            guard ext == "end", let h = try? FileHandle(forReadingFrom: u) else { return false }
            defer { try? h.close() }
            let head = String(decoding: h.readData(ofLength: 64), as: UTF8.self)
                .trimmingCharacters(in: CharacterSet.whitespacesAndNewlines.union(CharacterSet(charactersIn: "\u{FEFF}")))
            return head.uppercased().hasPrefix("[VERSN1]") || head.hasPrefix("{")
        }.sorted { a, b in
            let da = a.pathComponents.count, db = b.pathComponents.count
            if da != db { return da < db }
            return (a.pathExtension.lowercased() == "endlauncher") && b.pathExtension.lowercased() != "endlauncher"
        }
        var kind = InstalledMod.Kind.files
        var path = ""
        if let l = launches.first {
            if launches.count > 1, launches[1].pathComponents.count == l.pathComponents.count,
               launches[1].pathExtension.lowercased() == l.pathExtension.lowercased() {
                throw Failure(message: "It has more than one install script (\(l.lastPathComponent), \(launches[1].lastPathComponent)): add the folder of the one you want.")
            }
            kind = .script
            path = relative(l, to: staging)
            // Windows paths in the scripts: "\" -> "/" (Binarius on Windows takes both)
            for u in all where ["end", "endscript", "endlauncher"].contains(u.pathExtension.lowercased()) {
                if var t = try? String(contentsOf: u, encoding: .utf8), t.contains("\\") {
                    t = t.replacingOccurrences(of: "\\", with: "/")
                    try t.write(to: u, atomically: true, encoding: .utf8)
                }
            }
        } else if let root = gameTreeRoot(staging) {
            path = relative(root, to: staging)
        } else {
            throw Failure(message: "This doesn't look like a mod for this game: it has no install.end (Binary) or .endlauncher (Binarius) script, and no game folders such as TRACKS, GLOBAL or CARS.")
        }

        // a name: Unlimiter's own, else the archive's or folder's
        var name = src.deletingPathExtension().lastPathComponent
        var id = name.lowercased().filter { $0.isLetter || $0.isNumber }
        if plugin?.lowercased() == Unlimiter.plugin.lowercased() { name = Unlimiter.name; id = Unlimiter.id }
        if id.isEmpty { id = "mod" }
        if id != Unlimiter.id {
            let base = id; var n = 2
            while existing.contains(where: { $0.id == id }) { id = "\(base)\(n)"; n += 1 }
        }
        let dest = library.appendingPathComponent(id)
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: staging, to: dest)
        var m = InstalledMod(id: id, name: name, source: src.lastPathComponent, kind: kind, path: path, plugin: plugin)
        if id == Unlimiter.id { m.choices = Unlimiter.presets }
        return m
    }

    static func remove(_ m: InstalledMod) {
        try? FileManager.default.removeItem(at: library.appendingPathComponent(m.id))
    }

    /// Download: Unlimiter's release from GitHub, checked, then added.
    static func downloadUnlimiter(progress: @escaping (Int64) -> Void) throws -> URL {
        let fm = FileManager.default
        try fm.createDirectory(at: store, withIntermediateDirectories: true)
        let dest = store.appendingPathComponent("NFSU2-ULv\(Unlimiter.version).zip")
        let sem = DispatchSemaphore(value: 0)
        var failure: String? = nil
        let task = URLSession.shared.downloadTask(with: Unlimiter.download) { loc, resp, err in
            defer { sem.signal() }
            if let err = err { failure = err.localizedDescription; return }
            if let h = resp as? HTTPURLResponse, h.statusCode != 200 { failure = "the server answered \(h.statusCode)"; return }
            guard let loc = loc else { failure = "nothing was downloaded"; return }
            try? fm.removeItem(at: dest)
            do { try fm.moveItem(at: loc, to: dest) } catch { failure = error.localizedDescription }
        }
        let obs = task.progress.observe(\.completedUnitCount) { p, _ in progress(p.completedUnitCount) }
        task.resume()
        sem.wait()
        obs.invalidate()
        if let f = failure { throw Failure(message: "The download failed: \(f).") }
        guard sha256(dest) == Unlimiter.zipSHA256 else {
            try? fm.removeItem(at: dest)
            throw Failure(message: "The file on GitHub is not the \(Unlimiter.version) release. Download it yourself and use Add File….")
        }
        return dest
    }

    // MARK: applying

    /// What the built files are for: the turned-on mods, their answers, the game.
    static func stamp(_ mods: [InstalledMod], game: GameLocation) -> String {
        let a = (try? FileManager.default.attributesOfItem(atPath: game.exe.path)) ?? [:]
        var s = "\(game.exe.path) \((a[.size] as? NSNumber)?.int64Value ?? -1) \(Int((a[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0))\n"
        // a new installer (an update of the app) installs the mods again
        if let i = installer, let d = (try? FileManager.default.attributesOfItem(atPath: i.path))?[.modificationDate] as? Date {
            s += "installer \(Int(d.timeIntervalSince1970))\n"
        }
        for m in ordered(mods) {
            s += "\(m.id) \(m.kind.rawValue) \(m.path) \(m.choices.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: "|"))\n"
        }
        return s
    }

    static func isBuilt(_ mods: [InstalledMod], game: GameLocation) -> Bool {
        guard let s = try? String(contentsOf: stampFile, encoding: .utf8) else { return mods.isEmpty }
        return s == stamp(mods, game: game) && FileManager.default.fileExists(atPath: files.path)
    }

    /// Plugin mods first (other mods may build on them), then the order they were added.
    static func ordered(_ mods: [InstalledMod]) -> [InstalledMod] {
        mods.filter { $0.plugin != nil } + mods.filter { $0.plugin == nil }
    }

    /// Builds Files for the turned-on mods (none: removes it).  Returns each script
    /// mod's report.  On a failure the message starts with the mod's name.
    static func build(_ on: [InstalledMod], game: GameLocation,
                      status: @escaping (String) -> Void) throws -> [String: ModReport] {
        let fm = FileManager.default
        try fm.createDirectory(at: store, withIntermediateDirectories: true)
        try? fm.removeItem(at: files)
        try? fm.removeItem(at: stampFile)
        if on.isEmpty { return [:] }
        guard let installer = installer else {
            throw Failure(message: "The mod installer (modinstall) is missing from the app.")
        }

        // 1. the clone, on the game's own volume when possible (instant, takes no space)
        let work: URL
        if let w = try? fm.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: game.folder, create: true) {
            work = w
        } else {
            work = store.appendingPathComponent("Work"); try? fm.removeItem(at: work)
            try fm.createDirectory(at: work, withIntermediateDirectories: true)
        }
        defer { try? fm.removeItem(at: work) }
        let clone = work.appendingPathComponent("Game")
        status("Preparing a copy of the game files…")
        try fm.copyItem(at: game.folder, to: clone)

        // 2. the mods
        var reports: [String: ModReport] = [:]
        for m in ordered(on) {
            status("Installing \(m.name)…")
            let src = library.appendingPathComponent(m.id)
            switch m.kind {
            case .files:
                // only the game's folders: a Readme or preview picture next to them stays out
                let root = src.appendingPathComponent(m.path)
                for d in (try fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil))
                    where gameFolders.contains(d.lastPathComponent.lowercased()) {
                    let existing = (try? fm.contentsOfDirectory(atPath: clone.path))?
                        .first { $0.caseInsensitiveCompare(d.lastPathComponent) == .orderedSame }
                    let target = clone.appendingPathComponent(existing ?? d.lastPathComponent)
                    try fm.createDirectory(at: target, withIntermediateDirectories: true)
                    try merge(d, into: target)
                }
            case .script:
                let copy = work.appendingPathComponent("Mod-" + m.id)
                try fm.copyItem(at: src, to: copy)
                let launch = copy.appendingPathComponent(m.path)
                let report = work.appendingPathComponent("report-\(m.id).json")
                var args = [launch.path, clone.path, "--report", report.path]
                for (q, a) in m.choices { args += ["--choose", "\(q)=\(a)"] }
                let (rc, out) = run(installer, args, cwd: launch.deletingLastPathComponent())
                let r = (try? Data(contentsOf: report)).flatMap { try? JSONDecoder().decode(ModReport.self, from: $0) }
                guard rc == 0, let rep = r, rep.saved else {
                    let why = r?.failure ?? r?.errors.first?.message ?? out.split(separator: "\n").last.map(String.init) ?? "exit \(rc)"
                    throw Failure(message: "\(m.name): \(why)")
                }
                reports[m.id] = rep
                try? fm.removeItem(at: copy)
            }
        }

        // 3. what changed
        status("Collecting the changed files…")
        let next = store.appendingPathComponent("Files-new")
        try? fm.removeItem(at: next)
        try fm.createDirectory(at: next, withIntermediateDirectories: true)
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey, .contentModificationDateKey]
        func info(_ u: URL) -> (Int, Date)? {
            guard let v = try? u.resourceValues(forKeys: Set(keys)), v.isRegularFile == true else { return nil }
            return (v.fileSize ?? -1, v.contentModificationDate ?? .distantPast)
        }
        // game files no mod left in place (before the changed ones move out of the clone)
        var deleted: [String] = []
        for u in allFiles(game.folder) {
            let rel = relative(u, to: game.folder)
            if !fm.fileExists(atPath: clone.appendingPathComponent(rel).path) { deleted.append(rel) }
        }
        var changed = 0
        for u in allFiles(clone) {
            let rel = relative(u, to: clone)
            let now = info(u), was = info(game.folder.appendingPathComponent(rel))
            if let n = now, let w = was, n.0 == w.0, n.1 == w.1 { continue }
            let to = next.appendingPathComponent(rel)
            try fm.createDirectory(at: to.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.moveItem(at: u, to: to)
            changed += 1
        }
        if !deleted.isEmpty {
            try deleted.joined(separator: "\n").appending("\n").write(to: next.appendingPathComponent(".deleted"), atomically: true, encoding: .utf8)
        }
        try fm.moveItem(at: next, to: files)
        try stamp(on, game: game).write(to: stampFile, atomically: true, encoding: .utf8)
        status("\(changed) changed files")
        return reports
    }

    // MARK: helpers

    /// Copies a plain-file mod's folders over the clone; names match the game's
    /// whatever their case (TRACKS/StreamL4RA.BUN replaces TRACKS/STREAML4RA.BUN).
    static func merge(_ src: URL, into dst: URL) throws {
        let fm = FileManager.default
        for item in (try fm.contentsOfDirectory(at: src, includingPropertiesForKeys: [.isDirectoryKey])) {
            let name = item.lastPathComponent
            if name.hasPrefix(".") { continue }
            let existing = (try? fm.contentsOfDirectory(atPath: dst.path))?.first { $0.caseInsensitiveCompare(name) == .orderedSame }
            let target = dst.appendingPathComponent(existing ?? name)
            if (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                try fm.createDirectory(at: target, withIntermediateDirectories: true)
                try merge(item, into: target)
            } else {
                try? fm.removeItem(at: target)
                try fm.copyItem(at: item, to: target)
            }
        }
    }

    /// The folder holding the game folders (TRACKS, GLOBAL...) of a plain-file mod,
    /// at most three levels down.
    static func gameTreeRoot(_ dir: URL, depth: Int = 0) -> URL? {
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey]) else { return nil }
        let dirs = items.filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
        if dirs.contains(where: { gameFolders.contains($0.lastPathComponent.lowercased()) }) { return dir }
        if depth >= 3 { return nil }
        for d in dirs { if let r = gameTreeRoot(d, depth: depth + 1) { return r } }
        return nil
    }

    static func allFiles(_ dir: URL) -> [URL] {
        guard let e = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: [.isRegularFileKey]) else { return [] }
        var out: [URL] = []
        for case let u as URL in e where (try? u.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { out.append(u) }
        return out
    }

    static func relative(_ u: URL, to base: URL) -> String {
        let b = base.standardizedFileURL.resolvingSymlinksInPath().path, p = u.standardizedFileURL.resolvingSymlinksInPath().path
        if p == b { return "" }
        return p.hasPrefix(b + "/") ? String(p.dropFirst(b.count + 1)) : u.lastPathComponent
    }

    static func sha256(_ u: URL) -> String {
        guard let d = try? Data(contentsOf: u, options: .alwaysMapped) else { return "" }
        return SHA256.hash(data: d).map { String(format: "%02x", $0) }.joined()
    }

    /// Runs a tool, returns its exit status and output.
    static func run(_ exe: URL, _ args: [String], cwd: URL? = nil) -> (Int32, String) {
        let p = Process()
        p.executableURL = exe
        p.arguments = args
        if let c = cwd { p.currentDirectoryURL = c }
        let pipe = Pipe()
        p.standardOutput = pipe; p.standardError = pipe
        var data = Data()
        let q = DispatchQueue(label: "mods.read")
        pipe.fileHandleForReading.readabilityHandler = { h in let d = h.availableData; q.sync { data.append(d) } }
        do { try p.run() } catch { return (-1, error.localizedDescription) }
        p.waitUntilExit()
        pipe.fileHandleForReading.readabilityHandler = nil
        let rest = pipe.fileHandleForReading.readDataToEndOfFile()
        return q.sync { data.append(rest); return (p.terminationStatus, String(decoding: data, as: UTF8.self)) }
    }
}

// MARK: - the launcher's side

extension LauncherModel {
    /// The ids of the turned-on mods.  Restore Original Game sets it to [].
    var modsOn: Set<String> {
        get { Set(mods.filter { $0.on }.map { $0.id }) }
        set {
            for i in mods.indices { mods[i].on = newValue.contains(mods[i].id) }
            Mods.save(mods)
            applyMods()
        }
    }

    var unlimiter: InstalledMod? { mods.first { $0.id == Mods.Unlimiter.id } }
    var otherMods: [InstalledMod] { mods.filter { $0.id != Mods.Unlimiter.id } }

    /// Why Unlimiter can't be used with this game and these engines (nil: it can).
    var unlimiterProblem: String? {
        guard let l = location else { return "Choose the game first." }
        if l.build?.engineSuffix != "-na" {
            return "Needs the North American v1.2 game (SPEED2.EXE of 4,800,512 bytes); this one is \(l.build?.name ?? "another version")."
        }
        if !enginesHaveUnlimiter { return "This build of the app has no Unlimiter code in its North American engine." }
        return nil
    }

    /// The mods that go into the game: turned on, and usable with it.
    var activeMods: [InstalledMod] {
        mods.filter { $0.on && ($0.plugin == nil || unlimiterProblem == nil) }
    }

    func addMod() {
        let p = NSOpenPanel()
        p.canChooseFiles = true
        p.canChooseDirectories = true
        p.allowsMultipleSelection = false
        p.prompt = "Add"
        p.message = "Choose a mod: its archive (.zip, .7z, .rar) or the folder it was extracted to."
        guard p.runModal() == .OK, let u = p.url else { return }
        addModFile(u)
    }

    /// Add File… on Unlimiter's row: the release archive (or its folder).
    func addUnlimiterFile() {
        let p = NSOpenPanel()
        p.canChooseFiles = true
        p.canChooseDirectories = true
        p.allowsMultipleSelection = false
        p.prompt = "Add"
        p.message = "Choose NFSU2-ULv\(Mods.Unlimiter.version).zip from GitHub (or the folder it was extracted to)."
        guard p.runModal() == .OK, let u = p.url else { return }
        addModFile(u, row: Mods.Unlimiter.id)
    }

    /// Adds a mod and turns it on.  row: the row that shows the progress ("add" for
    /// the Add Mod… button, Unlimiter's id for its row, which only takes Unlimiter).
    func addModFile(_ u: URL, row: String = "add", done: ((String?) -> Void)? = nil) {
        modBusy.insert(row)
        modNote[row] = "Adding \(u.lastPathComponent)…"
        let existing = mods
        DispatchQueue.global(qos: .userInitiated).async {
            var err: String? = nil, added: InstalledMod? = nil
            do {
                let m = try Mods.add(u, existing: existing)
                if row == Mods.Unlimiter.id && m.id != Mods.Unlimiter.id {
                    Mods.remove(m)
                    throw Mods.Failure(message: "\(u.lastPathComponent) is not NFSU2 Unlimiter \(Mods.Unlimiter.version).")
                }
                added = m
            } catch let e as Mods.Failure { err = e.message } catch { err = error.localizedDescription }
            DispatchQueue.main.async {
                self.modBusy.remove(row)
                self.modNote[row] = err
                if let m = added {
                    self.mods.removeAll { $0.id == m.id }
                    if m.id == Mods.Unlimiter.id { self.mods.insert(m, at: 0) } else { self.mods.append(m) }
                    self.modNote[m.id] = nil
                    Mods.save(self.mods)
                }
                if err == nil { self.applyMods(done: done) } else { done?(err) }
            }
        }
    }

    func downloadUnlimiter(done: ((String?) -> Void)? = nil) {
        let id = Mods.Unlimiter.id
        modBusy.insert(id)
        modNote[id] = "Downloading…"
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let zip = try Mods.downloadUnlimiter { n in
                    DispatchQueue.main.async {
                        self.modNote[id] = "Downloading… \(formatBytes(n)) of \(formatBytes(Mods.Unlimiter.size))"
                    }
                }
                DispatchQueue.main.async {
                    self.modBusy.remove(id)
                    self.addModFile(zip, row: id) { err in
                        try? FileManager.default.removeItem(at: zip)
                        done?(err)
                    }
                }
            } catch {
                let msg = (error as? Mods.Failure)?.message ?? error.localizedDescription
                DispatchQueue.main.async { self.modBusy.remove(id); self.modNote[id] = msg; done?(msg) }
            }
        }
    }

    func removeMod(_ m: InstalledMod) {
        Mods.remove(m)
        mods.removeAll { $0.id == m.id }
        modNote[m.id] = nil
        Mods.save(mods)
        applyMods()
    }

    func revealMod(_ m: InstalledMod) {
        NSWorkspace.shared.activateFileViewerSelecting([Mods.library.appendingPathComponent(m.id).appendingPathComponent(m.path)])
    }

    func setMod(_ m: InstalledMod, _ on: Bool) {
        guard let i = mods.firstIndex(where: { $0.id == m.id }) else { return }
        mods[i].on = on
        if on { modNote[m.id] = nil }
        Mods.save(mods)
        applyMods()
    }

    /// The player's answer to one of a mod's questions; the mod is installed again.
    func setChoice(_ m: InstalledMod, _ q: ModPrompt, _ answer: String) {
        guard let i = mods.firstIndex(where: { $0.id == m.id }) else { return }
        // an answer given by part of the question (Unlimiter's presets) gives way
        for k in mods[i].choices.keys where q.description.contains(k) { mods[i].choices[k] = nil }
        mods[i].choices[q.description] = answer
        Mods.save(mods)
        applyMods()
    }

    /// (Re)builds the mod files for the turned-on mods, off the main thread.
    func applyMods(done: ((String?) -> Void)? = nil) {
        if let d = done { modsDoneHook = d }
        guard case .ready(let loc) = game else { modsDone(nil, [:]); return }
        if modsApplying != nil { modsPending = true; return }
        let on = activeMods
        if Mods.isBuilt(on, game: loc) && (!on.isEmpty || !FileManager.default.fileExists(atPath: Mods.files.path)) {
            modsDone(nil, [:]); return
        }
        modsApplying = on.isEmpty ? "Removing the mod files…" : "Applying the mods…"
        DispatchQueue.global(qos: .userInitiated).async {
            var err: String? = nil, reports: [String: ModReport] = [:]
            do {
                reports = try Mods.build(on, game: loc) { s in DispatchQueue.main.async { self.modsApplying = s } }
            } catch let e as Mods.Failure { err = e.message } catch { err = error.localizedDescription }
            DispatchQueue.main.async { self.modsDone(err, reports) }
        }
    }

    private func modsDone(_ err: String?, _ reports: [String: ModReport]) {
        modsApplying = nil
        for (id, r) in reports {
            guard let i = mods.firstIndex(where: { $0.id == id }) else { continue }
            mods[i].prompts = r.prompts
            // SpeedReflect is a plugin of its own that scripts copy in when it is there;
            // it isn't, and couldn't run here anyway: not a problem worth showing
            mods[i].issues = r.errors.filter { !($0.message ?? "").contains("SpeedReflect.asi was not found") }
        }
        if let e = err {
            modsFailure = e
            playAfterMods = false
            // the mod that failed is turned off again, with the reason in its row, and
            // the others are applied without it
            if let i = mods.firstIndex(where: { $0.on && e.hasPrefix($0.name + ":") }) {
                mods[i].on = false
                modNote[mods[i].id] = String(e.dropFirst(mods[i].name.count + 1)).trimmingCharacters(in: .whitespaces)
                modsPending = true
            } else { notice = "The mods could not be applied: \(e)" }
        }
        if !reports.isEmpty || err != nil { Mods.save(mods) }
        if modsPending { modsPending = false; applyMods(); return }
        let hook = modsDoneHook, failure = modsFailure
        modsDoneHook = nil; modsFailure = nil
        hook?(failure)
        if playAfterMods { playAfterMods = false; play() }
    }
}
