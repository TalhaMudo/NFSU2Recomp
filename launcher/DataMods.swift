// Data mods: PC mods that change the game's own data files (texture packs inside the
// track and global bundles, the track database), as they are made for the PC game:
// either plain replacement files, or an "install.end" for nlgxzef's Binary, which
// points to an Endscript of edits.
//
// The launcher never changes the game folder.  It builds the changed files into
// ~/Library/Application Support/NFSU2/Mods/Files, at the same paths as in the game
// folder, and the engine reads a file from there when that folder has it
// (runtime/fileio.c, NFSU2_MODS).  With no mod turned on the engine is not told about
// the folder and reads only the game's own files.
//
// The mods are not part of this app.  Download fetches a mod's archive from its own
// page on nfsmods.xyz, only when the player clicks it, and checks it against the
// release this was tested with (SHA-256); Add File... takes a copy the player
// already has (the archive or the folder it was extracted to).
//
// The Endscript subset done here (what the mods in the catalog use; a script with
// any other command is refused, naming it):
//   combobox "A" "B" ... "title", then "A" ... "B" ... end   the first option is used
//   if collection_exists <file> TPKBlocks|Tracks <name>, do, else, end
//   replace_texture <file> TPKBlocks <tpk> <texture> "<dds>"  DXT1/3/5 or A8R8G8B8 .dds
//   update_collection <file> Tracks <track> <field> <value>  the map placement fields
//   unlock_memory ...    Binary empties the game's memory layout files so bigger
//                        bundles fit; this engine allocates without them, so nothing
//                        is written for it.
// A replaced texture's new data is added at the end of its TPK's data (the old data
// stays, unused), its TextureInfo and platform format are updated, and the sizes of
// the enclosing chunks grow by a multiple of 0x80, so every chunk after it keeps its
// alignment.  Edited .lzc bundles are written uncompressed; the game reads both.
// The TPK and track layouts follow MaxHwoy's Nikki and NFSCO's Endscript (MIT),
// which Binary is built on; no code of theirs is used.
import Foundation
import CryptoKit

enum DataMods {
    struct Failure: Error { let message: String }

    struct Mod {
        let id: String
        let name: String
        let author: String
        let summary: String
        let page: URL
        let download: URL
        let size: Int64
        let sha256: String
        let file: String            // the archive's name in the store
    }

    // The releases this was tested with.  Another file is not used for Download.
    static let catalog: [Mod] = [
        Mod(id: "detailed-map", name: "Detailed Map", author: "JeansBig",
            summary: "The world and race maps at 2048 pixels, with shortcuts and ramps.",
            page: URL(string: "https://nfsmods.xyz/mod/4485")!,
            download: URL(string: "https://nfsmods.xyz/uploads/mods/4485/2d7d87-NFSU2%20Detailed%20Map%20v1%20Reup.zip")!,
            size: 44_673_138,
            sha256: "098ac4c3e914f60569020dcbd433b7aae0ce224c2e2d5a69bd3644981c2b1035",
            file: "detailed-map.zip"),
        Mod(id: "fwy-signs", name: "Upscaled Freeway Signs", author: "XoLSt94",
            summary: "Redrawn highway, parking and speed limit signs.",
            page: URL(string: "https://nfsmods.xyz/mod/7342")!,
            download: URL(string: "https://nfsmods.xyz/uploads/mods/7342/9d1294-Upscaled%20Freeway%20Signs.zip")!,
            size: 52_408_411,
            sha256: "aa7523da445c63d876520fa295df1dbdf34ee9a5e56d2ee421b48b89b0273f1a",
            file: "fwy-signs.zip"),
        Mod(id: "carbon-4k", name: "Carbon 4K", author: "XoLSt94",
            summary: "Carbon fibre parts at 4096 instead of 512 pixels.",
            page: URL(string: "https://nfsmods.xyz/mod/7401")!,
            download: URL(string: "https://nfsmods.xyz/uploads/mods/7401/14a9cf-Carbon%204K.zip")!,
            size: 2_209_603,
            sha256: "4d680ad221cd870a0df761eb233c7fde33ab3522616ed715c13170c12ab1991d",
            file: "carbon-4k.zip"),
    ]

    // MARK: the store

    /// ~/Library/Application Support/NFSU2/Mods: the archives, and Files (what the engine reads).
    static var defaultStore: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/NFSU2/Mods")
    }
    static func overlay(_ store: URL) -> URL { store.appendingPathComponent("Files") }
    static func source(_ m: Mod, _ store: URL) -> URL? {
        let fm = FileManager.default
        for u in [store.appendingPathComponent(m.file), store.appendingPathComponent(m.id)] where fm.fileExists(atPath: u.path) {
            return u
        }
        return nil
    }

    /// What the overlay was built from; the engine is only pointed at it while this matches.
    static func stamp(_ mods: [Mod], game: URL, store: URL) -> String {
        var s = "game \(game.standardizedFileURL.path)\n"
        for m in mods {
            let src = source(m, store)
            let a = src.flatMap { try? FileManager.default.attributesOfItem(atPath: $0.path) }
            let size = (a?[.size] as? NSNumber)?.int64Value ?? -1
            let date = (a?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
            s += "\(m.id) \(src?.lastPathComponent ?? "-") \(size) \(Int(date))\n"
        }
        return s
    }
    static func isBuilt(_ mods: [Mod], game: URL, store: URL) -> Bool {
        let f = overlay(store).appendingPathComponent(".built")
        return (try? String(contentsOf: f, encoding: .utf8)) == stamp(mods, game: game, store: store)
    }

    // MARK: getting a mod

    /// Downloads a mod's archive (progress: bytes so far), checks it and keeps it in the store.
    static func download(_ m: Mod, store: URL, progress: @escaping (Int64) -> Void) throws {
        let file = try Downloader(progress: progress).fetch(m.download)
        defer { try? FileManager.default.removeItem(at: file) }
        guard sha256(file) == m.sha256 else {
            throw Failure(message: "The file on nfsmods.xyz is not the release this was made for. Download \(m.name) from its page yourself and use Add File….")
        }
        try keep(file, as: m, store: store)
    }

    /// Add File...: the mod's archive or the folder it was extracted to.  It is
    /// checked by preparing it once (the files it needs must be there).
    static func importCopy(_ src: URL, as m: Mod, game: URL, store: URL) throws {
        let work = try tempDir(store)
        defer { try? FileManager.default.removeItem(at: work) }
        let root = try unpack(src, into: work)
        _ = try Plan.make(root: root)
        // a folder: the checked copy is kept (the player's folder is only read)
        try keep(isDirectory(src) ? work.appendingPathComponent("mod") : src, as: m, store: store, move: isDirectory(src))
    }

    static func keep(_ src: URL, as m: Mod, store: URL, move: Bool = false) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: store, withIntermediateDirectories: true)
        for u in [store.appendingPathComponent(m.file), store.appendingPathComponent(m.id)] where fm.fileExists(atPath: u.path) {
            try fm.removeItem(at: u)
        }
        let dest = isDirectory(src) ? store.appendingPathComponent(m.id) : store.appendingPathComponent(m.file)
        if move { try fm.moveItem(at: src, to: dest) } else { try fm.copyItem(at: src, to: dest) }
    }

    static func remove(_ m: Mod, store: URL) {
        let fm = FileManager.default
        for u in [store.appendingPathComponent(m.file), store.appendingPathComponent(m.id)] { try? fm.removeItem(at: u) }
    }

    /// One download into a temporary file, waiting for it (call it off the main thread).
    final class Downloader: NSObject, URLSessionDownloadDelegate {
        private let progress: (Int64) -> Void
        private let done = DispatchSemaphore(value: 0)
        private var file: URL?
        private var failure: Error?
        init(progress: @escaping (Int64) -> Void) { self.progress = progress }

        func fetch(_ url: URL) throws -> URL {
            let cfg = URLSessionConfiguration.ephemeral
            cfg.timeoutIntervalForRequest = 30
            let s = URLSession(configuration: cfg, delegate: self, delegateQueue: nil)
            s.downloadTask(with: url).resume()
            done.wait()
            s.finishTasksAndInvalidate()
            if let f = file { return f }
            throw Failure(message: "The download did not finish (\(failure?.localizedDescription ?? "no answer")). Try again, or use Add File… with a copy from the mod's page.")
        }
        func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                        totalBytesWritten: Int64, totalBytesExpectedToWrite _: Int64) {
            progress(totalBytesWritten)
        }
        func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
            if let http = downloadTask.response as? HTTPURLResponse, http.statusCode != 200 {
                failure = Failure(message: "HTTP \(http.statusCode)"); return
            }
            let keep = FileManager.default.temporaryDirectory.appendingPathComponent("nfsu2-mod-\(UUID().uuidString)")
            do { try FileManager.default.moveItem(at: location, to: keep); file = keep } catch { failure = error }
        }
        func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
            if let e = error { failure = e }
            done.signal()
        }
    }

    // MARK: building the overlay

    /// Builds Files from the turned-on mods (catalog order: plain files first, then
    /// the scripts, so a script edits on top of a replaced file).  Nothing turned on:
    /// Files is removed.
    static func build(_ mods: [Mod], game: URL, store: URL, status: (String) -> Void = { _ in }) throws {
        let fm = FileManager.default
        let out = overlay(store)
        guard !mods.isEmpty else { try? fm.removeItem(at: out); return }
        let part = store.appendingPathComponent("Files.partial")
        try? fm.removeItem(at: part)
        try fm.createDirectory(at: part, withIntermediateDirectories: true)
        var ok = false
        defer { if !ok { try? fm.removeItem(at: part) } }
        let files = FileSet(game: game, out: part)
        var plans = [(Mod, Plan, URL)]()
        defer { for (_, _, w) in plans { try? fm.removeItem(at: w) } }
        // a failure names its mod (the launcher turns that one off again)
        func step(_ m: Mod, _ what: String, _ f: () throws -> Void) throws {
            status("\(what) \(m.name)…")
            do { try f() } catch let e as Failure { throw Failure(message: "\(m.name): \(e.message)") }
            catch { throw Failure(message: "\(m.name): \(error.localizedDescription)") }
        }
        for m in mods {
            try step(m, "Unpacking") {
                guard let src = source(m, store) else { throw Failure(message: "not downloaded.") }
                let work = try tempDir(store)
                plans.append((m, Plan(), work))
                plans[plans.count - 1].1 = try Plan.make(root: try unpack(src, into: work))
            }
        }
        for (m, p, _) in plans where p.script == nil { try step(m, "Applying") { try files.place(p) } }
        for (m, p, _) in plans where p.script != nil {
            try step(m, "Applying") { try Endscript(files: files, plan: p).run(); try files.flush() }
        }
        try stamp(mods, game: game, store: store).write(to: part.appendingPathComponent(".built"), atomically: true, encoding: .utf8)
        try? fm.removeItem(at: out)
        try fm.moveItem(at: part, to: out)
        ok = true
    }

    static func tempDir(_ store: URL) throws -> URL {
        // in the store, so files move into the overlay without a copy
        let u = store.appendingPathComponent("work-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    /// The archive (.zip, .7z or .rar, with macOS's own bsdtar) or folder, unpacked or
    /// copied into the launcher's own fresh work folder: everything later reads, moves or
    /// edits only what is in there.  A mod with a symbolic link anywhere is refused, so it
    /// can't make the launcher read, move or overwrite files outside it.
    static func unpack(_ src: URL, into work: URL) throws -> URL {
        let fm = FileManager.default
        if isLink(src) { throw Failure(message: "\(src.lastPathComponent) is a link; choose the mod's archive or folder itself.") }
        if isDirectory(src) {
            let copy = work.appendingPathComponent("mod")
            try fm.copyItem(at: src, to: copy)      // links are copied as links, and refused below
        } else {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
            // bsdtar's defaults refuse ".." and absolute paths and won't write through links
            p.arguments = ["-x", "-f", src.path, "-C", work.path]
            p.standardOutput = FileHandle.nullDevice
            p.standardError = FileHandle.nullDevice
            try p.run()
            p.waitUntilExit()
            guard p.terminationStatus == 0 else { throw Failure(message: "\(src.lastPathComponent) could not be opened (not a .zip, .7z or .rar, or damaged).") }
        }
        try checkTree(work)
        return work
    }

    /// No symbolic links, nothing outside the folder.
    static func checkTree(_ root: URL) throws {
        let base = root.resolvingSymlinksInPath().standardizedFileURL.path + "/"
        // the enumerator does not descend through links; every item is checked before use
        guard let e = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.isSymbolicLinkKey], options: []) else {
            throw Failure(message: "The mod could not be read.")
        }
        while let u = e.nextObject() as? URL {
            if (try? u.resourceValues(forKeys: [.isSymbolicLinkKey]))?.isSymbolicLink != false || isLink(u) {
                throw Failure(message: "The mod contains a link (\(u.lastPathComponent)), which the launcher doesn't accept.")
            }
            guard u.resolvingSymlinksInPath().standardizedFileURL.path.hasPrefix(base) else {
                throw Failure(message: "The mod has a file outside its own folder (\(u.lastPathComponent)).")
            }
        }
    }

    static func isLink(_ u: URL) -> Bool {
        var st = stat()
        return lstat(u.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK
    }

    /// What a mod is: an install.end with its Endscript, or game files to use as they are.
    struct Plan {
        var script: URL? = nil          // the Endscript
        var files: [(String, URL)] = [] // plain files: game-relative path, source
        var root: URL? = nil            // the work folder they are in (DataMods.unpack)

        static let gameDirs = ["CARS", "FRONTEND", "GLOBAL", "LANGUAGES", "MOVIES", "SOUND", "TRACKS"]

        static func make(root: URL) throws -> Plan {
            if let inst = find(root, depth: 3, where: { $0.lastPathComponent.lowercased() == "install.end" }) {
                let text = try readText(inst)
                guard let r = text.range(of: #""Endscript"\s*:\s*"([^"]*)""#, options: .regularExpression) else {
                    throw Failure(message: "The mod's install.end names no script.")
                }
                let field = String(text[r])
                let name = field.split(separator: "\"", omittingEmptySubsequences: false).dropLast().last.map(String.init) ?? ""
                guard let s = locate(inst.deletingLastPathComponent(), name) else {
                    throw Failure(message: "The mod's script (\(name)) is missing.")
                }
                return Plan(script: s, root: root)
            }
            // plain files: the folder that has game folders in it (a "Default" variant first)
            guard let base = find(root, depth: 3, dirs: true, where: { d in
                gameDirs.contains(d.lastPathComponent.uppercased())
            })?.deletingLastPathComponent() else {
                throw Failure(message: "This is neither a Binary mod (install.end) nor game files (TRACKS, CARS, GLOBAL...).")
            }
            var p = Plan(root: root)
            let fm = FileManager.default
            for top in (try? fm.contentsOfDirectory(at: base, includingPropertiesForKeys: nil)) ?? []
            where gameDirs.contains(top.lastPathComponent.uppercased()) && isDirectory(top) {
                let e = fm.enumerator(at: top, includingPropertiesForKeys: [.isRegularFileKey])
                while let f = e?.nextObject() as? URL {
                    guard (try? f.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true else { continue }
                    let rel = String(f.standardizedFileURL.path.dropFirst(base.standardizedFileURL.path.count + 1))
                    p.files.append((rel, f))
                }
            }
            guard !p.files.isEmpty else { throw Failure(message: "The mod's game folders are empty.") }
            return p
        }

        /// breadth first, so the shallowest match (and of equals, a "default" one) wins
        static func find(_ root: URL, depth: Int, dirs: Bool = false, where ok: (URL) -> Bool) -> URL? {
            var level = [root]
            for _ in 0...depth {
                var next = [URL]()
                let items = level.flatMap { d -> [URL] in
                    ((try? FileManager.default.contentsOfDirectory(at: d, includingPropertiesForKeys: nil)) ?? [])
                        .filter { !$0.lastPathComponent.hasPrefix(".") && $0.lastPathComponent != "__MACOSX" }
                        .sorted { a, b in
                            let da = a.path.lowercased().contains("default"), db = b.path.lowercased().contains("default")
                            return da != db ? da : a.path < b.path
                        }
                }
                for u in items {
                    if isDirectory(u) { if dirs && ok(u) { return u }; next.append(u) }
                    else if !dirs && ok(u) { return u }
                }
                level = next
            }
            return nil
        }
    }

    // MARK: the game's files, as edited so far

    final class FileSet {
        let game: URL, out: URL
        private var dirty = [String: (String, [UInt8])]()   // key: upper-case path -> (path, data)
        private var held = 0

        init(game: URL, out: URL) { self.game = game; self.out = out }

        /// "TRACKS\TRACKMAP4000.BIN" -> its path in the game folder, with the folder's own case
        func path(_ win: String) throws -> String {
            let parts = win.replacingOccurrences(of: "\\", with: "/").split(separator: "/").map(String.init).filter { $0 != "." }
            guard !parts.isEmpty, !parts.contains("..") else { throw Failure(message: "The mod names a file outside the game folder (\(win)).") }
            var rel = [String](), dir = game
            for (i, c) in parts.enumerated() {
                let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
                guard let n = names.first(where: { $0.caseInsensitiveCompare(c) == .orderedSame }) else {
                    // a new file (only its name: the folders must exist)
                    guard i == parts.count - 1 else { throw Failure(message: "The game has no \(win).") }
                    rel.append(c); break
                }
                rel.append(n); dir = dir.appendingPathComponent(n)
            }
            return rel.joined(separator: "/")
        }

        func exists(_ win: String) -> Bool {
            guard let p = try? path(win) else { return false }
            return dirty[p.uppercased()] != nil || FileManager.default.fileExists(atPath: out.appendingPathComponent(p).path)
                || FileManager.default.fileExists(atPath: game.appendingPathComponent(p).path)
        }

        /// the file's bytes, JDLZ bundles unpacked
        func load(_ win: String) throws -> [UInt8] {
            let p = try path(win)
            if let d = dirty[p.uppercased()] { return d.1 }
            let mine = out.appendingPathComponent(p)
            let src = FileManager.default.fileExists(atPath: mine.path) ? mine : game.appendingPathComponent(p)
            guard let data = try? Data(contentsOf: src) else { throw Failure(message: "The game has no \(win).") }
            let b = [UInt8](data)
            return b.starts(with: Array("JDLZ".utf8)) ? try jdlz(b) : b
        }

        func store(_ win: String, _ data: [UInt8]) throws {
            let p = try path(win)
            if let old = dirty[p.uppercased()] { held -= old.1.count }
            dirty[p.uppercased()] = (p, data)
            held += data.count
            if held > 256 << 20 { try flush() }
        }

        func flush() throws {
            for (_, (p, d)) in dirty {
                let u = out.appendingPathComponent(p)
                try FileManager.default.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
                try Data(d).write(to: u)
            }
            dirty = [:]; held = 0
        }

        /// plain files: moved in as they are
        func place(_ plan: Plan) throws {
            let fm = FileManager.default
            for (rel, src) in plan.files {
                // only ever moved out of the launcher's own work folder
                guard let r = plan.root, src.standardizedFileURL.path.hasPrefix(r.standardizedFileURL.path + "/"), !isLink(src) else {
                    throw Failure(message: "The mod's \(rel) is not in its own folder.")
                }
                let p = try path(rel)
                let u = out.appendingPathComponent(p)
                try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? fm.removeItem(at: u)
                try fm.moveItem(at: src, to: u)
            }
        }
    }

    // MARK: the Endscript

    struct Endscript {
        let files: FileSet
        let plan: Plan
        var dir: URL { plan.script!.deletingLastPathComponent() }

        private struct Frame { var combobox: Bool; var taken: Bool; var choice: String = ""; var label: String = ""; var outer: Bool }

        func run() throws {
            let text = try readText(plan.script!)
            var stack = [Frame]()
            func active() -> Bool { stack.allSatisfy { $0.combobox ? $0.label == $0.choice : $0.taken } }
            for (n, raw) in text.components(separatedBy: .newlines).enumerated() {
                let line = raw.trimmingCharacters(in: .whitespaces)
                if line.isEmpty || line.hasPrefix("//") || line.hasPrefix("#") || line.hasPrefix("[VERSN") { continue }
                let t = tokens(line)
                guard let cmd = t.first else { continue }
                let where_ = "line \(n + 1) of \(plan.script!.lastPathComponent)"
                if line.hasPrefix("\"") && t.count == 1 {          // an option of the combobox
                    guard var f = stack.last, f.combobox else { throw Failure(message: "Unexpected \(line) at \(where_).") }
                    f.label = cmd; stack[stack.count - 1] = f; continue
                }
                switch cmd.lowercased() {
                case "combobox":
                    guard t.count >= 3 else { throw Failure(message: "A combobox without options at \(where_).") }
                    stack.append(Frame(combobox: true, taken: true, choice: t[1], outer: active()))   // the first option
                case "if":
                    let take = active() ? try condition(Array(t.dropFirst()), where_) : false
                    stack.append(Frame(combobox: false, taken: take, outer: active()))
                case "do": continue
                case "else":
                    guard var f = stack.last, !f.combobox else { throw Failure(message: "else without if at \(where_).") }
                    f.taken = f.outer && !f.taken; stack[stack.count - 1] = f
                case "end":
                    guard !stack.isEmpty else { throw Failure(message: "end without a block at \(where_).") }
                    stack.removeLast()
                default:
                    guard active() else { continue }
                    try command(cmd.lowercased(), t, where_)
                }
            }
        }

        func tokens(_ line: String) -> [String] {
            var r = [String](), cur = "", quoted = false, has = false
            for c in line {
                if c == "\"" { quoted.toggle(); has = true; continue }
                if !quoted && (c == " " || c == "\t") {
                    if has || !cur.isEmpty { r.append(cur) }
                    cur = ""; has = false; continue
                }
                cur.append(c)
            }
            if has || !cur.isEmpty { r.append(cur) }
            return r
        }

        func condition(_ t: [String], _ at: String) throws -> Bool {
            guard t.first?.lowercased() == "collection_exists", t.count == 4 else {
                throw Failure(message: "The mod's script uses \"if \(t.first ?? "")\" (\(at)), which the launcher can't do yet.")
            }
            guard files.exists(t[1]) else { return false }
            let d = try files.load(t[1])
            switch t[2].lowercased() {
            case "tpkblocks": return TPK.list(d).contains { $0.name.caseInsensitiveCompare(t[3]) == .orderedSame }
            case "tracks": return UInt16(t[3]).map { Tracks.record(d, $0) != nil } ?? false
            default: throw Failure(message: "The mod's script asks about \(t[2]) (\(at)), which the launcher can't do yet.")
            }
        }

        func command(_ cmd: String, _ t: [String], _ at: String) throws {
            switch cmd {
            case "unlock_memory":
                return      // see the header: nothing to write for this engine
            case "replace_texture":
                guard t.count == 6, t[2].lowercased() == "tpkblocks" else { throw Failure(message: "A replace_texture the launcher can't read (\(at)).") }
                guard let file = locate(dir, t[5]) else { throw Failure(message: "The mod's \(t[5]) is missing.") }
                let dds = try DDS(file)
                let key = t[4].lowercased().hasPrefix("0x") ? UInt32(t[4].dropFirst(2), radix: 16) ?? 0 : stringHash(t[4])
                var d = try files.load(t[1])
                try TPK.replace(&d, tpk: t[3], texture: key, name: t[4], with: dds)
                try files.store(t[1], d)
            case "update_collection":
                guard t.count == 6, t[2].lowercased() == "tracks", let track = UInt16(t[3]) else {
                    throw Failure(message: "The mod's script changes \(t.count > 2 ? t[2] : "a collection") (\(at)), which the launcher can't do yet.")
                }
                var d = try files.load(t[1])
                try Tracks.set(&d, track: track, field: t[4], value: t[5])
                try files.store(t[1], d)
            default:
                throw Failure(message: "The mod's script uses \"\(cmd)\" (\(at)), which the launcher can't do yet.")
            }
        }
    }

    // MARK: chunks and TPKs

    static func u32(_ d: [UInt8], _ o: Int) -> UInt32 {
        guard o >= 0, o + 4 <= d.count else { return 0 }
        return UInt32(d[o]) | UInt32(d[o + 1]) << 8 | UInt32(d[o + 2]) << 16 | UInt32(d[o + 3]) << 24
    }
    static func put32(_ d: inout [UInt8], _ o: Int, _ v: UInt32) {
        for k in 0..<4 { d[o + k] = UInt8(truncatingIfNeeded: v >> (8 * UInt32(k))) }
    }
    static func put16(_ d: inout [UInt8], _ o: Int, _ v: Int) {
        d[o] = UInt8(truncatingIfNeeded: v); d[o + 1] = UInt8(truncatingIfNeeded: v >> 8)
    }

    /// (id, header offset, payload start, payload end)
    static func chunks(_ d: [UInt8], _ start: Int, _ end: Int) -> [(UInt32, Int, Int, Int)] {
        var r = [(UInt32, Int, Int, Int)](), off = start
        while off + 8 <= end {
            let sz = Int(u32(d, off + 4))
            if sz > end - off - 8 { break }
            r.append((u32(d, off), off, off + 8, off + 8 + sz))
            off += 8 + sz
        }
        return r
    }

    enum TPK {
        struct Info {
            var name: String
            var path: [Int]                 // header offsets: the enclosing chunks, then the TPK
            var infos: (Int, Int)           // TextureInfo array (0x7C each)
            var formats: (Int, Int)?        // platform info (0x20 each, D3D format at +0x14)
            var data: (Int, Int, Int, Int)? // the 0x33320002 chunk: header, payload start, end; its 0xB3320000
            var compressed: Bool
        }

        static func list(_ d: [UInt8]) -> [Info] {
            var r = [Info](), used = Set<String>()
            func walk(_ s: Int, _ e: Int, _ path: [Int]) {
                for (id, h, b, ce) in chunks(d, s, e) {
                    if id == 0xB330_0000 { if var t = parse(d, h, b, ce, path + [h]) {
                        // Binary's names: the .tpk file's name, else the collection name
                        let n = t.name.uppercased()
                        t.name = used.contains(n) ? "TPK\(r.count)" : n
                        used.insert(t.name); r.append(t) } }
                    else if id & 0x8000_0000 != 0 && path.count < 6 { walk(b, ce, path + [h]) }
                }
            }
            walk(0, d.count, [])
            return r
        }

        static func cstr(_ d: [UInt8], _ o: Int, _ n: Int) -> String {
            let s = d[o..<min(d.count, o + n)].prefix { $0 != 0 }
            return String(decoding: s, as: UTF8.self)
        }

        static func parse(_ d: [UInt8], _ h: Int, _ b: Int, _ e: Int, _ path: [Int]) -> Info? {
            var t = Info(name: "", path: path, infos: (0, 0), formats: nil, data: nil, compressed: false)
            for (id, _, cb, ce) in chunks(d, b, e) {
                if id == 0xB331_0000 {
                    for (pid, _, pb, pe) in chunks(d, cb, ce) {
                        switch pid {
                        case 0x3331_0001:
                            let file = cstr(d, pb + 0x20, 0x40)
                            if file.lowercased().hasSuffix(".tpk") {
                                let base = file.split(whereSeparator: { $0 == "\\" || $0 == "/" }).last.map(String.init) ?? file
                                t.name = String(base.dropLast(4))
                            } else { t.name = cstr(d, pb + 4, 0x1C) }
                        case 0x3331_0003: t.compressed = true
                        case 0x3331_0004: t.infos = (pb, pe)
                        case 0x3331_0005: t.formats = (pb, pe)
                        default: break
                        }
                    }
                } else if id == 0xB332_0000 {
                    if let c = chunks(d, cb, ce).first(where: { $0.0 == 0x3332_0002 }) { t.data = (c.1, c.2, c.3, cb - 8) }
                }
            }
            return t.infos.1 > t.infos.0 ? t : nil
        }

        static func replace(_ d: inout [UInt8], tpk: String, texture key: UInt32, name: String, with dds: DDS) throws {
            let all = list(d)
            func has(_ t: Info) -> Int? {
                (0..<((t.infos.1 - t.infos.0) / 0x7C)).first { u32(d, t.infos.0 + $0 * 0x7C + 0x24) == key }
            }
            // by name; a name the file doesn't have (another tool's naming): the one TPK with the texture
            var pick = all.first { $0.name.caseInsensitiveCompare(tpk) == .orderedSame && has($0) != nil }
            if pick == nil { let w = all.filter { has($0) != nil }; if w.count == 1 { pick = w[0] } }
            guard let t = pick, let i = has(t), let data = t.data else {
                throw Failure(message: "The game file has no texture \(name) in \(tpk).")
            }
            guard !t.compressed else { throw Failure(message: "\(tpk) is a compressed texture pack, which the launcher can't change yet.") }
            let pixBase = (data.1 + 0x7F) & ~0x7F
            let oldEnd = data.2
            let start = (oldEnd + 0x7F) & ~0x7F
            let grow = (start - oldEnd + dds.levels.count + 0x7F) & ~0x7F
            var add = [UInt8](repeating: 0, count: grow)
            add.replaceSubrange((start - oldEnd)..<(start - oldEnd + dds.levels.count), with: dds.levels)
            d.insert(contentsOf: add, at: oldEnd)
            // every chunk around the new data grows (the info chunks lie before it: same offsets)
            for h in t.path + [data.3, data.0] { put32(&d, h + 4, u32(d, h + 4) + UInt32(grow)) }
            let ti = t.infos.0 + i * 0x7C
            put32(&d, ti + 0x30, UInt32(start - pixBase))           // image offset
            put32(&d, ti + 0x38, UInt32(dds.levels.count))          // image size
            put32(&d, ti + 0x3C, 0)                                 // palette size
            put32(&d, ti + 0x40, UInt32(dds.baseSize))              // level 0 size, as Binary writes it
            put16(&d, ti + 0x44, dds.w); put16(&d, ti + 0x46, dds.h)
            d[ti + 0x48] = UInt8(log2(dds.w)); d[ti + 0x49] = UInt8(log2(dds.h))
            d[ti + 0x4A] = dds.compression; d[ti + 0x4B] = 0
            put16(&d, ti + 0x4C, 0)                                 // palette entries
            d[ti + 0x4E] = UInt8(dds.count)
            if let f = t.formats, f.0 + (i + 1) * 0x20 <= f.1 { put32(&d, f.0 + i * 0x20 + 0x14, dds.format) }
        }

        static func log2(_ v: Int) -> Int { var n = 0; while (1 << (n + 1)) <= v { n += 1 }; return n }
    }

    struct DDS {
        let w: Int, h: Int, count: Int
        let format: UInt32          // the platform info's D3D format
        let compression: UInt8      // the TextureInfo's compression type
        let levels: [UInt8]
        var baseSize: Int {
            if compression == 0x20 { return w * h * 4 }
            var x = levels.count; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16
            return x - (x >> 1)
        }

        init(_ url: URL) throws {
            guard let b = try? [UInt8](Data(contentsOf: url)), b.count >= 128, b.starts(with: Array("DDS ".utf8)) else {
                throw Failure(message: "The mod's \(url.lastPathComponent) is missing or not a .dds file.")
            }
            h = Int(u32(b, 12)); w = Int(u32(b, 16))
            // The game uploads at most 8 levels (the most any of its own textures has); a
            // texture declaring more had the rest missing, so only the first 8 are kept.
            let mips = min(8, u32(b, 8) & 0x20000 != 0 ? max(1, Int(u32(b, 28))) : 1)
            let flags = u32(b, 80), four = u32(b, 84)
            let block: Int
            switch (flags & 4 != 0, four) {
            case (true, 0x3154_5844): format = four; compression = 0x22; block = 8      // DXT1
            case (true, 0x3354_5844): format = four; compression = 0x24; block = 16     // DXT3
            case (true, 0x3554_5844): format = four; compression = 0x26; block = 16     // DXT5
            case (false, _) where u32(b, 88) == 32 && u32(b, 92) == 0xFF0000 && u32(b, 96) == 0xFF00
                                 && u32(b, 100) == 0xFF && u32(b, 104) == 0xFF00_0000:
                format = 0x15; compression = 0x20; block = 0                              // A8R8G8B8
            default: throw Failure(message: "The mod's \(url.lastPathComponent) is in a format the launcher can't use yet.")
            }
            guard w > 0, h > 0, w <= 8192, h <= 8192 else { throw Failure(message: "The mod's \(url.lastPathComponent) has an unusable size.") }
            var n = 0, lv = 0
            while lv < mips {
                let lw = max(1, w >> lv), lh = max(1, h >> lv)
                let size = block > 0 ? max(1, (lw + 3) / 4) * max(1, (lh + 3) / 4) * block : lw * lh * 4
                if 128 + n + size > b.count { break }
                n += size; lv += 1
            }
            guard lv > 0 else { throw Failure(message: "The mod's \(url.lastPathComponent) is cut short.") }
            count = lv
            levels = Array(b[128..<(128 + n)])
        }
    }

    // MARK: the track database (GLOBALB's Tracks chunk, 0x128 bytes per track)

    enum Tracks {
        // the fields map placement mods change (floats)
        static let fields: [String: Int] = [
            "trackmapcalibrationoffsetx": 0xAC, "trackmapcalibrationoffsety": 0xB0,
            "trackmapcalibrationwidth": 0xB4, "trackmapcalibrationzoomin": 0xC0,
            "menumapzoomoffsetx": 0x118, "menumapzoomoffsety": 0x11C, "menumapzoomwidth": 0x120,
        ]

        static func record(_ d: [UInt8], _ track: UInt16) -> Int? {
            func walk(_ s: Int, _ e: Int, _ depth: Int) -> Int? {
                for (id, _, b, ce) in chunks(d, s, e) {
                    if id == 0x0003_4201 {
                        var o = b
                        while o + 0x128 <= ce {
                            if Int(d[o + 0x8A]) | Int(d[o + 0x8B]) << 8 == Int(track) { return o }
                            o += 0x128
                        }
                    } else if id & 0x8000_0000 != 0 && depth < 4, let r = walk(b, ce, depth + 1) { return r }
                }
                return nil
            }
            return walk(0, d.count, 0)
        }

        static func set(_ d: inout [UInt8], track: UInt16, field: String, value: String) throws {
            guard let off = fields[field.lowercased()] else {
                throw Failure(message: "The mod changes the track field \(field), which the launcher can't do yet.")
            }
            guard let v = Float(value) else { throw Failure(message: "\(value) is not a number (\(field)).") }
            guard let r = record(d, track) else { throw Failure(message: "The game has no track \(track).") }
            put32(&d, r + off, v.bitPattern)
        }
    }

    // MARK: helpers

    /// the game's name hash (bStringHash)
    static func stringHash(_ s: String) -> UInt32 {
        var h: UInt32 = 0xFFFF_FFFF
        for c in s.utf8 { h = h &* 33 &+ UInt32(c) }
        return h
    }

    /// EA's JDLZ (the .lzc bundles)
    static func jdlz(_ b: [UInt8]) throws -> [UInt8] {
        let n = Int(u32(b, 8))
        var out = [UInt8](repeating: 0, count: n)
        var i = 16, o = 0, f1 = 1, f2 = 1
        while i < b.count && o < n {
            if f1 == 1 { f1 = Int(b[i]) | 0x100; i += 1 }
            if f2 == 1 { f2 = Int(b[i]) | 0x100; i += 1 }
            if f1 & 1 != 0 {
                guard i + 1 < b.count else { break }
                let ln: Int, t: Int
                if f2 & 1 != 0 { ln = (Int(b[i + 1]) | (Int(b[i]) & 0xF0) << 4) + 3; t = (Int(b[i]) & 0x0F) + 1 }
                else { t = (Int(b[i + 1]) | (Int(b[i]) & 0xE0) << 3) + 17; ln = (Int(b[i]) & 0x1F) + 3 }
                i += 2
                guard o - t >= 0, o + ln <= n else { throw Failure(message: "A game file is damaged (JDLZ).") }
                for k in 0..<ln { out[o + k] = out[o + k - t] }
                o += ln; f2 >>= 1
            } else {
                out[o] = b[i]; o += 1; i += 1
            }
            f1 >>= 1
        }
        guard o == n else { throw Failure(message: "A game file is damaged (JDLZ: \(o) of \(n) bytes).") }
        return out
    }

    static func sha256(_ u: URL) -> String {
        guard let h = try? FileHandle(forReadingFrom: u) else { return "" }
        defer { try? h.close() }
        var s = SHA256()
        while true {
            let chunk = h.readData(ofLength: 1 << 20)
            if chunk.isEmpty { break }
            s.update(data: chunk)
        }
        return s.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func isDirectory(_ u: URL) -> Bool {
        var d: ObjCBool = false
        return FileManager.default.fileExists(atPath: u.path, isDirectory: &d) && d.boolValue
    }

    /// a mod's relative Windows path, found case-insensitively
    static func locate(_ dir: URL, _ win: String) -> URL? {
        var cur = dir
        for c in win.replacingOccurrences(of: "\\", with: "/").split(separator: "/").map(String.init) where c != "." {
            guard c != ".." else { return nil }
            let names = (try? FileManager.default.contentsOfDirectory(atPath: cur.path)) ?? []
            guard let n = names.first(where: { $0.caseInsensitiveCompare(c) == .orderedSame }) else { return nil }
            cur = cur.appendingPathComponent(n)
        }
        return cur
    }

    static func readText(_ u: URL) throws -> String {
        guard let d = try? Data(contentsOf: u) else { throw Failure(message: "\(u.lastPathComponent) could not be read.") }
        return String(data: d, encoding: .utf8) ?? String(decoding: d, as: UTF8.self)
    }
}
