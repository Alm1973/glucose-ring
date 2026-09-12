import Foundation

func nightscoutTests() {
    // Verbatim body from the live instance, 2026-09-12 (PHONE_APP_REPORT.md).
    let live = #"[{"_id":"6aa4c87aa24de4b1501f5b01","type":"sgv","sysTime":"2026-09-12T03:35:03.083Z","date":1789184103083,"dateString":"2026-09-12T03:35:03.083Z","device":"nightscout-connect","direction":"FortyFiveDown","sgv":204,"trend":5,"utcOffset":0,"mills":1789184103083}]"#
    let D: Int64 = 1_789_184_103_083
    func parse(_ s: String) -> Fetch { Nightscout.parse(Data(s.utf8)) }
    func isBad(_ s: String) -> Bool {
        if case .badBody = parse(s) { return true }
        return false
    }

    section("nightscout: the live response")
    expectEqual(parse(live), .ok(Entry(sgv: 204, direction: "FortyFiveDown", dateMs: D)), "live body")
    if case .ok(let e?) = parse(live) {
        expectEqual(RingProtocol.formatReading(sgv: e.sgv, epochMs: e.dateMs, direction: e.direction),
                    "204,FortyFiveDown,1789184103083\n", "live body -> the documented wire line")
    } else {
        check(false, "live body parses")
    }

    section("nightscout: accepted")
    expectEqual(parse("[]"), .ok(nil), "empty list means no readings")
    expectEqual(parse(#"[{"sgv":100,"date":\#(D)}]"#), .ok(Entry(sgv: 100, direction: "", dateMs: D)),
                "missing type and direction")
    expectEqual(parse(#"[{"sgv":100,"date":\#(D),"direction":null}]"#), .ok(Entry(sgv: 100, direction: "", dateMs: D)),
                "null direction becomes empty")
    expectEqual(parse(#"[{"sgv":100,"date":\#(D),"direction":"NOT COMPUTABLE"}]"#),
                .ok(Entry(sgv: 100, direction: "NOT COMPUTABLE", dateMs: D)), "direction with spaces")
    expectEqual(parse(#"[{"sgv":204.0,"date":\#(D)}]"#), .ok(Entry(sgv: 204, direction: "", dateMs: D)),
                "204.0 is a whole number")
    expectEqual(parse(#"[{"sgv":5,"date":\#(D)}]"#), .ok(Entry(sgv: 5, direction: "", dateMs: D)),
                "an error code parses; the relay refuses to send it")

    section("nightscout: refused, never coerced")
    let bad: [(String, String)] = [
        (#"[{"sgv":"204","date":\#(D)}]"#, "sgv as a string"),
        (#"[{"sgv":204.5,"date":\#(D)}]"#, "fractional sgv"),
        (#"[{"date":\#(D)}]"#, "missing sgv"),
        (#"[{"sgv":null,"date":\#(D)}]"#, "null sgv"),
        (#"[{"sgv":true,"date":\#(D)}]"#, "boolean sgv"),
        (#"[{"sgv":4294967500,"date":\#(D)}]"#, "sgv overflows Int32"),
        (#"[{"sgv":204,"date":1789184103}]"#, "date in seconds"),
        (#"[{"sgv":204,"date":"2026-09-12T03:35:03.083Z"}]"#, "date as a string"),
        (#"[{"sgv":204}]"#, "missing date"),
        (#"[{"sgv":204,"date":1789184103083.5}]"#, "fractional date"),
        (#"[{"type":"mbg","sgv":204,"date":\#(D)}]"#, "not an sgv entry"),
        ("", "empty body"),
        ("{}", "an object, not a list"),
        ("null", "null"),
        ("<html>502 Bad Gateway</html>", "an HTML error page"),
        ("[1]", "a number entry"),
        ("[null]", "a null entry"),
        (#"["sgv"]"#, "a string entry"),
    ]
    for (body, name) in bad { check(isBad(body), "refuse \(name)") }

    section("nightscout: every truncation of the live body")
    let bytes = Array(live.utf8)
    var accepted = 0
    for n in 0..<bytes.count {
        if case .badBody = Nightscout.parse(Data(bytes[0..<n])) {} else { accepted += 1 }
    }
    expectEqual(accepted, 0, "prefixes accepted, out of \(bytes.count)")
}
