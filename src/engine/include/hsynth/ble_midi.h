// MIDI over Bluetooth Low Energy (BLE-MIDI 1.0) packet format.
//
// Hosts whose OS does not handle BLE MIDI itself (the Android GATT peripheral)
// use this to turn BLE packets into plain MIDI bytes and back:
//
//   packet  = header, then messages; header = 10hhhhhh (timestamp bits 12..7)
//   message = timestamp byte 1lllllll (bits 6..0), then a MIDI message; running
//             status may follow without status / timestamp bytes
//   SysEx   = ts F0 data...  (continues in the next packets right after the header,
//             realtime messages may interleave as ts + F8..FF) ... ts F7
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hsynth {
namespace ble {

// Standard UUIDs of the BLE MIDI service / its single characteristic.
constexpr const char* kServiceUuid = "03b80e5a-ede8-4b33-a751-6ce34ec4c700";
constexpr const char* kCharacteristicUuid = "7772e5db-3868-4112-a1a9-f2669d106bf3";

// Decodes the packets of one connection (stateful: SysEx spans packets).
class Decoder {
public:
    // Called once per complete MIDI message (channel, system, realtime or a whole F0..F7 SysEx).
    using Sink = std::function<void(const uint8_t* data, size_t len)>;
    void packet(const uint8_t* p, size_t n, const Sink& out);
    void reset();

private:
    std::vector<uint8_t> sysex_;
    bool inSysex_ = false;
    uint8_t running_ = 0;
};

// Encodes one complete MIDI message (or SysEx) into packets of at most maxPacket
// bytes (ATT MTU - 3; at least 5). timeMs: the sender's clock in milliseconds.
std::vector<std::vector<uint8_t>> encode(const uint8_t* msg, size_t len, size_t maxPacket, uint32_t timeMs);

}  // namespace ble
}  // namespace hsynth
