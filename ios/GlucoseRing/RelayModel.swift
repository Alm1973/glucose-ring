import Foundation
import UIKit
import UserNotifications

/// The relay: the ring asks (#REQ) -> !TIME at once -> one Nightscout GET ->
/// the reading written back -> !GET STATUS to confirm. No threshold logic:
/// alarms are the ring's job. Main queue only (the central uses queue: nil).
final class RelayModel: ObservableObject {
    static let shared = RelayModel()

    @Published private(set) var status = Relay.Status(ok: false, title: "Starting", detail: "")
    @Published private(set) var linkText = ""

    private let config = Config.load()
    private let ring = RingLink()
    private let nightscout: NightscoutClient
    private var relay = RelayState()
    private var fetching = false

    private init() {
        nightscout = NightscoutClient(config: config)
        ring.onState = { [weak self] s in self?.onLink(s) }
        ring.onLine = { [weak self] line in self?.onRingLine(line) }
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        refresh()
    }

    func findRing() { ring.findRing() }

    func refresh() {
        status = relay.status(nowMs: Self.nowMs(), configProblem: config.problem)
        linkText = ring.state.text
    }

    private func onLink(_ s: RingLink.State) {
        if s == .ready { relay.linkConfirmed() }
        refresh()
    }

    private func onRingLine(_ line: String) {
        guard let p = RingProtocol.parseRingLine(line) else { return }
        switch p.kind {
        case "REQ":    answerPoke()
        case "STATUS": relay.onRingStatus(p)
        default:       break
        }
        refresh()
    }

    /// The ring asked. iOS gives a background app a few seconds for this.
    private func answerPoke() {
        let now = Self.nowMs()
        relay.onPoke(nowMs: now)
        // First, before any network request: the ring's trusted clock and its
        // proof of life. A slow or failed fetch then shows on the ring as STALE,
        // never as MISS (PROTOCOL.md section 9).
        ring.write(RingProtocol.time(now))
        guard config.problem == nil, !fetching else { return }
        fetching = true

        var task = UIBackgroundTaskIdentifier.invalid
        task = UIApplication.shared.beginBackgroundTask(withName: "nightscout-fetch") {
            UIApplication.shared.endBackgroundTask(task)
            task = .invalid
        }
        nightscout.fetch { [weak self] result in
            DispatchQueue.main.async {
                defer {
                    if task != .invalid { UIApplication.shared.endBackgroundTask(task); task = .invalid }
                }
                guard let self else { return }
                self.fetching = false
                let wasRejected = self.relay.tokenRejectedCode != nil
                if let line = self.relay.onFetch(result, nowMs: Self.nowMs()) {
                    self.ring.write(line)
                    self.ring.write(RingProtocol.getStatus)   // a write is not delivery: ask the ring
                }
                if !wasRejected, let code = self.relay.tokenRejectedCode { Self.notifyTokenRejected(code) }
                self.refresh()
            }
        }
    }

    /// A revoked token is permanent. Say so even when the app is not open.
    private static func notifyTokenRejected(_ code: Int) {
        let content = UNMutableNotificationContent()
        content.title = "Glucose Ring: token rejected"
        content.body = "Nightscout refused the token (HTTP \(code)). Readings will not reach the ring until it is replaced."
        UNUserNotificationCenter.current().add(
            UNNotificationRequest(identifier: "token-rejected", content: content, trigger: nil))
    }

    static func nowMs() -> Int64 { Int64((Date().timeIntervalSince1970 * 1000).rounded()) }
}
