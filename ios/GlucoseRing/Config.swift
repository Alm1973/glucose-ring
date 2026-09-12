import Foundation

/// The Nightscout URL and read-only token, from Secrets.plist: gitignored,
/// bundled at build time (PHONE_APP.md section 4). Never logged.
struct Config {
    let url: String
    let token: String

    static func load(bundle: Bundle = .main) -> Config {
        guard let file = bundle.url(forResource: "Secrets", withExtension: "plist"),
              let data = try? Data(contentsOf: file),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
        else { return Config(url: "", token: "") }
        return Config(url: plist["NightscoutURL"] as? String ?? "",
                      token: plist["NightscoutToken"] as? String ?? "")
    }

    var problem: String? { Relay.configProblem(url: url, token: token) }
}
