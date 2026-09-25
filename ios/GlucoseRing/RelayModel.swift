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
    @Published private(set) var test: Relay.Status?
    @Published private(set) var testing = false
    @Published private(set) var testedAt: Date?
    @Published private(set) var battery: String?
    @Published private(set) var batteryAt: Date?
    /// The last line the ring sent, whatever it was. A command that silently
    /// does nothing is indistinguishable from a dead link without this.
    @Published private(set) var lastReply: String?
    @Published private(set) var lastReplyAt: Date?

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

    /// Ask the ring what it measures on its own divider. The only way to read
    /// it once a cell is attached: the serial log needs USB, and USB is the
    /// charger, so the firmware (correctly) skips sampling while it is in.
    func readBattery() { ring.write(RingProtocol.getBatt) }

    /// Fire one alert pattern on demand. Proves the alert path end to end,
    /// and -- on battery -- that the power path survives the ~85 mA a buzz
    /// draws, which is when a marginal joint or a protection IC lets go.
    func buzzTest(_ pattern: String) { ring.write(RingProtocol.buzz(pattern)) }

    /// One real, read-only Nightscout fetch on demand. It proves the token,
    /// network and parsing on this phone before a ring exists. Nothing is
    /// written to the ring, and it does not count as a ring request.
    func testNightscout() {
        guard !testing else { return }
        if let problem = config.problem {
            test = Relay.Status(ok: false, title: "Not configured", detail: problem)
            testedAt = Date()
            return
        }
        testing = true
        nightscout.fetch { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.testing = false
                self.testedAt = Date()
                self.relay.onTestFetch(result)   // a revoked token is recorded; nothing else changes
                self.test = Relay.testStatus(result, nowMs: Self.nowMs())
                self.refresh()
            }
        }
    }

    func refresh() {
        status = relay.status(nowMs: Self.nowMs(), configProblem: config.problem)
        linkText = ring.state.text
    }

    private func onLink(_ s: RingLink.State) {
        if s == .ready { relay.linkConfirmed() }
        refresh()
    }

    private func onRingLine(_ line: String) {
        lastReply = line
        lastReplyAt = Date()
        guard let p = RingProtocol.parseRingLine(line) else { return }
        switch p.kind {
        case "REQ":    answerPoke()
        case "STATUS": relay.onRingStatus(p)
        case "BATT":   battery = Relay.batteryText(p.fields); batteryAt = Date()
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
        content.title = "GRing: token rejected"
        content.body = "Nightscout refused the token (HTTP \(code)). Readings will not reach the ring until it is replaced."
        UNUserNotificationCenter.current().add(
            UNNotificationRequest(identifier: "token-rejected", content: content, trigger: nil))
    }

    static func nowMs() -> Int64 { Int64((Date().timeIntervalSince1970 * 1000).rounded()) }
}
