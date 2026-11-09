#include "hsynth/midi_learn.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/json.h"
#include "hsynth/midi.h"
#include "hsynth/params.h"

namespace hsynth {

namespace {

// CC 0..127 -> parameter value along the parameter's curve.
float ccToValue(const MidiMapping& m, int value) {
    const ParamInfo& info = paramTable()[(size_t)m.param];
    const float t = (float)value / 127.0f;
    switch (info.kind) {
        case ParamKind::Bool: return value >= 64 ? 1.0f : 0.0f;
        case ParamKind::Enum:
        case ParamKind::Int: return std::round(m.min + (m.max - m.min) * t);
        default: break;
    }
    if (info.curve == ParamCurve::Exp && m.min > 0.0f && m.max > 0.0f) return m.min * std::pow(m.max / m.min, t);
    return m.min + (m.max - m.min) * t;
}

}  // namespace

void MidiLearn::arm(int param, int part) {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = param >= 0 && param < P::kNumParams ? param : -1;
    armedPart_ = part;
}

void MidiLearn::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = -1;
}

int MidiLearn::armedParam() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_;
}

int MidiLearn::lastLearned() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastLearned_;
}

void MidiLearn::onUmp(const uint32_t* words, size_t count, Engine& engine) {
    for (size_t i = 0; i < count;) {
        const uint32_t w0 = words[i];
        const int n = MidiTranslator::umpWordCount(w0);
        if (i + (size_t)n > count) break;
        const int mt = (int)(w0 >> 28);
        const uint8_t status = (uint8_t)((w0 >> 16) & 0xFF);
        if (mt == 0x2 && (status & 0xF0) == 0xB0) {  // MIDI 1.0 CC in a packet
            const uint8_t m[3] = {status, (uint8_t)((w0 >> 8) & 0x7F), (uint8_t)(w0 & 0x7F)};
            onMidi(m, 3, engine);
        } else if (mt == 0x4 && (status & 0xF0) == 0xB0) {  // MIDI 2.0 CC: 32-bit value
            const uint8_t m[3] = {status, (uint8_t)((w0 >> 8) & 0x7F), (uint8_t)(words[i + 1] >> 25)};
            onMidi(m, 3, engine);
        }
        i += (size_t)n;
    }
}

bool MidiLearn::onMidi(const uint8_t* data, size_t len, Engine& engine) {
    if (len < 3 || (data[0] & 0xF0) != 0xB0) return false;
    const int ch = data[0] & 0x0F, cc = data[1] & 0x7F, value = data[2] & 0x7F;
    if (cc >= 120) return false;  // channel mode messages are never mapped
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed_ >= 0) {
        // replace any mapping of this CC (on this channel) and of this parameter
        std::vector<MidiMapping> kept;
        for (const auto& m : maps_)
            if (m.param != armed_ && !(m.cc == cc && (m.channel < 0 || m.channel == ch))) kept.push_back(m);
        maps_ = std::move(kept);
        const ParamInfo& info = paramTable()[(size_t)armed_];
        MidiMapping m;
        m.channel = -1;
        m.cc = cc;
        m.param = armed_;
        m.part = armedPart_;
        m.min = info.min;
        m.max = info.max;
        maps_.push_back(m);
        lastLearned_ = armed_;
        armed_ = -1;
    }
    if (Engine::standardCcIndex(cc) >= 0) syncLocked(engine, cc);
    bool hit = false;
    for (const auto& m : maps_) {
        if (m.cc != cc || (m.channel >= 0 && m.channel != ch)) continue;
        const float v = ccToValue(m, value);
        if (isPartParam(m.param)) engine.setPartParam(m.part >= 0 ? m.part : (engine.multiTimbral() ? engine.selectedPart() : 0), m.param, v);
        else engine.setParam(m.param, v);
        hit = true;
    }
    return hit;
}

void MidiLearn::syncLocked(Engine& engine, int onlyCc) const {
    // a standard controller learned for a part parameter replaces its standard response there
    const int sel = engine.multiTimbral() ? engine.selectedPart() : 0;
    for (int cc : Engine::kStandardCcs) {
        if (onlyCc >= 0 && cc != onlyCc) continue;
        uint32_t parts = 0;
        for (const auto& m : maps_)
            if (m.cc == cc && isPartParam(m.param)) parts |= 1u << (m.part >= 0 ? m.part : sel);
        for (int k = 0; k < kNumParts; ++k) engine.setStandardCcOverride(k, cc, (parts >> k) & 1u);
    }
}

void MidiLearn::syncStandardOverrides(Engine& engine) const {
    std::lock_guard<std::mutex> lock(mutex_);
    syncLocked(engine, -1);
}

std::vector<MidiMapping> MidiLearn::mappings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maps_;
}

void MidiLearn::setMappings(std::vector<MidiMapping> m) {
    std::lock_guard<std::mutex> lock(mutex_);
    maps_ = std::move(m);
}

void MidiLearn::removeParam(int param) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MidiMapping> kept;
    for (const auto& m : maps_)
        if (m.param != param) kept.push_back(m);
    maps_ = std::move(kept);
}

std::string MidiLearn::describe(int param) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string s;
    for (const auto& m : maps_) {
        if (m.param != param) continue;
        if (!s.empty()) s += ", ";
        s += "CC " + std::to_string(m.cc) + (m.channel >= 0 ? " ch " + std::to_string(m.channel + 1) : "");
        if (m.part >= 0) s += " (part " + std::to_string(m.part + 1) + ")";
    }
    return s;
}

std::string MidiLearn::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json root = Json::object();
    root.set("format", "hsynth-midimap");
    root.set("version", 1);
    Json a = Json::array();
    for (const auto& m : maps_) {
        Json o = Json::object();
        o.set("param", paramTable()[(size_t)m.param].name);
        o.set("cc", m.cc);
        o.set("channel", m.channel < 0 ? 0 : m.channel + 1);  // 0 = any
        if (m.part >= 0) o.set("part", m.part + 1);
        o.set("min", m.min);
        o.set("max", m.max);
        a.push(o);
    }
    root.set("mappings", a);
    return root.dump();
}

bool MidiLearn::fromJson(const std::string& json, std::string& error) {
    Json root;
    if (!Json::parse(json, root, error)) return false;
    const Json* a = root.find("mappings");
    if (!a || !a->isArray()) { error = "no mappings"; return false; }
    std::vector<MidiMapping> maps;
    for (size_t i = 0; i < a->size(); ++i) {
        const Json& o = a->at(i);
        const Json* name = o.find("param");
        const int id = name && name->isString() ? findParam(name->string()) : -1;
        if (id < 0) continue;  // parameter no longer exists
        MidiMapping m;
        m.param = id;
        if (auto v = o.find("cc")) m.cc = std::max(0, std::min(119, (int)v->number()));
        if (auto v = o.find("channel")) m.channel = (int)v->number() - 1;
        if (auto v = o.find("part")) m.part = std::max(-1, std::min(kNumParts - 1, (int)v->number() - 1));
        const ParamInfo& info = paramTable()[(size_t)id];
        m.min = info.min;
        m.max = info.max;
        if (auto v = o.find("min")) m.min = (float)v->number(info.min);
        if (auto v = o.find("max")) m.max = (float)v->number(info.max);
        maps.push_back(m);
    }
    setMappings(std::move(maps));
    return true;
}

bool MidiLearn::load(const std::string& path, std::string& error) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return fromJson(ss.str(), error);
}

bool MidiLearn::save(const std::string& path, std::string& error) const {
    std::ofstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot write " + path; return false; }
    f << toJson();
    return (bool)f;
}

}  // namespace hsynth
