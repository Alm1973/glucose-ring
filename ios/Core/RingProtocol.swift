import Foundation

/// The ring's wire protocol (PROTOCOL.md, protocol 3). Pure: Foundation only,
/// so it is unit-tested without Xcode (ios/run_core_tests.sh).
enum RingProtocol {
    static let sgvMin = 20
    static let sgvMax = 600
    static let epochMsMin: Int64 = 1_600_000_000_000
    static let directionMax = 23

    /// The wire line for a reading, or nil if it must not be sent. Returning nil
    /// rather than a best-effort string is deliberate: the ring drops a malformed
    /// line silently, so a formatting bug would look exactly like a dead link.
    static func formatReading(sgv: Int, epochMs: Int64, direction: String?) -> String? {
        guard sgv >= sgvMin, sgv <= sgvMax, epochMs >= epochMsMin else { return nil }
        return "\(sgv),\(sanitizeDirection(direction)),\(epochMs)\n"
    }

    /// A comma would make a fourth field and the ring would drop the line;
    /// non-printables are rejected there too; 24+ characters is an error on the
    /// ring, not a truncation -- so trim to 23 here.
    static func sanitizeDirection(_ raw: String?) -> String {
        guard let raw else { return "" }
        var out = String.UnicodeScalarView()
        var kept = 0
        for s in raw.unicodeScalars where s.value >= 0x20 && s.value <= 0x7E && s != "," {
            if kept == directionMax { break }
            out.append(s)
            kept += 1
        }
        return String(out)
    }

    // ---- phone -> ring ----
    static let version   = "!VER\n"
    static let getStatus = "!GET STATUS\n"
    static let getBatt   = "!GET BATT\n"
    /// Sent first in every answer to #REQ: the ring's trusted clock and its
    /// proof that the phone is alive (PROTOCOL.md §9).
    static func time(_ epochMs: Int64) -> String { "!TIME \(epochMs)\n" }

    // ---- ring -> phone ----
    struct Parsed: Equatable {
        let kind: String
        let fields: [String: String]
    }

    /// "#STATUS sgv=112 dir=Flat" -> kind + fields. Bare tokens keep an empty value.
    static func parseRingLine(_ line: String) -> Parsed? {
        guard line.hasPrefix("#") else { return nil }
        let parts = line.dropFirst().split(separator: " ")
        guard let kind = parts.first else { return nil }
        var fields: [String: String] = [:]
        for p in parts.dropFirst() {
            if let eq = p.firstIndex(of: "="), eq != p.startIndex {
                fields[String(p[..<eq])] = String(p[p.index(after: eq)...])
            } else {
                fields[String(p)] = ""
            }
        }
        return Parsed(kind: String(kind), fields: fields)
    }

    /// Reassembles newline-delimited lines from arbitrarily chunked notifications.
    struct LineAssembler {
        private var buf: [UInt8] = []
        private let limit: Int

        init(limit: Int = 512) { self.limit = limit }

        mutating func feed(_ chunk: [UInt8]) -> [String] {
            var out: [String] = []
            buf.append(contentsOf: chunk)
            while let nl = buf.firstIndex(of: 0x0A) {
                var line = Array(buf[..<nl])
                buf.removeSubrange(...nl)
                if line.last == 0x0D { line.removeLast() }
                if !line.isEmpty { out.append(String(decoding: line, as: UTF8.self)) }
            }
            if buf.count > limit { buf.removeAll() }   // a wedged link must not grow this forever
            return out
        }
    }

    /// Split a line into writes of at most `maxLength` bytes, never under 20
    /// (the ATT minimum). The ring reassembles by newline.
    static func chunks(_ line: String, maxLength: Int) -> [[UInt8]] {
        let bytes = Array(line.utf8)
        let n = max(20, maxLength)
        return stride(from: 0, to: bytes.count, by: n).map { Array(bytes[$0 ..< min($0 + n, bytes.count)]) }
    }
}
