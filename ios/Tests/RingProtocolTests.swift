import Foundation

func ringProtocolTests() {
    let T: Int64 = 1_789_238_400_000

    section("protocol: formatting (PROTOCOL.md section 8 vectors)")
    expectEqual(RingProtocol.formatReading(sgv: 112, epochMs: T, direction: "Flat"), "112,Flat,\(T)\n", "typical")
    expectEqual(RingProtocol.formatReading(sgv: 20, epochMs: RingProtocol.epochMsMin, direction: ""),
                "20,,1600000000000\n", "min sgv, empty direction, epoch floor")
    expectEqual(RingProtocol.formatReading(sgv: 600, epochMs: RingProtocol.epochMsMin, direction: "Flat"),
                "600,Flat,1600000000000\n", "max sgv")
    expectEqual(RingProtocol.formatReading(sgv: 95, epochMs: T, direction: "RATE OUT OF RANGE"),
                "95,RATE OUT OF RANGE,\(T)\n", "spaces survive")
    expectEqual(RingProtocol.formatReading(sgv: 112, epochMs: T, direction: nil), "112,,\(T)\n", "nil direction")
    for sgv in [19, 601, 0, -5] {
        check(RingProtocol.formatReading(sgv: sgv, epochMs: T, direction: "Flat") == nil, "sgv \(sgv) refused")
    }
    check(RingProtocol.formatReading(sgv: 112, epochMs: RingProtocol.epochMsMin - 1, direction: "Flat") == nil,
          "below the epoch floor refused")
    check(RingProtocol.formatReading(sgv: 112, epochMs: 1_789_238_400, direction: "Flat") == nil, "seconds refused")

    section("protocol: direction hygiene")
    expectEqual(RingProtocol.sanitizeDirection("Fla,t"), "Flat", "comma stripped")
    expectEqual(RingProtocol.sanitizeDirection("Fla\tt"), "Flat", "tab")
    expectEqual(RingProtocol.sanitizeDirection("Fla\u{01}t"), "Flat", "control byte")
    expectEqual(RingProtocol.sanitizeDirection("Fla\u{7F}t"), "Flat", "DEL")
    expectEqual(RingProtocol.sanitizeDirection("Fla\u{FF}t"), "Flat", "high byte")
    expectEqual(RingProtocol.sanitizeDirection("Fla\nt"), "Flat", "newline")
    expectEqual(RingProtocol.sanitizeDirection(String(repeating: "x", count: 40)).count,
                RingProtocol.directionMax, "trimmed to 23")
    if let line = RingProtocol.formatReading(sgv: 112, epochMs: T, direction: "Fla,t\n,x") {
        expectEqual(line.filter { $0 == "," }.count, 2, "hostile direction: exactly two commas")
        expectEqual(line.filter { $0 == "\n" }.count, 1, "hostile direction: exactly one newline")
    } else {
        check(false, "hostile direction still formats")
    }

    section("protocol: commands")
    expectEqual(RingProtocol.time(1_789_184_103_083), "!TIME 1789184103083\n", "!TIME")
    expectEqual(RingProtocol.getStatus, "!GET STATUS\n", "!GET STATUS")
    expectEqual(RingProtocol.version, "!VER\n", "!VER")

    section("protocol: ring lines")
    let s = RingProtocol.parseRingLine("#STATUS sgv=112 dir=Flat age_s=42 stale=0 alert=none up_s=3600")
    expectEqual(s?.kind, "STATUS", "STATUS kind")
    expectEqual(s?.fields["sgv"], "112", "STATUS sgv")
    expectEqual(s?.fields["stale"], "0", "STATUS stale")
    expectEqual(RingProtocol.parseRingLine("#REQ")?.kind, "REQ", "#REQ")
    expectEqual(RingProtocol.parseRingLine("#LINK misses=2 clock=1 next_poke_s=5")?.fields["misses"], "2", "#LINK")
    check(RingProtocol.parseRingLine("#ALERT miss")?.fields["miss"] == "", "bare token kept")
    check(RingProtocol.parseRingLine("112,Flat,\(T)") == nil, "a reading is not a ring line")
    check(RingProtocol.parseRingLine("") == nil && RingProtocol.parseRingLine("#") == nil, "empty lines")

    section("protocol: reassembly and chunking")
    var a = RingProtocol.LineAssembler()
    check(a.feed(Array("#STATUS sgv=1".utf8)).isEmpty, "partial line held")
    check(a.feed(Array("12 dir=Flat".utf8)).isEmpty, "still partial")
    expectEqual(a.feed(Array(" alert=none\n".utf8)), ["#STATUS sgv=112 dir=Flat alert=none"], "joined across chunks")
    expectEqual(a.feed(Array("#REQ\n#OK TIME\r\n".utf8)), ["#REQ", "#OK TIME"], "two lines in one chunk, CRLF")
    var g = RingProtocol.LineAssembler(limit: 64)
    _ = g.feed(Array(repeating: 0x78, count: 200))
    expectEqual(g.feed(Array("#REQ\n".utf8)), ["#REQ"], "unbounded garbage discarded")

    let line = "204,FortyFiveDown,1789184103083\n"
    let c20 = RingProtocol.chunks(line, maxLength: 20)
    expectEqual(c20.map { $0.count }, [20, 12], "32 bytes -> 20 + 12 at the default MTU")
    expectEqual(c20.flatMap { $0 }, Array(line.utf8), "chunks rejoin exactly")
    expectEqual(RingProtocol.chunks(line, maxLength: 5).first?.count, 20, "never under 20")
    expectEqual(RingProtocol.chunks(line, maxLength: 182).count, 1, "one write at a larger MTU")
}
