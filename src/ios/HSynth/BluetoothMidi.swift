import CoreAudioKit
import SwiftUI

/// Bluetooth LE MIDI with the system's own screens (CoreAudioKit):
/// - central: find and connect BLE MIDI keyboards / controllers (and a Mac offering BLE MIDI);
/// - peripheral: advertise this iPhone as a BLE MIDI device, so a Mac / PC / iPad can connect.
/// Connected devices become ordinary CoreMIDI ports; MidiIn picks them up by itself
/// (it rescans when the MIDI setup changes), notes and SysEx remote control included.
struct BluetoothMidiSheet: UIViewControllerRepresentable {
    enum Mode { case central, peripheral }
    let mode: Mode
    let onDone: () -> Void

    func makeUIViewController(context: Context) -> UINavigationController {
        let vc: UIViewController = mode == .central ? CABTMIDICentralViewController() : CABTMIDILocalPeripheralViewController()
        vc.navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: context.coordinator,
                                                               action: #selector(Coordinator.done))
        return UINavigationController(rootViewController: vc)
    }

    func updateUIViewController(_ uiViewController: UINavigationController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(onDone: onDone) }

    final class Coordinator: NSObject {
        let onDone: () -> Void
        init(onDone: @escaping () -> Void) { self.onDone = onDone }
        @objc func done() { onDone() }
    }
}
