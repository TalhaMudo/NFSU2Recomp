import Foundation
import zlib

struct SaveError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}
struct SaveEntry {
    let path: String
    let data: Data
    var modified: Date = Date()
}
extension Data {
    func u16(_ at: Int) -> Int { Int(self[at]) | Int(self[at + 1]) << 8 }
    func u32(_ at: Int) -> UInt32 { (0..<4).reduce(0) { $0 | UInt32(self[at + $1]) << ($1 * 8) } }
    mutating func put16(_ at: Int, _ n: Int) { self[at] = UInt8(truncatingIfNeeded: n); self[at + 1] = UInt8(truncatingIfNeeded: n >> 8) }
    mutating func put32(_ at: Int, _ n: UInt32) { for i in 0..<4 { self[at + i] = UInt8(truncatingIfNeeded: n >> (i * 8)) } }
}

enum SaveArchive {
    static let maxTotal = 256 << 20, maxEntry = 64 << 20, maxEntries = 4096
    static func validPath(_ path: String) -> Bool {
        let parts = path.split(separator: "/", omittingEmptySubsequences: false)
        return !path.isEmpty && path.utf8.count <= 1024 && !path.contains(":") && !path.contains("\\") &&
            !path.unicodeScalars.contains { $0.value < 32 || $0.value == 127 } &&
            parts.allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." && $0.utf8.count <= 255 }
    }
    static func validatePaths(_ entries: [SaveEntry]) throws {
        var seen = Set<String>(), bytes = 0
        guard entries.count <= maxEntries else { throw SaveError("Too many archive entries.") }
        for e in entries {
            bytes += e.data.count
            let path = e.path.precomposedStringWithCanonicalMapping.lowercased()
            guard validPath(e.path), seen.insert(path).inserted, e.data.count <= maxEntry, bytes <= maxTotal else {
                throw SaveError("Unsafe, duplicate or oversized archive entry.")
            }
        }
        for name in seen {
            var parts = name.split(separator: "/"); parts.removeLast()
            while !parts.isEmpty {
                guard !seen.contains(parts.joined(separator: "/")) else { throw SaveError("Archive file and directory paths overlap.") }
                parts.removeLast()
            }
        }
    }
    static func crc(_ data: Data) -> UInt32 {
        data.withUnsafeBytes { UInt32(crc32(0, $0.bindMemory(to: Bytef.self).baseAddress, uInt(data.count))) }
    }
    static func deflate(_ input: Data, inflate: Bool, limit: Int = maxTotal) throws -> Data {
        var stream = z_stream()
        let status = inflate ? inflateInit2_(&stream, -MAX_WBITS, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) :
            deflateInit2_(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
        guard status == Z_OK else { throw SaveError("Could not open compressed data.") }
        defer { if inflate { inflateEnd(&stream) } else { deflateEnd(&stream) } }
        return try input.withUnsafeBytes { source in
            stream.next_in = UnsafeMutablePointer(mutating: source.bindMemory(to: Bytef.self).baseAddress)
            stream.avail_in = uInt(input.count)
            var result = Data(), chunk = [UInt8](repeating: 0, count: 32768)
            while true {
                let rc = chunk.withUnsafeMutableBytes { output -> Int32 in
                    stream.next_out = output.bindMemory(to: Bytef.self).baseAddress
                    stream.avail_out = 32768
                    return inflate ? zlib.inflate(&stream, Z_NO_FLUSH) : zlib.deflate(&stream, Z_FINISH)
                }
                let count = chunk.count - Int(stream.avail_out)
                guard result.count + count <= limit else { throw SaveError("Compressed data exceeds the size limit.") }
                result.append(contentsOf: chunk.prefix(count))
                if rc == Z_STREAM_END {
                    guard stream.avail_in == 0 else { throw SaveError("Trailing compressed data.") }
                    return result
                }
                guard rc == Z_OK, count > 0 else { throw SaveError("Damaged compressed data.") }
            }
        }
    }
    static func encode(_ entries: [SaveEntry]) throws -> Data {
        try validatePaths(entries)
        var local = Data(), central = Data()
        for entry in entries {
            let name = Data(entry.path.utf8), checksum = crc(entry.data), offset = local.count
            let c = Calendar(identifier: .gregorian).dateComponents([.year,.month,.day,.hour,.minute,.second], from: entry.modified)
            let date = (max(1980, min(2107, c.year!)) - 1980) << 9 | c.month! << 5 | c.day!
            let time = c.hour! << 11 | c.minute! << 5 | c.second! / 2
            var h = Data(repeating: 0, count: 30)
            h.put32(0, 0x04034b50); h.put16(4,20); h.put16(6,0x800); h.put16(10,time); h.put16(12,date)
            h.put32(14,checksum); h.put32(18,UInt32(entry.data.count)); h.put32(22,UInt32(entry.data.count)); h.put16(26,name.count)
            local.append(h); local.append(name); local.append(entry.data)
            var cd = Data(repeating: 0, count: 46)
            cd.put32(0,0x02014b50); cd.put16(4,0x31e); cd.put16(6,20); cd.put16(8,0x800); cd.put16(12,time); cd.put16(14,date)
            cd.put32(16,checksum); cd.put32(20,UInt32(entry.data.count)); cd.put32(24,UInt32(entry.data.count)); cd.put16(28,name.count)
            cd.put32(38,0o100644 << 16); cd.put32(42,UInt32(offset)); central.append(cd); central.append(name)
        }
        var end = Data(repeating: 0, count: 22)
        end.put32(0,0x06054b50); end.put16(8,entries.count); end.put16(10,entries.count)
        end.put32(12,UInt32(central.count)); end.put32(16,UInt32(local.count))
        local.append(central); local.append(end); return local
    }
    static func decode(_ data: Data) throws -> [SaveEntry] {
        func bad() -> SaveError { SaveError("Invalid or unsafe ZIP archive.") }
        guard data.count >= 22, data.count <= maxTotal + (8 << 20) else { throw bad() }
        guard let end = stride(from: data.count - 22, through: max(0,data.count - 65557), by: -1).first(where: {
            data.u32($0) == 0x06054b50 && $0 + 22 + data.u16($0 + 20) == data.count
        }) else { throw bad() }
        let count = data.u16(end + 10), start = Int(data.u32(end + 16))
        guard data.u32(end + 4) == 0, count == data.u16(end + 8), count <= maxEntries,
              start + Int(data.u32(end + 12)) == end else { throw bad() }
        var cursor = start, total = 0, entries = [SaveEntry](), ranges = [Range<Int>]()
        for _ in 0..<count {
            guard cursor >= 0, cursor + 46 <= end, data.u32(cursor) == 0x02014b50 else { throw bad() }
            let flags = data.u16(cursor + 8), method = data.u16(cursor + 10), packed = Int(data.u32(cursor + 20)), size = Int(data.u32(cursor + 24))
            let nl = data.u16(cursor + 28), extra = data.u16(cursor + 30), comment = data.u16(cursor + 32), local = Int(data.u32(cursor + 42))
            let mode = (data.u32(cursor + 38) >> 16) & 0xf000
            guard flags & ~0x808 == 0, method == 0 || method == 8, data.u16(cursor + 34) == 0,
                  mode == 0 || mode == 0x8000 || mode == 0x4000,
                  cursor + 46 + nl + extra + comment <= end,
                  let name = String(data: data[(cursor + 46)..<(cursor + 46 + nl)], encoding: .utf8),
                  local + 30 <= start, data.u32(local) == 0x04034b50,
                  data.u16(local + 6) == flags, data.u16(local + 8) == method, data.u16(local + 26) == nl else { throw bad() }
            let payload = local + 30 + nl + data.u16(local + 28)
            guard payload + packed <= start, Data(data[(local + 30)..<(local + 30 + nl)]) == Data(name.utf8),
                  size <= maxEntry, total + size <= maxTotal else { throw bad() }
            let range = local..<(payload + packed)
            guard !ranges.contains(where: { $0.overlaps(range) }) else { throw bad() }; ranges.append(range)
            if flags & 8 == 0 {
                guard data.u32(local + 14) == data.u32(cursor + 16), Int(data.u32(local + 18)) == packed,
                      Int(data.u32(local + 22)) == size else { throw bad() }
            }
            total += size
            let raw = Data(data[payload..<(payload + packed)])
            let content = method == 0 ? raw : try deflate(raw, inflate: true, limit: size)
            guard content.count == size, crc(content) == data.u32(cursor + 16) else { throw bad() }
            if name.hasSuffix("/") {
                guard size == 0, validPath(String(name.dropLast())) else { throw bad() }
            } else {
                guard mode != 0x4000 else { throw bad() }
                let day = data.u16(cursor + 14), time = data.u16(cursor + 12)
                let date = Calendar(identifier: .gregorian).date(from: DateComponents(year: 1980 + (day >> 9), month: (day >> 5) & 15, day: day & 31, hour: time >> 11, minute: (time >> 5) & 63, second: (time & 31) * 2)) ?? Date(timeIntervalSince1970: 0)
                entries.append(SaveEntry(path: name, data: content, modified: date))
            }
            cursor += 46 + nl + extra + comment
        }
        guard cursor == end else { throw bad() }; try validatePaths(entries); return entries
    }
}
