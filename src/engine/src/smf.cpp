#include "hsynth/smf.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "hsynth/fileio.h"
#include "hsynth/midi.h"

namespace hsynth {

namespace {

struct RawEvent {
    uint64_t tick;
    uint32_t order;      // file order, keeps simultaneous events stable
    uint8_t status;      // channel message status, or 0xFF meta (tempo)
    uint8_t d1, d2;
    uint32_t tempo;      // meta tempo: microseconds per quarter note
};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct Capture : EventSink {
    std::vector<Event>* out;
    void onEvent(const Event& e) override { out->push_back(e); }
};

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

}  // namespace

bool parseSmf(const uint8_t* data, size_t size, Song& out, std::string& error) {
    out = Song{};
    if (size < 14 || std::string((const char*)data, 4) != "MThd") { error = "not a MIDI file"; return false; }
    const uint32_t hlen = be32(data + 4);
    if (hlen < 6 || 8 + (size_t)hlen > size) { error = "bad MIDI header"; return false; }
    const int format = be16(data + 8);
    const int ntracks = be16(data + 10);
    const uint16_t division = be16(data + 12);
    if (format > 2) { error = "unsupported MIDI format " + std::to_string(format); return false; }
    // ticks -> seconds: PPQ (tempo map) or SMPTE (fixed)
    const bool smpte = division & 0x8000;
    const int ppq = smpte ? 0 : std::max<int>(1, division);
    double smpteTickSec = 0.0;
    if (smpte) {
        const int fps = -(int8_t)(division >> 8);
        const int tpf = division & 0xFF;
        smpteTickSec = 1.0 / (double)(std::max(1, fps * std::max(1, tpf)));
    }

    std::vector<RawEvent> raw;
    uint32_t order = 0;
    size_t pos = 8 + hlen;
    for (int t = 0; t < ntracks && pos + 8 <= size; ++t) {
        if (std::string((const char*)data + pos, 4) != "MTrk") {  // skip unknown chunks
            pos += 8 + (size_t)be32(data + pos + 4);
            --t;
            continue;
        }
        const size_t len = be32(data + pos + 4);
        size_t p = pos + 8;
        const size_t end = std::min(size, p + len);
        pos = p + len;
        uint64_t tick = 0;
        uint8_t running = 0;
        auto varlen = [&](uint32_t& v) {
            v = 0;
            for (int i = 0; i < 4 && p < end; ++i) {
                const uint8_t b = data[p++];
                v = (v << 7) | (b & 0x7F);
                if (!(b & 0x80)) return true;
            }
            return false;
        };
        while (p < end) {
            uint32_t delta;
            if (!varlen(delta)) break;
            tick += delta;
            if (p >= end) break;
            uint8_t st = data[p];
            if (st == 0xFF) {  // meta
                if (p + 2 > end) break;
                const uint8_t type = data[p + 1];
                p += 2;
                uint32_t mlen;
                if (!varlen(mlen) || p + mlen > end) break;
                if (type == 0x51 && mlen == 3)
                    raw.push_back({tick, order++, 0xFF, 0, 0, (uint32_t)data[p] << 16 | (uint32_t)data[p + 1] << 8 | data[p + 2]});
                if (type == 0x03 && t == 0 && out.name.empty() && mlen > 0) out.name.assign((const char*)data + p, mlen);
                p += mlen;
                if (type == 0x2F) break;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {  // sysex: skipped
                ++p;
                uint32_t slen;
                if (!varlen(slen) || p + slen > end) break;
                p += slen;
                continue;
            }
            if (st & 0x80) {
                running = st;
                ++p;
            } else if (!running) {
                error = "running status without a status byte";
                return false;
            } else {
                st = running;
            }
            const int nd = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 1 : 2;
            if (p + (size_t)nd > end) break;
            const uint8_t d1 = data[p] & 0x7F, d2 = nd > 1 ? (data[p + 1] & 0x7F) : 0;
            p += (size_t)nd;
            raw.push_back({tick, order++, st, d1, d2, 0});
        }
    }
    if (raw.empty()) { error = "no events"; return false; }
    std::stable_sort(raw.begin(), raw.end(), [](const RawEvent& a, const RawEvent& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });

    // tempo map -> seconds; channel messages -> engine events
    MidiTranslator tr;
    tr.setMultiTimbral(true);
    std::vector<Event> evs;
    Capture cap;
    cap.out = &evs;
    double usPerQuarter = 500000.0;
    bool tempoSeen = false;
    uint64_t lastTick = 0;
    double sec = 0.0;
    int bank[16], bankLsb[16];
    for (auto& b : bank) b = -1;
    for (auto& b : bankLsb) b = -1;
    for (const RawEvent& r : raw) {
        sec += smpte ? (double)(r.tick - lastTick) * smpteTickSec
                     : (double)(r.tick - lastTick) * usPerQuarter / 1e6 / (double)ppq;
        lastTick = r.tick;
        if (r.status == 0xFF) {
            usPerQuarter = std::max<uint32_t>(1, r.tempo);
            if (!tempoSeen) out.initialTempo = (float)(60e6 / usPerQuarter);
            tempoSeen = true;
            out.tempos.push_back({sec, (float)(60e6 / usPerQuarter)});
            continue;
        }
        const int ch = r.status & 15;
        const int type = r.status & 0xF0;
        if (type == 0xC0) {
            const int key = patchKey(bank[ch], bankLsb[ch], r.d1);
            if (out.initialProgram[ch] < 0) {
                out.initialProgram[ch] = key;
                out.bankMsb[ch] = bank[ch];
            }
            out.programChanges.push_back({sec, ch, key});
            // in the stream too: the engine switches to a staged program at this point
            Event pc = Event::control(EventType::ProgramChange, (float)key, r.d1);
            pc.channel = (uint8_t)ch;
            out.events.push_back({sec, pc});
            continue;
        }
        if (type == 0xB0 && r.d1 == 0) bank[ch] = r.d2;
        if (type == 0xB0 && r.d1 == 32) bankLsb[ch] = r.d2;
        if (type == 0x90 && r.d2 > 0) {
            out.channelUsed[ch] = true;
            ++out.noteCount;
        }
        const uint8_t msg[3] = {r.status, r.d1, r.d2};
        evs.clear();
        tr.processMidi1(msg, (type == 0xD0) ? 2 : 3, cap);
        for (const Event& e : evs) out.events.push_back({sec, e});
    }
    out.length = sec;
    return true;
}

bool loadSmf(const std::string& path, Song& out, std::string& error) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) { error = "cannot open " + path; return false; }
    std::fseek(f.get(), 0, SEEK_END);
    const long len = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    if (len <= 0 || len > (64l << 20)) { error = "bad MIDI file size"; return false; }
    std::vector<uint8_t> buf((size_t)len);
    if (std::fread(buf.data(), 1, buf.size(), f.get()) != buf.size()) { error = "read error " + path; return false; }
    if (!parseSmf(buf.data(), buf.size(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    out.path = path;
    const std::string file = baseName(path);
    if (out.name.empty() || out.name.size() > 60) out.name = file;
    out.name = file;  // the file name is what the user knows the song by
    return true;
}

}  // namespace hsynth
