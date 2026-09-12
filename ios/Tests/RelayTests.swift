import Foundation

private let ONE_MIN: Int64 = 60_000
private let NOW: Int64 = 1_789_184_103_083 + ONE_MIN
private let ringConfirms = RingProtocol.parseRingLine("#STATUS sgv=204 dir=FortyFiveDown age_s=1 stale=0 alert=none up_s=9")!
private let ringHasNothing = RingProtocol.parseRingLine("#STATUS sgv=none dir= age_s=0 stale=1 alert=stale up_s=42")!

private func entry(age: Int64, sgv: Int = 204, dir: String = "FortyFiveDown") -> Entry {
    Entry(sgv: sgv, direction: dir, dateMs: NOW - age)
}

private func status(_ fetch: Fetch?, poke: Int64? = NOW, ring: RingProtocol.Parsed? = nil,
                    config: String? = nil, at now: Int64 = NOW) -> Relay.Status {
    var s = RelayState()
    if let poke { s.onPoke(nowMs: poke) }
    if let fetch { _ = s.onFetch(fetch, nowMs: NOW) }
    if let ring { s.onRingStatus(ring) }
    return s.status(nowMs: now, configProblem: config)
}

func relayTests() {
    section("relay: what is written back")
    expectEqual(Relay.lineToSend(entry(age: ONE_MIN), nowMs: NOW), "204,FortyFiveDown,\(NOW - ONE_MIN)\n", "a fresh reading")
    check(Relay.lineToSend(Entry(sgv: 204, direction: "Flat", dateMs: NOW - Relay.staleMs), nowMs: NOW) != nil,
          "age == 15 min is still sent")
    check(Relay.lineToSend(Entry(sgv: 204, direction: "Flat", dateMs: NOW - Relay.staleMs - 1), nowMs: NOW) == nil,
          "1 ms older is never sent")
    check(Relay.lineToSend(Entry(sgv: 204, direction: "Flat", dateMs: NOW + Relay.futureToleranceMs), nowMs: NOW) != nil,
          "skew within tolerance")
    check(Relay.lineToSend(Entry(sgv: 204, direction: "Flat", dateMs: NOW + Relay.futureToleranceMs + 1), nowMs: NOW) == nil,
          "future-dated is never sent")
    check(Relay.lineToSend(entry(age: ONE_MIN, sgv: 5), nowMs: NOW) == nil, "a sensor error code is never sent")

    section("relay: dedupe on date, per ring link")
    var r = RelayState()
    check(r.onFetch(.ok(entry(age: ONE_MIN)), nowMs: NOW) != nil, "first time: written")
    check(r.onFetch(.ok(entry(age: ONE_MIN)), nowMs: NOW + 5 * ONE_MIN) == nil, "same reading at the next poke: not re-sent")
    check(r.onFetch(.ok(entry(age: 0, sgv: 190)), nowMs: NOW) != nil, "a newer reading: written")
    r.linkConfirmed()
    check(r.onFetch(.ok(entry(age: 0, sgv: 190)), nowMs: NOW) != nil,
          "after a reconnect the current reading goes once more")
    for f in [Fetch.http(401), .http(500), .unreachable("timed out"), .badBody("x"), .ok(nil)] {
        var q = RelayState()
        check(q.onFetch(f, nowMs: NOW) == nil, "nothing written for \(f)")
    }

    section("relay: a revoked token is permanent, not a blip")
    var t = RelayState()
    _ = t.onFetch(.http(401), nowMs: NOW)
    expectEqual(t.tokenRejectedCode, 401, "401 recorded")
    _ = t.onFetch(.unreachable("timed out"), nowMs: NOW + ONE_MIN)
    expectEqual(t.tokenRejectedCode, 401, "a later network failure does not clear it")
    _ = t.onFetch(.http(500), nowMs: NOW + 2 * ONE_MIN)
    expectEqual(t.tokenRejectedCode, 401, "nor does a server error")
    t.onPoke(nowMs: NOW + 2 * ONE_MIN)
    expectEqual(t.status(nowMs: NOW + 2 * ONE_MIN, configProblem: nil).title, "Token rejected",
                "the UI shows it, distinctly from a network failure")
    _ = t.onFetch(.ok(entry(age: ONE_MIN)), nowMs: NOW)
    check(t.tokenRejectedCode == nil, "only a successful fetch clears it")
    var f403 = RelayState()
    _ = f403.onFetch(.http(403), nowMs: NOW)
    expectEqual(f403.tokenRejectedCode, 403, "403 too")

    section("relay: status")
    expectEqual(status(nil, poke: nil).title, "Waiting for the ring", "before any #REQ")
    let good = status(.ok(entry(age: ONE_MIN)), ring: ringConfirms)
    check(good.ok && good.title == "Relaying 204 FortyFiveDown", "healthy: \(good.title)")
    expectEqual(status(.ok(entry(age: ONE_MIN))).title, "Sending 204 FortyFiveDown to the ring", "until the ring confirms")
    expectEqual(status(.ok(entry(age: ONE_MIN)), ring: ringHasNothing).title, "Ring did not take the reading",
                "the ring disagrees")
    expectEqual(status(.http(401)).title, "Token rejected", "401")
    expectEqual(status(.http(500)).title, "Nightscout error", "500")
    expectEqual(status(.unreachable("timed out")).title, "Can't reach Nightscout", "unreachable")
    expectEqual(status(.badBody("x")).title, "Unexpected reply", "bad body")
    expectEqual(status(.ok(nil)).title, "No readings in Nightscout", "empty")
    expectEqual(status(.ok(entry(age: 23 * ONE_MIN))).title, "No current glucose data", "stale")
    check(status(.ok(entry(age: 23 * ONE_MIN))).detail.contains("23 min old"), "says how old")
    expectEqual(status(.ok(Entry(sgv: 204, direction: "Flat", dateMs: NOW + 3_600_000))).title,
                "Reading dated in the future", "future-dated")
    expectEqual(status(.ok(entry(age: ONE_MIN, sgv: 5))).title, "Not a usable reading", "error code")
    expectEqual(status(.ok(entry(age: ONE_MIN)), ring: ringConfirms, at: NOW + 17 * ONE_MIN).title, "Ring not asking",
                "no #REQ for 17 min")
    expectEqual(status(.ok(entry(age: ONE_MIN)), ring: ringConfirms, config: "not set").title, "Not configured",
                "a config problem comes first")

    section("relay: 'Relaying' only when everything is true (exhaustive)")
    let fetches: [(String, Fetch?)] = [
        ("none", nil), ("fresh", .ok(entry(age: ONE_MIN))), ("stale", .ok(entry(age: 20 * ONE_MIN))),
        ("empty", .ok(nil)), ("code", .ok(entry(age: ONE_MIN, sgv: 5))), ("401", .http(401)), ("403", .http(403)),
        ("500", .http(500)), ("net", .unreachable("x")), ("body", .badBody("x")),
    ]
    let pokes: [(String, Int64?)] = [("never", nil), ("recent", NOW), ("long ago", NOW - 20 * ONE_MIN)]
    let rings: [(String, RingProtocol.Parsed?)] = [("silent", nil), ("confirms", ringConfirms), ("disagrees", ringHasNothing)]
    let configs: [(String, String?)] = [("ok", nil), ("bad", "not set")]
    var wrong = 0, total = 0
    for (fn, f) in fetches {
        for (pn, p) in pokes {
            for (rn, ring) in rings {
                for (cn, c) in configs {
                    let s = status(f, poke: p, ring: ring, config: c)
                    let expected = fn == "fresh" && pn == "recent" && rn == "confirms" && cn == "ok"
                    total += 1
                    if s.ok != expected || s.title.hasPrefix("Relaying") != expected {
                        wrong += 1
                        print("    \(fn)/\(pn)/\(rn)/\(cn) -> \(s.title)")
                    }
                }
            }
        }
    }
    expectEqual(wrong, 0, "wrong combinations out of \(total)")

    section("relay: backoff for connect-then-drop loops")
    var b = Backoff(capMs: 8_000)
    expectEqual((0..<6).map { _ in b.next(random: 1.0) }, [1_000, 2_000, 4_000, 8_000, 8_000, 8_000], "doubles to the 8 s cap")
    b.reset()
    expectEqual(b.next(random: 1.0), 1_000, "reset")
    var j = Backoff(capMs: 8_000)
    expectEqual(j.next(random: 0.0), 500, "jitter floor is half a step")

    section("relay: config")
    check(Relay.configProblem(url: "https://x", token: Relay.tokenPlaceholder) != nil, "placeholder token")
    check(Relay.configProblem(url: "https://x", token: "  ") != nil, "blank token")
    check(Relay.configProblem(url: "http://x", token: "t") != nil, "not https")
    check(Relay.configProblem(url: "", token: "t") != nil, "no URL")
    check(Relay.configProblem(url: "https://x", token: "t") == nil, "valid")
}
