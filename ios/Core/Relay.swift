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
        switch f {
        case .http(let code) where code == 401 || code == 403: tokenRejectedCode = code
        case .ok:                                              tokenRejectedCode = nil
        default:                                               break   // neither sets nor clears it
        }
        guard case .ok(let entry) = f, let e = entry, e.dateMs > sentOnLink,
              let line = Relay.lineToSend(e, nowMs: nowMs) else { return nil }
        sentOnLink = e.dateMs
        ringStatus = nil
        return line
    }

    func status(nowMs: Int64, configProblem: String?) -> Relay.Status {
        if let p = configProblem {
            return .init(ok: false, title: "Not configured", detail: p)
        }
        if let code = tokenRejectedCode {
            return .init(ok: false, title: "Token rejected",
                         detail: "Nightscout refused the token (HTTP \(code)): it is wrong or has been revoked. " +
                                 "This will not fix itself. Put a valid read-only token in Secrets.plist and " +
                                 "reinstall. Until then the ring will buzz STALE.")
        }
        guard let poke = lastPokeAtMs else {
            return .init(ok: false, title: "Waiting for the ring",
                         detail: "Once connected, the ring asks for a reading every ~5 minutes.")
        }
        if nowMs - poke > Relay.ringSilentMs {
            return .init(ok: false, title: "Ring not asking",
                         detail: "No request from the ring for \(Self.minutes(nowMs - poke)) min. It may be out " +
                                 "of range, off, or disconnected; it will buzz MISS.")
        }
        switch lastFetch {
        case nil:
            return .init(ok: false, title: "Waiting for Nightscout", detail: "Fetching.")
        case .unreachable(let why)?:
            return .init(ok: false, title: "Can't reach Nightscout",
                         detail: "Can't reach Nightscout (\(why)). Tried again at the ring's next request.")
        case .http(let code)?:
            return .init(ok: false, title: "Nightscout error",
                         detail: "Nightscout returned HTTP \(code). Tried again at the ring's next request.")
        case .badBody(let why)?:
            return .init(ok: false, title: "Unexpected reply", detail: "Nightscout's reply was not understood (\(why)).")
        case .ok(nil)?:
            return .init(ok: false, title: "No readings in Nightscout", detail: "Nightscout returned no glucose readings.")
        case .ok(let e?)?:
            let age = nowMs - e.dateMs
            if e.dateMs > nowMs + Relay.futureToleranceMs {
                return .init(ok: false, title: "Reading dated in the future", detail: "Check the phone's date and time.")
            }
            if age > Relay.staleMs {
                return .init(ok: false, title: "No current glucose data",
                             detail: "Newest reading (\(e.sgv)) is \(Self.minutes(age)) min old. New CGM data is " +
                                     "not reaching Nightscout; the ring will buzz STALE.")
            }
            if RingProtocol.formatReading(sgv: e.sgv, epochMs: e.dateMs, direction: e.direction) == nil {
                return .init(ok: false, title: "Not a usable reading",
                             detail: "Nightscout's newest value (\(e.sgv)) is not a glucose reading.")
            }
            let label = [String(e.sgv), RingProtocol.sanitizeDirection(e.direction)]
                .filter { !$0.isEmpty }.joined(separator: " ")
            guard let ring = ringStatus else {
                return .init(ok: false, title: "Sending \(label) to the ring",
                             detail: "\(Self.ageText(age)) old. Waiting for the ring to confirm.")
            }
            if ring.fields["sgv"] == String(e.sgv), ring.fields["stale"] == "0" {
                return .init(ok: true, title: "Relaying \(label)", detail: "\(Self.ageText(age)) old. The ring confirms it.")
            }
            return .init(ok: false, title: "Ring did not take the reading",
                         detail: "The ring reports sgv=\(ring.fields["sgv"] ?? "?") stale=\(ring.fields["stale"] ?? "?").")
        }
    }

    private static func minutes(_ ms: Int64) -> Int64 { ms / 60_000 }
    private static func ageText(_ ms: Int64) -> String { ms < 60_000 ? "Under a minute" : "\(ms / 60_000) min" }
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
