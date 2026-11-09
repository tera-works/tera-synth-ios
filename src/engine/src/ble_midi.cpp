#include "hsynth/ble_midi.h"

#include <algorithm>

namespace hsynth {
namespace ble {

namespace {
// Data bytes after a status byte (SysEx excluded).
int dataBytes(uint8_t status) {
    if (status < 0xF0) {
        const uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;
    }
}
}  // namespace

void Decoder::reset() {
    sysex_.clear();
    inSysex_ = false;
    running_ = 0;
}

void Decoder::packet(const uint8_t* p, size_t n, const Sink& out) {
    if (n < 2 || !(p[0] & 0x80)) return;  // not a BLE MIDI packet
    size_t i = 1;
    while (i < n) {
        const uint8_t b = p[i];
        if (inSysex_) {
            if (b < 0x80) {  // SysEx data (also right after the header of a continuation packet)
                sysex_.push_back(b);
                if (sysex_.size() > (1u << 20)) reset();
                ++i;
                continue;
            }
            // a timestamp: then F7 (end), a realtime byte, or a new status (SysEx aborted)
            if (++i >= n) break;
            const uint8_t c = p[i];
            if (c == 0xF7) {
                sysex_.push_back(0xF7);
                out(sysex_.data(), sysex_.size());
                sysex_.clear();
                inSysex_ = false;
                ++i;
            } else if (c >= 0xF8) {
                out(&c, 1);
                ++i;
            } else {
                sysex_.clear();
                inSysex_ = false;  // c is handled below as a status byte after a timestamp
                --i;               // back to the timestamp
            }
            continue;
        }
        uint8_t status;
        if (b & 0x80) {  // timestamp, then a status byte or running-status data
            if (++i >= n) break;
            const uint8_t c = p[i];
            if (c & 0x80) {
                if (c == 0xF0) {
                    inSysex_ = true;
                    sysex_.assign(1, 0xF0);
                    ++i;
                    continue;
                }
                if (c >= 0xF8) {
                    out(&c, 1);
                    ++i;
                    continue;
                }
                status = c;
                if (c < 0xF0) running_ = c;
                else running_ = 0;  // system common cancels running status
                ++i;
            } else {
                if (!running_) { ++i; continue; }
                status = running_;
            }
        } else {  // running-status data without a timestamp byte
            if (!running_) { ++i; continue; }
            status = running_;
        }
        const int nd = dataBytes(status);
        uint8_t m[3] = {status, 0, 0};
        int k = 0;
        while (k < nd && i < n && p[i] < 0x80) m[1 + k++] = p[i++];
        if (k == nd) out(m, (size_t)(1 + nd));
    }
}

std::vector<std::vector<uint8_t>> encode(const uint8_t* msg, size_t len, size_t maxPacket, uint32_t timeMs) {
    std::vector<std::vector<uint8_t>> out;
    if (len == 0) return out;
    maxPacket = std::max<size_t>(5, std::min<size_t>(maxPacket, 512));
    const uint8_t header = (uint8_t)(0x80 | ((timeMs >> 7) & 0x3F));
    const uint8_t ts = (uint8_t)(0x80 | (timeMs & 0x7F));
    if (msg[0] != 0xF0) {  // short message: header, timestamp, message
        out.push_back({header, ts});
        out.back().insert(out.back().end(), msg, msg + len);
        return out;
    }
    // SysEx: header ts F0 data... | header data... | ... ts F7
    const size_t end = (msg[len - 1] == 0xF7) ? len - 1 : len;  // data incl. F0, without F7
    std::vector<uint8_t> pk{header, ts};
    for (size_t i = 0; i < end; ++i) {
        if (pk.size() >= maxPacket) {
            out.push_back(std::move(pk));
            pk = {header};
        }
        pk.push_back(msg[i]);
    }
    if (pk.size() + 2 > maxPacket) {
        out.push_back(std::move(pk));
        pk = {header};
    }
    pk.push_back(ts);
    pk.push_back(0xF7);
    out.push_back(std::move(pk));
    return out;
}

}  // namespace ble
}  // namespace hsynth
