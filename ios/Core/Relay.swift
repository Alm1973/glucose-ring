import Foundation

/// App-side relay rules. Pure (Foundation only) and unit-tested by
/// ios/run_core_tests.sh. No threshold or alarm logic: that is the ring's job.
///
/// The dangerous failure is looking healthy while relaying nothing, so the
/// status says "Relaying" only when a current reading reached the ring and the
/// ring's own #STATUS confirms it.
enum Relay {
    /// The ring's default stale window (`!GET STATUS` stale window, 15 min).
    static let staleMs: Int64 = 15 * 60_000
    /// Clock disagreement tolerated before a reading counts as future-dated.
    static let futureToleranceMs: Int64 = 2 * 60_000
    /// The ring pokes every ~5 min; this long without a #REQ means it has stopped.
    static let ringSilentMs: Int64 = 16 * 60_000
    static let tokenPlaceholder = "PASTE_READ_ONLY_TOKEN_HERE"

    struct Status: Equatable {
        let ok: Bool
        let title: String
        let detail: String
    }

    /// Why the bundled Secrets.plist can't be used, or nil if it can.
    static func configProblem(url: String, token: String) -> String? {
        if token.trimmingCharacters(in: .whitespaces).isEmpty || token == tokenPlaceholder {
            return "The Nightscout token is not set (NightscoutToken in ios/GlucoseRing/Secrets.plist)."
        }
        if !url.hasPrefix("https://") {
            return "The Nightscout URL is missing or not https (NightscoutURL in ios/GlucoseRing/Secrets.plist)."
        }
        return nil
    }

    /// The reading line to write back, or nil. Only a current, valid reading.
    /// The ring re-judges it against trusted time; this is defence in depth.
    static func lineToSend(_ e: Entry, nowMs: Int64) -> String? {
        guard e.dateMs <= nowMs + futureToleranceMs, nowMs - e.dateMs <= staleMs else { return nil }
        return RingProtocol.formatReading(sgv: e.sgv, epochMs: e.dateMs, direction: e.direction)
    }

    /// What one fetch says about the data, ring aside: nil for a current,
    /// usable reading, otherwise the problem in plain words. The status screen
    /// and the Test Nightscout button both use this, so they never disagree.
    /// A 401/403 is reported by the callers, because the status keeps it sticky.
    static func dataProblem(_ fetch: Fetch, nowMs: Int64) -> Status? {
        switch fetch {
        case .unreachable(let why):
            return .init(ok: false, title: "Can't reach Nightscout",
                         detail: "Can't reach Nightscout (\(why)). Tried again at the ring's next request.")
        case .http(let code):
            return .init(ok: false, title: "Nightscout error",
                         detail: "Nightscout returned HTTP \(code). Tried again at the ring's next request.")
        case .badBody(let why):
            return .init(ok: false, title: "Unexpected reply", detail: "Nightscout's reply was not understood (\(why)).")
        case .ok(nil):
            return .init(ok: false, title: "No readings in Nightscout", detail: "Nightscout returned no glucose readings.")
        case .ok(let e?):
            let age = nowMs - e.dateMs
            if e.dateMs > nowMs + futureToleranceMs {
                return .init(ok: false, title: "Reading dated in the future", detail: "Check the phone's date and time.")
            }
            if age > staleMs {
                return .init(ok: false, title: "No current glucose data",
                             detail: "Newest reading (\(e.sgv)) is \(minutes(age)) min old. New CGM data is " +
                                     "not reaching Nightscout; the ring will buzz STALE.")
            }
            if RingProtocol.formatReading(sgv: e.sgv, epochMs: e.dateMs, direction: e.direction) == nil {
                return .init(ok: false, title: "Not a usable reading",
                             detail: "Nightscout's newest value (\(e.sgv)) is not a glucose reading.")
            }
            return nil
        }
    }

    static func tokenRejected(_ code: Int) -> Status {
        .init(ok: false, title: "Token rejected",
              detail: "Nightscout refused the token (HTTP \(code)): it is wrong or has been revoked. " +
                      "This will not fix itself. Put a valid read-only token in Secrets.plist and " +
                      "reinstall. Until then the ring will buzz STALE.")
    }

    /// The Test Nightscout button: one real, read-only fetch. Nothing is
    /// written to the ring, and a test never counts as a ring request.
    static func testStatus(_ fetch: Fetch, nowMs: Int64) -> Status {
        if case .http(let code) = fetch, code == 401 || code == 403 { return tokenRejected(code) }
        if let problem = dataProblem(fetch, nowMs: nowMs) { return problem }
        guard case .ok(let e?) = fetch else {   // dataProblem covers every other case
            return .init(ok: false, title: "Nightscout error", detail: "No reading.")
        }
        return .init(ok: true, title: "Nightscout works",
                     detail: "Latest reading \(label(e)), \(ageText(nowMs - e.dateMs)) old, and the token is " +
                             "accepted. Readings will reach the ring once it is connected.")
    }

    static func label(_ e: Entry) -> String {
        [String(e.sgv), RingProtocol.sanitizeDirection(e.direction)].filter { !$0.isEmpty }.joined(separator: " ")
    }

    static func minutes(_ ms: Int64) -> Int64 { ms / 60_000 }

    static func ageText(_ ms: Int64, capitalized: Bool = false) -> String {
        ms < 60_000 ? (capitalized ? "Under a minute" : "under a minute") : "\(ms / 60_000) min"
    }
}

struct RelayState {
    /// Sticky: a 401/403 stays until a fetch succeeds. A revoked token is a
    /// permanent failure and must not look like a network blip.
    private(set) var tokenRejectedCode: Int?
    private(set) var lastFetch: Fetch?
    private(set) var lastPokeAtMs: Int64?
    /// `date` of the newest reading written on the current ring link.
    private(set) var sentOnLink: Int64 = 0
    /// The ring's own #STATUS since our last write.
    private(set) var ringStatus: RingProtocol.Parsed?

    /// A link was confirmed (#BOOT/#VER/#REQ). The ring may have rebooted, so
    /// the current reading may be written once more; the ring drops duplicates.
    mutating func linkConfirmed() {
        sentOnLink = 0
        ringStatus = nil
    }

    mutating func onPoke(nowMs: Int64) { lastPokeAtMs = nowMs }

    mutating func onRingStatus(_ p: RingProtocol.Parsed) {
        if p.kind == "STATUS" { ringStatus = p }
    }

    /// Record a fetch; returns the reading line to write back, if any.
    mutating func onFetch(_ f: Fetch, nowMs: Int64) -> String? {
        lastFetch = f
        noteToken(f)
        guard case .ok(let entry) = f, let e = entry, e.dateMs > sentOnLink,
              let line = Relay.lineToSend(e, nowMs: nowMs) else { return nil }
        sentOnLink = e.dateMs
        ringStatus = nil
        return line
    }

    /// The Test Nightscout button. A 401/403 is as permanent as ever, so it is
    /// recorded; nothing else changes. No line is sent and no poke is implied.
    mutating func onTestFetch(_ f: Fetch) { noteToken(f) }

    private mutating func noteToken(_ f: Fetch) {
        switch f {
        case .http(let code) where code == 401 || code == 403: tokenRejectedCode = code
        case .ok:                                              tokenRejectedCode = nil
        default:                                               break   // neither sets nor clears it
        }
    }

    func status(nowMs: Int64, configProblem: String?) -> Relay.Status {
        if let p = configProblem {
            return .init(ok: false, title: "Not configured", detail: p)
        }
        if let code = tokenRejectedCode { return Relay.tokenRejected(code) }
        guard let poke = lastPokeAtMs else {
            return .init(ok: false, title: "Waiting for the ring",
                         detail: "Once connected, the ring asks for a reading every ~5 minutes.")
        }
        if nowMs - poke > Relay.ringSilentMs {
            return .init(ok: false, title: "Ring not asking",
                         detail: "No request from the ring for \(Relay.minutes(nowMs - poke)) min. It may be out " +
                                 "of range, off, or disconnected; it will buzz MISS.")
        }
        guard let fetch = lastFetch else {
            return .init(ok: false, title: "Waiting for Nightscout", detail: "Fetching.")
        }
        if let problem = Relay.dataProblem(fetch, nowMs: nowMs) { return problem }
        guard case .ok(let e?) = fetch else {   // dataProblem covers every other case
            return .init(ok: false, title: "Nightscout error", detail: "No reading.")
        }
        let age = nowMs - e.dateMs
        let label = Relay.label(e)
        guard let ring = ringStatus else {
            return .init(ok: false, title: "Sending \(label) to the ring",
                         detail: "\(Relay.ageText(age, capitalized: true)) old. Waiting for the ring to confirm.")
        }
        if ring.fields["sgv"] == String(e.sgv), ring.fields["stale"] == "0" {
            return .init(ok: true, title: "Relaying \(label)",
                         detail: "\(Relay.ageText(age, capitalized: true)) old. The ring confirms it.")
        }
        return .init(ok: false, title: "Ring did not take the reading",
                     detail: "The ring reports sgv=\(ring.fields["sgv"] ?? "?") stale=\(ring.fields["stale"] ?? "?").")
    }
}

/// "#BATT mv=3942 pct=62 chg=0" -> "3.942 V (3942 mV) - 62% - on battery".
/// The raw mV is shown alongside the volts: it is the number
/// `VBAT_DIVIDER_COMP` is calibrated against, and what the low-voltage
/// cutoff depends on. Anything missing or unparseable reads "?" rather than
/// being guessed.
extension Relay {
    static func batteryText(_ f: [String: String]) -> String {
        let chg: String
        switch f["chg"] {
        case "1":  chg = "on charger"
        case "0":  chg = "on battery"
        default:   chg = "charge state ?"
        }
        // The ring skips its divider while the charger drives that node, so
        // there is often no reading at all. A percentage computed from 0 mV
        // would render as a flat battery, which is worse than saying nothing.
        guard let mv = Int(f["mv"] ?? ""), mv > 0 else {
            return f["chg"] == "1" ? "not sampled while charging - \(chg)"
                                   : "no reading - \(chg)"
        }
        let pct = Int(f["pct"] ?? "").map { "\($0)%" } ?? "?%"
        return String(format: "%.3f V (%d mV)", Double(mv) / 1000.0, mv) + " - \(pct) - \(chg)"
    }
}

/// Delay for connect-then-drop loops: 1 s doubling to a cap, with jitter
/// (50-100 % of the step). A plain disconnect reconnects at once instead.
struct Backoff {
    let startMs: Int
    let capMs: Int
    private var attempt = 0

    init(startMs: Int = 1_000, capMs: Int) {
        self.startMs = startMs
        self.capMs = capMs
    }

    mutating func next(random: Double = Double.random(in: 0..<1)) -> Int {
        let base = min(capMs, startMs << min(attempt, 20))
        attempt += 1
        return Int(Double(base) * (0.5 + 0.5 * random))
    }

    mutating func reset() { attempt = 0 }
}
