import Foundation

/// One Nightscout sgv entry, reduced to the three fields the ring takes.
struct Entry: Equatable {
    let sgv: Int
    let direction: String
    let dateMs: Int64
}

/// Outcome of one fetch. Pure data, so the relay rules are testable offline.
enum Fetch: Equatable {
    /// HTTP 200 and a well-formed body; nil when Nightscout has no sgv entries.
    case ok(Entry?)
    /// Any status but 200 -- 401/403 means the token is wrong or revoked.
    case http(Int)
    /// No HTTP response at all. Fixed, token-free text.
    case unreachable(String)
    /// HTTP 200, but not the body Nightscout sends.
    case badBody(String)
}

/// Parser for `GET /api/v1/entries/sgv.json?count=1`, verified against the
/// live instance (PHONE_APP_REPORT.md).
enum Nightscout {
    static let path = "/api/v1/entries/sgv.json"

    /// Strict on every field that reaches the ring: `sgv` and `date` must be
    /// whole JSON numbers and `date` must be epoch milliseconds. A string
    /// "204", 204.5, `true`, or a seconds timestamp is refused, never coerced --
    /// a reinterpreted value is a fabricated reading.
    static func parse(_ body: Data) -> Fetch {
        guard let top = try? JSONSerialization.jsonObject(with: body, options: []),
              let list = top as? [Any] else { return .badBody("not a JSON list") }
        guard let first = list.first else { return .ok(nil) }
        guard let o = first as? [String: Any] else { return .badBody("entry is not an object") }

        if let type = o["type"], !(type is NSNull), (type as? String) != "sgv" {
            return .badBody("entry is not an sgv")
        }
        guard let sgv = wholeNumber(o["sgv"]) else { return .badBody("sgv missing or not a whole number") }
        guard sgv >= Int64(Int32.min), sgv <= Int64(Int32.max) else { return .badBody("sgv out of range") }
        guard let date = wholeNumber(o["date"]) else { return .badBody("date missing or not a whole number") }
        guard date >= RingProtocol.epochMsMin else { return .badBody("date is not epoch milliseconds") }

        // Nightscout allows direction to be absent; the ring accepts an empty one.
        return .ok(Entry(sgv: Int(sgv), direction: o["direction"] as? String ?? "", dateMs: date))
    }

    /// A JSON number with no fractional part. 204.0 is 204; "204", 204.5 and true are not.
    private static func wholeNumber(_ v: Any?) -> Int64? {
        guard let n = v as? NSNumber else { return nil }
        if CFGetTypeID(n) == CFBooleanGetTypeID() { return nil }   // JSON true/false
        let d = n.doubleValue
        guard d.isFinite, d == d.rounded(), abs(d) <= 9.0e15 else { return nil }
        return Int64(d)
    }
}
