import Foundation

/// Writes the formatter's exact bytes for a sweep of inputs, so
/// tests/run_wire_crosscheck.sh can decode them with the firmware's own
/// rp_parse() (tests/wire_crosscheck.cpp).
///
///   lines.bin     every wire line, concatenated exactly as written
///   expected.tsv  sgv<TAB>direction<TAB>epoch_ms the ring should decode
func exportWire(to dir: URL) throws -> Int {
    let directions = [
        "", "NONE", "DoubleUp", "SingleUp", "FortyFiveUp", "Flat", "FortyFiveDown",
        "SingleDown", "DoubleDown", "NOT COMPUTABLE", "RATE OUT OF RANGE",
        "Fla,t", String(repeating: "x", count: 40), "tab\there", "\u{00FC}n\u{00EF}code", "cr\rlf\n",
    ]
    let epochs: [Int64] = [RingProtocol.epochMsMin, 1_789_184_103_083, Int64.max]
    var bytes: [UInt8] = []
    var expected = ""
    var count = 0
    for epoch in epochs {
        for d in directions {
            for sgv in RingProtocol.sgvMin...RingProtocol.sgvMax {
                guard let line = RingProtocol.formatReading(sgv: sgv, epochMs: epoch, direction: d) else {
                    check(false, "export: \(sgv)/\(d)/\(epoch) did not format")
                    continue
                }
                bytes.append(contentsOf: line.utf8)
                expected += "\(sgv)\t\(RingProtocol.sanitizeDirection(d))\t\(epoch)\n"
                count += 1
            }
        }
    }
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    try Data(bytes).write(to: dir.appendingPathComponent("lines.bin"))
    try Data(expected.utf8).write(to: dir.appendingPathComponent("expected.tsv"))
    return count
}
