import Foundation

func tryChecked(_ body: () throws -> Bool) -> Bool { try! body() }

@main struct SaveManagerTests {
    static func main() throws {
        let root = URL(fileURLWithPath: "/private/tmp").appendingPathComponent("save-manager-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let saves = root.appendingPathComponent("saves"), registry = root.appendingPathComponent("registry")
        let store = SaveStore(saves: saves, registry: registry)
        var profile = Data(repeating: 0, count: 54966)
        profile.replaceSubrange(0..<4, with: Data("20CM".utf8))
        for (at, value) in [(4,54966),(8,8),(12,54930),(28,0x10d),(32,54930)] { profile.put32(at, UInt32(value)) }
        profile.replaceSubrange(53797..<53801, with: Data("TEST".utf8))
        for i in 0..<48 { profile.put32(36 + i * 24 + 20, UInt32.max) }
        profile.put32(16, SaveProfile.checksum(Data(profile[28..<36])))
        profile.put32(20, SaveProfile.checksum(Data(profile[36...])))
        profile.put32(24, SaveProfile.checksum(Data(profile[..<24])))
        let entries = [SaveEntry(path: "NFS Underground 2/TEST/TEST", data: profile)]
        let zip = try SaveArchive.encode(entries)
        assert(tryChecked { try SaveArchive.decode(zip).first!.data == profile })
        try store.restore(zip, gameRunning: { false })
        assert(tryChecked { try store.profiles().map { $0.name } == ["TEST"] })
        let before = try store.snapshot()
        func rejects(_ body: () throws -> Void) {
            do { try body(); fatalError("Expected rejection") } catch { }
        }
        rejects { try store.restore(zip, gameRunning: { true }) }
        var broken = zip; broken[45] ^= 1
        rejects { try store.restore(broken, gameRunning: { false }) }
        rejects { _ = try SaveArchive.encode([SaveEntry(path: "../escape", data: Data())]) }
        rejects { _ = try SaveArchive.encode([SaveEntry(path: "A", data: Data()), SaveEntry(path: "a", data: Data())]) }
        var invalid = profile; invalid[100] ^= 1
        rejects { try store.restore(SaveArchive.encode([SaveEntry(path: entries[0].path, data: invalid)]), gameRunning: { false }) }
        assert(tryChecked { try store.snapshot().map { $0.data } == before.map { $0.data } })
        // A spelling the file system treats as the profile root is checked like the root itself.
        rejects { try store.restore(SaveArchive.encode(entries + [SaveEntry(path: "nfs UNDERGROUND 2/EVIL/evil", data: invalid)]), gameRunning: { false }) }
        // Files outside a valid profile's folder are skipped; the profile goes under the root's own spelling.
        let extra = [SaveEntry(path: "nfs underground 2/TEST/TEST", data: profile), SaveEntry(path: "evil.txt", data: Data([1])),
                     SaveEntry(path: "NFS Underground 2/OTHER/OTHER.DAT", data: Data([1])), SaveEntry(path: "Other/TEST/TEST", data: profile)]
        assert(tryChecked { try store.restore(SaveArchive.encode(extra), gameRunning: { false }) == 3 })
        assert(tryChecked { try store.snapshot().map { $0.path } == ["NFS Underground 2/TEST/TEST"] })
        assert(tryChecked { try FileManager.default.contentsOfDirectory(atPath: saves.path) == ["NFS Underground 2"] })
        // A full backup's registry.txt brings back only the game's number settings.
        let reg = SaveEntry(path: "registry.txt", data: Data("d g_CarLodLevel=2\ns Install Dir=/tmp\nd bad name=1\nd g_Fog=-1\nd NotFirstTime=1\n".utf8))
        assert(tryChecked { try store.restore(SaveArchive.encode(entries + [reg]), gameRunning: { false }) == 0 })
        assert(tryChecked { try Data(contentsOf: registry) == Data("d g_CarLodLevel=2\nd NotFirstTime=1\n".utf8) })
        // Magazine / DVD covers in folders of their own come back with a full restore; a damaged one is skipped.
        var cover = Data(repeating: 0, count: 1048612)
        cover.replaceSubrange(0..<4, with: Data("20CM".utf8))
        for (at, value) in [(4,1048612),(8,8),(12,0x100000),(28,0x10d),(32,0x100000)] { cover.put32(at, UInt32(value)) }
        cover.put32(16, SaveProfile.checksum(Data(cover[28..<36])))
        cover.put32(20, SaveProfile.checksum(Data(cover[36...])))
        cover.put32(24, SaveProfile.checksum(Data(cover[..<24])))
        var badCover = cover; badCover[1000] ^= 1
        let covers = [SaveEntry(path: "NFS Underground 2/TEST Magazine 1/TEST Magazine 1", data: cover),
                      SaveEntry(path: "NFS Underground 2/TEST DVD 1/TEST DVD 1", data: badCover)]
        assert(tryChecked { try store.restore(SaveArchive.encode(entries + covers + [reg]), gameRunning: { false }) == 1 })
        assert(tryChecked { try store.snapshot().map { $0.path } ==
            ["NFS Underground 2/TEST Magazine 1/TEST Magazine 1", "NFS Underground 2/TEST/TEST", "registry.txt"] })
        assert(tryChecked { try store.profiles().map { $0.name } == ["TEST"] })
        let link = saves.appendingPathComponent("link")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: registry)
        rejects { _ = try store.snapshot() }
        try FileManager.default.removeItem(at: link)
        assert(tryChecked { try FileManager.default.contentsOfDirectory(atPath: store.backups.path).contains { $0.hasSuffix(".zip") } })
        print("save_manager: archive, profile, game guard, rollback and symlink checks passed")
    }
}
