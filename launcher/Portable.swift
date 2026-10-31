// "Make a portable copy": a new app next to this one, with the game files inside
// (Contents/Resources/Game), the same layout ./pack.sh --embed produces.
//
// The running app is never modified: writing inside it would break its signature
// and fails anyway when macOS runs a downloaded app from a read-only location
// (App Translocation).  The copy is built under a hidden temporary name, the
// download tag is removed (otherwise Gatekeeper would check the copy again and
// refuse it), it is signed ad hoc again (the added files changed the bundle)
// and only then moved to its final name.  Files are cloned where the disk allows
// it (APFS), so on the same disk the copy takes no extra space and is instant.
import Foundation
import Darwin

final class PortableBuilder {
    struct Progress { var done: Int64; var total: Int64; var phase: String }
    enum Failure: Error { case cancelled, noSpace(Int64, Int64), step(String) }

    let sourceApp: URL
    let gameFolder: URL
    let destination: URL
    private let lock = NSLock()
    private var cancelFlag = false

    // The engine never reads these: Windows-only plugins and scripts.
    static let skippedGameItems: Set<String> = ["SCRIPTS", "dinput8.dll", "server.dll",
                                                "server.cfg", "SafeMode.bat"]
    // Copied unless left out (the Make Copy dialog's checkboxes): the game plays on without them.
    struct Extra { let folder: String; let title: String; let without: String }
    static let extras = [
        Extra(folder: "MOVIES", title: "the movies", without: "the game skips its movies"),
        Extra(folder: "LANGPACKS", title: "the language packs", without: "only the languages in LANGUAGES"),
    ]

    let leaveOut: Set<String>    // folder names of `extras`, upper case

    init(sourceApp: URL, gameFolder: URL, destination: URL, leaveOut: Set<String> = []) {
        self.sourceApp = sourceApp
        self.gameFolder = gameFolder
        self.destination = destination
        self.leaveOut = Set(leaveOut.map { $0.uppercased() })
    }

    /// The extras the game folder has, with their sizes.
    static func extras(in gameFolder: URL) -> [(Extra, Int64)] {
        let fm = FileManager.default
        let names = (try? fm.contentsOfDirectory(atPath: gameFolder.path)) ?? []
        return extras.compactMap { x in
            guard let name = names.first(where: { $0.caseInsensitiveCompare(x.folder) == .orderedSame }),
                  let e = fm.enumerator(at: gameFolder.appendingPathComponent(name),
                                        includingPropertiesForKeys: [.fileSizeKey]) else { return nil }
            var total: Int64 = 0
            for case let u as URL in e { total += Int64((try? u.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0) }
            return (x, total)
        }
    }

    func cancel() { lock.lock(); cancelFlag = true; lock.unlock() }
    private var cancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelFlag }

    private struct Item { let src: URL; let rel: String; let isDir: Bool; let size: Int64 }

    /// Files under `root`, skipping what `skip` rejects (relative path, depth 1 = top level).
    private func list(_ root: URL, skip: (String, Int) -> Bool) -> [Item] {
        var out: [Item] = []
        let keys: [URLResourceKey] = [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey]
        guard let en = FileManager.default.enumerator(at: root, includingPropertiesForKeys: keys,
                                                      options: []) else { return out }
        let base = root.standardizedFileURL.path
        for case let u as URL in en {
            let p = u.standardizedFileURL.path
            guard p.hasPrefix(base) else { continue }
            let rel = String(p.dropFirst(base.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            if u.lastPathComponent == ".DS_Store" || skip(rel, en.level) {
                if (try? u.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true { en.skipDescendants() }
                continue
            }
            let v = try? u.resourceValues(forKeys: Set(keys))
            let isDir = v?.isDirectory == true && v?.isSymbolicLink != true
            out.append(Item(src: u, rel: rel, isDir: isDir, size: isDir ? 0 : Int64(v?.fileSize ?? 0)))
        }
        return out
    }

    private func copy(_ items: [Item], into dst: URL, progress: (Int64) -> Void) throws {
        let fm = FileManager.default
        for it in items {
            if cancelled { throw Failure.cancelled }
            let target = dst.appendingPathComponent(it.rel)
            if it.isDir {
                try fm.createDirectory(at: target, withIntermediateDirectories: true)
                continue
            }
            try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            // COPYFILE_CLONE: clone on APFS, a normal copy otherwise; keeps modes and links.
            if copyfile(it.src.path, target.path, nil, copyfile_flags_t(COPYFILE_CLONE)) != 0 {
                throw Failure.step("Could not copy \(it.rel): \(String(cString: strerror(errno)))")
            }
            progress(it.size)
        }
    }

    @discardableResult
    private func run(_ tool: String, _ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }

    private func volumeID(_ u: URL) -> NSObject? {
        (try? u.resourceValues(forKeys: [.volumeIdentifierKey]))?.volumeIdentifier as? NSObject
    }

    /// Builds the copy.  Calls `report` from this (background) thread.
    func build(report: (Progress) -> Void) throws -> URL {
        let fm = FileManager.default
        let parent = destination.deletingLastPathComponent()
        let temp = parent.appendingPathComponent("." + destination.lastPathComponent + ".partial")
        try? fm.removeItem(at: temp)

        report(Progress(done: 0, total: 0, phase: "Looking at the game files…"))
        let appItems = list(sourceApp) { rel, _ in rel == "Contents/Resources/Game" || rel == "Contents/_CodeSignature" }
        let gameItems = list(gameFolder) { rel, level in
            level == 1 && (Self.skippedGameItems.contains(rel)
                           || leaveOut.contains(rel.uppercased()))
        }
        let total = (appItems + gameItems).reduce(Int64(0)) { $0 + $1.size }

        // Clones on the same disk need no space; on another disk the whole size is needed.
        if volumeID(parent) != volumeID(gameFolder),
           let free = (try? parent.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
                        .volumeAvailableCapacityForImportantUsage,
           free < total + 50_000_000 {
            throw Failure.noSpace(total, free)
        }

        var done: Int64 = 0
        var lastReport = Date.distantPast
        let tick: (Int64) -> Void = { n in
            done += n
            if Date().timeIntervalSince(lastReport) > 0.05 {
                lastReport = Date()
                report(Progress(done: done, total: total, phase: "Copying the game files…"))
            }
        }
        do {
            try fm.createDirectory(at: temp, withIntermediateDirectories: false)
            try copy(appItems, into: temp, progress: tick)
            try copy(gameItems, into: temp.appendingPathComponent("Contents/Resources/Game"), progress: tick)
            if cancelled { throw Failure.cancelled }

            report(Progress(done: total, total: total, phase: "Finishing…"))
            run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", temp.path])
            if run("/usr/bin/codesign", ["--force", "--sign", "-", "--timestamp=none", temp.path]) != 0 {
                NSLog("portable copy: codesign failed (the copy still runs on this Mac)")
            }
            if fm.fileExists(atPath: destination.path) {
                // The save panel already asked whether to replace it.
                if (try? fm.trashItem(at: destination, resultingItemURL: nil)) == nil {
                    try fm.removeItem(at: destination)
                }
            }
            try fm.moveItem(at: temp, to: destination)
        } catch {
            try? fm.removeItem(at: temp)
            throw error
        }
        return destination
    }
}
