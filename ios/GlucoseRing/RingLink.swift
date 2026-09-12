import CoreBluetooth
import Foundation
import UIKit

/// BLE central for the ring's Nordic UART Service.
///
/// With `UIBackgroundModes = bluetooth-central` and a restore identifier, iOS
/// keeps the connection while the app is suspended, wakes the app for the
/// ring's #REQ notification, and relaunches it after the system has killed
/// it. Without both, the design fails silently. All delegate callbacks arrive
/// on the main queue.
final class RingLink: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {

    enum State: Equatable {
        case bluetoothOff, unauthorized, noRing, scanning, connecting, connected, ready

        var text: String {
            switch self {
            case .bluetoothOff: return "Bluetooth is off, so the ring cannot be reached."
            case .unauthorized: return "Bluetooth permission was refused. Allow it in Settings > Privacy & Security > Bluetooth."
            case .noRing:       return "No ring chosen yet. Tap Find ring."
            case .scanning:     return "Looking for the ring..."
            case .connecting:   return "Ring not connected. iOS reconnects by itself when it is in range."
            case .connected:    return "Connected, waiting for the ring to answer. Accept the pairing request if asked."
            case .ready:        return "Ring connected."
            }
        }
    }

    // PROTOCOL.md, cross-checked against the firmware core's BLEUart.cpp:45-60.
    static let service = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let rxUUID  = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")   // we write
    static let txUUID  = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")   // the ring notifies
    static let ringName = "GlucoseRing"
    private static let restoreID = "com.glucosering.relay.central"
    private static let savedKey = "ringPeripheralID"

    var onState: ((State) -> Void)?
    var onLine: ((String) -> Void)?
    private(set) var state: State = .noRing {
        didSet { if state != oldValue { onState?(state) } }
    }

    private var central: CBCentralManager!
    private var ring: CBPeripheral?
    private var rx: CBCharacteristic?
    private var assembler = RingProtocol.LineAssembler()
    private var queue: [Data] = []
    private var writing = false
    private var connectedAt: Date?
    private var quickDrops = 0
    private var pacing = Backoff(capMs: 8_000)

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil, options: [
            CBCentralManagerOptionRestoreIdentifierKey: Self.restoreID,
            CBCentralManagerOptionShowPowerAlertKey: true,
        ])
    }

    // MARK: - Choosing the ring

    func findRing() {
        guard central.state == .poweredOn else { return }
        state = .scanning
        central.scanForPeripherals(withServices: [Self.service], options: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in
            guard let self, self.state == .scanning else { return }
            self.central.stopScan()
            self.state = self.ring == nil ? .noRing : .connecting
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // The service filter matches any Nordic UART device; require the name too.
        // The ring sends its name in the scan response (glucose-ring.ino setup()).
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name
        guard name == Self.ringName else { return }
        central.stopScan()
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedKey)
        connect(peripheral)
    }

    // MARK: - State restoration

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        // iOS relaunched us and kept the peripheral: reattach to it, never rescan.
        if let p = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            ring = p
            p.delegate = self
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:    reattach()
        case .unauthorized: state = .unauthorized
        default:            state = .bluetoothOff
        }
    }

    private func reattach() {
        if let p = ring {
            if p.state == .connected {
                p.delegate = self
                state = .connected
                discover(p)
            } else {
                connect(p)
            }
            return
        }
        if let saved = UserDefaults.standard.string(forKey: Self.savedKey), let id = UUID(uuidString: saved),
           let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(p)
            return
        }
        state = .noRing
    }

    // MARK: - Connection

    private func connect(_ p: CBPeripheral) {
        ring = p
        p.delegate = self
        state = .connecting
        // A pending connect never times out: iOS completes it whenever the ring
        // is in range, even while this app is suspended.
        central.connect(p, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedAt = Date()
        state = .connected
        discover(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        reconnect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        rx = nil
        queue.removeAll()
        writing = false
        assembler = RingProtocol.LineAssembler()
        reconnect(peripheral)
    }

    /// Reconnect at once. Only a connect-then-drop loop is paced, and never by
    /// more than 8 s, inside a background task, so the delayed connect is still
    /// issued within the time a background app gets. A longer timer could strand
    /// the app with no pending connect, and iOS would then never wake it.
    private func reconnect(_ p: CBPeripheral) {
        let lasted = connectedAt.map { Date().timeIntervalSince($0) } ?? 0
        connectedAt = nil
        quickDrops = lasted < 10 ? quickDrops + 1 : 0
        guard quickDrops >= 3 else {
            pacing.reset()
            connect(p)
            return
        }
        state = .connecting
        var task = UIBackgroundTaskIdentifier.invalid
        task = UIApplication.shared.beginBackgroundTask(withName: "ring-reconnect") {
            UIApplication.shared.endBackgroundTask(task)
            task = .invalid
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(pacing.next())) { [weak self] in
            self?.connect(p)
            if task != .invalid { UIApplication.shared.endBackgroundTask(task) }
        }
    }

    // MARK: - Discovery and subscription

    private func discover(_ p: CBPeripheral) {
        p.discoverServices([Self.service])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = peripheral.services?.first(where: { $0.uuid == Self.service }) else { return }
        peripheral.discoverCharacteristics([Self.rxUUID, Self.txUUID], for: s)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        rx = service.characteristics?.first { $0.uuid == Self.rxUUID }
        if let tx = service.characteristics?.first(where: { $0.uuid == Self.txUUID }) {
            peripheral.setNotifyValue(true, for: tx)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard error == nil, characteristic.isNotifying else { return }
        // Subscribed. The ring reads nothing until the link is encrypted and
        // answers only then; ask, in case its #BOOT already went by.
        write(RingProtocol.version)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.txUUID, let value = characteristic.value else { return }
        for line in assembler.feed([UInt8](value)) {
            // Only a reply proves the ring reads what we write.
            if line.hasPrefix("#BOOT") || line.hasPrefix("#VER") || line.hasPrefix("#REQ") {
                state = .ready
                quickDrops = 0
                pacing.reset()
            }
            onLine?(line)
        }
    }

    // MARK: - Writes

    /// Queue one protocol line, chunked to MTU - 3 (the .withoutResponse
    /// length). The .withResponse length reports up to 512 because iOS would
    /// fall back to a long write, which the ring's NUS characteristic is not
    /// sized for.
    func write(_ line: String) {
        guard let p = ring, rx != nil else { return }
        let max = p.maximumWriteValueLength(for: .withoutResponse)
        queue.append(contentsOf: RingProtocol.chunks(line, maxLength: max).map { Data($0) })
        pump()
    }

    /// One write in flight at a time, each acknowledged by the ring's stack.
    private func pump() {
        guard !writing, let p = ring, let c = rx, !queue.isEmpty else { return }
        writing = true
        p.writeValue(queue.removeFirst(), for: c, type: .withResponse)
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        writing = false
        if error != nil {
            // Part of a line never arrived. Drop the link; the next #REQ retries.
            queue.removeAll()
            central.cancelPeripheralConnection(peripheral)
            return
        }
        pump()
    }
}
