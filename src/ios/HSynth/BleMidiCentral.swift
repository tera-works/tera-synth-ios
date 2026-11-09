import CoreBluetooth
import CoreMIDI
import SwiftUI

/// Bluetooth LE MIDI devices through Core Bluetooth, like other music apps: lists the
/// devices that advertise the MIDI service, the ones already connected to iOS and the
/// ones used before (remembered). A connected peripheral is handed to CoreMIDI
/// (MIDIBluetoothDriverActivateAllConnections), where it shows up as a MIDI input.
/// Remembered devices are reconnected when the app starts (and whenever they come
/// back in range: a Core Bluetooth connect request does not time out).
final class BleMidiCentral: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    static let midiService = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")
    private static let knownKey = "bleMidiKnown"
    static var knownIDs: [UUID] { (UserDefaults.standard.stringArray(forKey: knownKey) ?? []).compactMap(UUID.init) }

    struct Device: Identifiable {
        let id: UUID
        var name: String
        var state: CBPeripheralState
        var known: Bool
    }

    @Published private(set) var devices: [Device] = []
    @Published private(set) var poweredOn = false
    @Published private(set) var scanning = false
    @Published var message = ""
    /// A device was handed to CoreMIDI: reopen the MIDI inputs.
    var onConnected: (() -> Void)?

    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var names: [UUID: String] = [:]

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    // MARK: - actions

    func scan() {
        guard poweredOn else { message = "Bluetooth がオフか、使用が許可されていません"; return }
        addKnown()
        central.scanForPeripherals(withServices: [Self.midiService], options: nil)
        scanning = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in self?.stopScan() }
    }

    func stopScan() {
        if central.isScanning { central.stopScan() }
        scanning = false
    }

    /// Tap on a device: connect, or disconnect (and forget) when connected.
    func toggle(_ id: UUID) {
        guard let p = peripherals[id] else { return }
        switch p.state {
        case .connected, .connecting:
            central.cancelPeripheralConnection(p)
            forget(id)
        default:
            message = ""
            central.connect(p, options: nil)
        }
        refresh()
    }

    // MARK: - Core Bluetooth

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        poweredOn = c.state == .poweredOn
        if c.state == .unauthorized { message = "設定アプリで HSynth の Bluetooth の使用を許可してください" }
        guard poweredOn else { refresh(); return }
        addKnown()
        for id in Self.knownIDs {  // reconnect the devices used before
            if let p = peripherals[id], p.state == .disconnected { central.connect(p, options: nil) }
        }
        refresh()
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
        add(p, name: advertisementData[CBAdvertisementDataLocalNameKey] as? String)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        remember(p.identifier)
        p.delegate = self
        p.discoverServices([Self.midiService])
        activate()
        refresh()
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        message = "接続できませんでした: " + (error?.localizedDescription ?? "")
        refresh()
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        // a remembered device that dropped out: wait for it to come back
        if Self.knownIDs.contains(p.identifier) { central.connect(p, options: nil) }
        refresh()
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        activate()  // some devices are published only once their MIDI service is known
    }

    // MARK: - helpers

    private func activate() {
        if #available(iOS 16.0, *) {
            let st = MIDIBluetoothDriverActivateAllConnections()
            if st != noErr { message = "CoreMIDI への接続に失敗しました（\(st)）" }
        }
        // CoreMIDI publishes the device a moment later
        for delay in [0.5, 1.5, 3.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.onConnected?() }
        }
    }

    private func addKnown() {
        for p in central.retrieveConnectedPeripherals(withServices: [Self.midiService]) { add(p, name: nil) }
        for p in central.retrievePeripherals(withIdentifiers: Self.knownIDs) { add(p, name: nil) }
    }

    private func add(_ p: CBPeripheral, name: String?) {
        peripherals[p.identifier] = p
        if let n = name ?? p.name, !n.isEmpty { names[p.identifier] = n }
        refresh()
    }

    private func remember(_ id: UUID) {
        var ids = UserDefaults.standard.stringArray(forKey: Self.knownKey) ?? []
        if !ids.contains(id.uuidString) { ids.append(id.uuidString) }
        UserDefaults.standard.set(ids, forKey: Self.knownKey)
    }

    private func forget(_ id: UUID) {
        let ids = (UserDefaults.standard.stringArray(forKey: Self.knownKey) ?? []).filter { $0 != id.uuidString }
        UserDefaults.standard.set(ids, forKey: Self.knownKey)
    }

    private func refresh() {
        let known = Set(Self.knownIDs)
        devices = peripherals.values.map {
            Device(id: $0.identifier, name: names[$0.identifier] ?? $0.name ?? "BLE MIDI", state: $0.state, known: known.contains($0.identifier))
        }.sorted { ($0.state == .connected ? 0 : 1, $0.name) < ($1.state == .connected ? 0 : 1, $1.name) }
    }
}

/// System screen sheet: BLE MIDI devices (scan, connect / disconnect).
struct BleMidiView: View {
    @ObservedObject var ble: BleMidiCentral
    let systemScreen: () -> Void
    let done: () -> Void

    var body: some View {
        NavigationView {
            List {
                Section {
                    ForEach(ble.devices) { d in
                        Button { ble.toggle(d.id) } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(d.name)
                                    if d.known { Text("以前に接続").font(.caption).foregroundColor(.secondary) }
                                }
                                Spacer()
                                Text(label(d.state)).foregroundColor(d.state == .connected ? .green : .secondary)
                            }
                        }
                    }
                    if ble.devices.isEmpty {
                        Text(ble.scanning ? "検索中…" : "「Scan」で近くの BLE MIDI 機器を探します").foregroundColor(.secondary)
                    }
                } footer: {
                    Text("タップで接続、接続中の機器をもう一度タップすると切断します。接続した機器は覚えておき、次回の起動時や電源を入れ直したときに自動で再接続します。" +
                         (ble.message.isEmpty ? "" : "\n\n" + ble.message))
                }
                Section {
                    Button("iOS 標準の接続画面を開く") { systemScreen() }
                }
            }
            .navigationTitle("Bluetooth MIDI")
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button(ble.scanning ? "Stop" : "Scan") { ble.scanning ? ble.stopScan() : ble.scan() }
                }
                ToolbarItem(placement: .navigationBarTrailing) { Button("Done") { ble.stopScan(); done() } }
            }
            .onAppear { ble.scan() }
        }
    }

    private func label(_ s: CBPeripheralState) -> String {
        switch s {
        case .connected: return "Connected"
        case .connecting: return "接続中…"
        case .disconnecting: return "切断中…"
        default: return "—"
        }
    }
}
