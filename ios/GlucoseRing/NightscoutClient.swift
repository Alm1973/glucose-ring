import Foundation

/// One GET per ring request, sized for the few seconds a background app gets.
/// Read-only: this app never writes to Nightscout.
///
/// The token travels as `?token=`, the form verified against this server. It
/// must never reach a log. Nothing here logs, and errors are reduced to fixed
/// text, because a URLError carries the full failing URL -- token included --
/// in its userInfo.
final class NightscoutClient {
    private let url: URL?
    private let session: URLSession

    init(config: Config) {
        var base = config.url
        while base.hasSuffix("/") { base.removeLast() }
        var comps = URLComponents(string: base + Nightscout.path)
        let token = config.token.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? ""
        comps?.percentEncodedQuery = "count=1&token=" + token
        url = comps?.url

        let c = URLSessionConfiguration.ephemeral   // no disk cache, no stored cookies
        c.timeoutIntervalForRequest = 5
        c.timeoutIntervalForResource = 6
        c.waitsForConnectivity = false
        c.urlCache = nil
        session = URLSession(configuration: c)
    }

    /// One attempt, no retry: the ring's next #REQ is the retry.
    func fetch(_ done: @escaping (Fetch) -> Void) {
        guard let url else {
            done(.unreachable("the Nightscout URL is not valid"))
            return
        }
        var req = URLRequest(url: url)
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        session.dataTask(with: req) { data, response, error in
            if let e = error as? URLError { done(.unreachable(Self.describe(e.code))); return }
            if error != nil { done(.unreachable("request failed")); return }
            guard let http = response as? HTTPURLResponse else { done(.unreachable("no HTTP response")); return }
            guard http.statusCode == 200 else { done(.http(http.statusCode)); return }
            guard let data, data.count <= 64 * 1024 else { done(.badBody("reply missing or too large")); return }
            done(Nightscout.parse(data))
        }.resume()
    }

    private static func describe(_ code: URLError.Code) -> String {
        switch code {
        case .timedOut:
            return "timed out"
        case .notConnectedToInternet, .networkConnectionLost, .dataNotAllowed, .internationalRoamingOff:
            return "no network"
        case .cannotFindHost, .dnsLookupFailed:
            return "the server name did not resolve"
        case .cannotConnectToHost:
            return "the server refused the connection"
        case .secureConnectionFailed, .serverCertificateUntrusted, .serverCertificateHasBadDate,
             .serverCertificateNotYetValid, .serverCertificateHasUnknownRoot, .clientCertificateRejected:
            return "secure connection failed"
        default:
            return "network error"
        }
    }
}
