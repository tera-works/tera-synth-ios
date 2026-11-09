// Remote control over MIDI SysEx: framing, request handling, change notifications.
// Protocol: docs/SYSEX.md.
#include "hsynth/remote.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "hsynth/fileio.h"
#include "hsynth/fx/ir_load.h"
#include "hsynth/gm_bank.h"
#include "hsynth/host_utils.h"
#include "hsynth/midi_learn.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"
#include "hsynth/preset_index.h"
#include "hsynth/setup.h"
#include "hsynth/smf.h"

namespace hsynth {

// ====================================================================== framing

namespace sysex {

std::string asciiJson(const Json& j) {
    const std::string s = j.dump(0);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out.push_back((char)c);
            ++i;
            continue;
        }
        // UTF-8 -> code point (strings are the only place where these appear)
        unsigned cp = 0;
        int n = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
        else { ++i; continue; }  // stray byte
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        char buf[16];
        if (cp >= 0x10000) {
            cp -= 0x10000;
            std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
        } else {
            std::snprintf(buf, sizeof buf, "\\u%04x", cp);
        }
        out += buf;
    }
    return out;
}

std::vector<std::vector<uint8_t>> encode(int device, int type, int seq, const std::string& json) {
    std::vector<std::vector<uint8_t>> out;
    const size_t total = std::max<size_t>(1, (json.size() + kChunkBytes - 1) / kChunkBytes);
    for (size_t k = 0; k < total; ++k) {
        std::vector<uint8_t> m;
        const size_t a = k * kChunkBytes, b = std::min(json.size(), a + kChunkBytes);
        m.reserve(kHeaderBytes + (b - a) + 1);
        m.push_back(0xF0);
        m.push_back(kManufacturer);
        m.push_back(kId1);
        m.push_back(kId2);
        m.push_back(kVersion);
        m.push_back((uint8_t)(device & 0x7F));
        m.push_back((uint8_t)(type & 0x7F));
        m.push_back((uint8_t)(seq & 0x7F));
        m.push_back((uint8_t)(k & 0x7F));
        m.push_back((uint8_t)((k >> 7) & 0x7F));
        m.push_back((uint8_t)(total & 0x7F));
        m.push_back((uint8_t)((total >> 7) & 0x7F));
        for (size_t i = a; i < b; ++i) m.push_back((uint8_t)(json[i] & 0x7F));
        m.push_back(0xF7);
        out.push_back(std::move(m));
    }
    return out;
}

bool parseFrame(const uint8_t* d, size_t len, Frame& f) {
    if (len < kHeaderBytes + 1 || d[0] != 0xF0 || d[len - 1] != 0xF7) return false;
    if (d[1] != kManufacturer || d[2] != kId1 || d[3] != kId2) return false;
    if (d[4] != kVersion) return false;
    for (size_t i = 1; i + 1 < len; ++i)
        if (d[i] & 0x80) return false;
    f.device = d[5];
    f.type = d[6];
    f.seq = d[7];
    f.index = d[8] | (d[9] << 7);
    f.total = d[10] | (d[11] << 7);
    if (f.total < 1 || f.index >= f.total) return false;
    f.payload.assign(reinterpret_cast<const char*>(d + kHeaderBytes), len - kHeaderBytes - 1);
    return true;
}

bool Assembler::add(const Frame& f, std::string& json) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = partial_.begin(); it != partial_.end();)  // drop what never completed
        it = now - it->second.started > std::chrono::seconds(10) ? partial_.erase(it) : std::next(it);
    if (f.total == 1) {
        json = f.payload;
        return true;
    }
    const int key = f.type << 8 | f.seq;
    Partial& p = partial_[key];
    // a new message with this seq (chunk 0 again, or another length)
    if (p.total != f.total || (f.index == 0 && !p.chunks.empty() && !p.chunks[0].empty())) {
        p = Partial();
        p.total = f.total;
        p.chunks.resize((size_t)f.total);
        p.started = now;
    }
    if (p.chunks[(size_t)f.index].empty()) ++p.received;
    p.chunks[(size_t)f.index] = f.payload.empty() ? std::string(" ") : f.payload;
    if (p.received < p.total) return false;
    json.clear();
    for (auto& c : p.chunks) json += c;
    partial_.erase(key);
    return true;
}

}  // namespace sysex

// ====================================================================== helpers

namespace {

const char* kOps[] = {"hello", "param_info", "mix_info", "get", "set", "mix_get", "mix_set", "get_state", "set_state",
                      "mode", "select_part", "presets", "load_preset", "save_preset", "samples", "impulse", "files",
                      "note", "event", "panic", "transport", "looper", "phrase", "songs", "song_load", "song",
                      "status", "meters", "settings", "learn", "subscribe", "unsubscribe", "setup_get", "setup_set"};

const char* kKinds[] = {"float", "int", "enum", "bool"};

Json errorReply(const std::string& msg) {
    Json r = Json::object();
    r.set("ok", false);
    r.set("error", msg);
    return r;
}

Json okReply() {
    Json r = Json::object();
    r.set("ok", true);
    return r;
}

int intField(const Json& j, const char* key, int def) {
    const Json* v = j.find(key);
    return v && (v->isNumber() || v->isBool()) ? (int)std::lround(v->number()) : def;
}

double numField(const Json& j, const char* key, double def) {
    const Json* v = j.find(key);
    return v && (v->isNumber() || v->isBool()) ? v->number() : def;
}

std::string strField(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->string() : std::string();
}

bool boolField(const Json& j, const char* key, bool def) {
    const Json* v = j.find(key);
    return v ? v->boolean(def) : def;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

const char* layerKey(int l) { return l == 0 ? "a" : l == 1 ? "b" : "c"; }

Json stringArray(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push(s);
    return a;
}

// "a" / "b" / "c" or 0..2
int layerOf(const Json& req) {
    const Json* v = req.find("layer");
    if (!v) return 0;
    if (v->isString()) {
        const std::string s = lower(v->string());
        return s == "b" ? 1 : s == "c" ? 2 : 0;
    }
    return std::max(0, std::min(kNumLayers - 1, (int)v->number()));
}

}  // namespace

// ====================================================================== RemoteControl

RemoteControl::RemoteControl(Engine& engine, Config config, Hooks hooks)
    : engine_(engine), config_(std::move(config)), hooks_(std::move(hooks)) {
    snapshot();
    lastStatus_ = std::chrono::steady_clock::now();
    worker_ = std::thread([this] { run(); });
}

RemoteControl::~RemoteControl() {
    {
        std::lock_guard<std::mutex> lock(inMutex_);
        run_.store(false);
    }
    inCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (programs_) programs_->stop();
}

void RemoteControl::setOutput(Output out) {
    std::lock_guard<std::mutex> lock(outMutex_);
    output_ = std::move(out);
}

bool RemoteControl::poll(std::vector<uint8_t>& message) {
    std::lock_guard<std::mutex> lock(outMutex_);
    if (outQueue_.empty()) return false;
    message = std::move(outQueue_.front());
    outQueue_.pop_front();
    return true;
}

void RemoteControl::onSysEx(const uint8_t* data, size_t len) {
    sysex::Frame f;
    if (!sysex::parseFrame(data, len, f)) return;
    if (f.type != sysex::Request) return;  // replies / notifications (our own echo on a loopback port)
    if (f.device != sysex::kAllDevices && f.device != config_.deviceId) return;
    std::lock_guard<std::mutex> lock(inMutex_);
    std::string json;
    if (!assembler_.add(f, json)) return;
    requests_.push_back({f.device, f.seq, std::move(json)});
    inCv_.notify_one();
}

void RemoteControl::emit(int type, int seq, const Json& body) {
    const auto msgs = sysex::encode(config_.deviceId, type, seq, sysex::asciiJson(body));
    std::lock_guard<std::mutex> lock(outMutex_);
    for (const auto& m : msgs) {
        if (output_) output_(m);
        else if (outQueue_.size() < 20000) outQueue_.push_back(m);
    }
}

void RemoteControl::run() {
    while (run_.load()) {
        Pending req;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(inMutex_);
            inCv_.wait_for(lock, std::chrono::milliseconds(std::max(10, intervalMs_)),
                           [this] { return !requests_.empty() || !run_.load(); });
            if (!run_.load()) break;
            if (!requests_.empty()) {
                req = std::move(requests_.front());
                requests_.pop_front();
                have = true;
            }
        }
        // changes made by others (UI, MIDI CCs) before this request
        for (const Json& n : collectChanges()) emit(sysex::Notify, (notifySeq_++) & 0x7F, n);
        if (!have) continue;
        Json request, reply;
        std::string err;
        if (!Json::parse(req.json, request, err) || !request.isObject()) {
            reply = errorReply("bad JSON: " + err);
        } else {
            reply = handle(request);
        }
        if (const Json* op = request.find("op"); op && op->isString()) {
            Json r = Json::object();  // "op" first, for readability
            r.set("op", op->string());
            for (const auto& m : reply.members()) r.set(m.first, m.second);
            reply = std::move(r);
        }
        emit(sysex::Reply, req.seq, reply);
        snapshot();  // what this request changed is not reported back as a change
    }
}

// ---------------------------------------------------------------- snapshots / notifications

void RemoteControl::snapshot() {
    snap_.resize((size_t)kNumParts * P::kNumParams);
    for (int p = 0; p < kNumParts; ++p) engine_.getPartParams(p, snap_.data() + (size_t)p * P::kNumParams);
    snapMix_.resize((size_t)kNumParts * Mix::Count);
    for (int p = 0; p < kNumParts; ++p)
        for (int i = 0; i < Mix::Count; ++i) snapMix_[(size_t)p * Mix::Count + i] = engine_.getPartMix(p, i);
    snapMulti_ = engine_.multiTimbral();
    snapSelected_ = engine_.selectedPart();
    snapSamples_.resize((size_t)kNumParts * kNumLayers);
    for (int p = 0; p < kNumParts; ++p)
        for (int l = 0; l < kNumLayers; ++l) snapSamples_[(size_t)p * kNumLayers + l] = engine_.partSampleSetName(p, l);
}

std::vector<Json> RemoteControl::collectChanges() {
    std::vector<Json> out;
    const auto now = std::chrono::steady_clock::now();
    if (subParams_) {
        const auto& table = paramTable();
        std::vector<float> cur((size_t)P::kNumParams);
        const int parts = engine_.multiTimbral() ? kNumParts : 1;
        Json global = Json::object();
        for (int p = 0; p < parts; ++p) {
            engine_.getPartParams(p, cur.data());
            float* old = snap_.data() + (size_t)p * P::kNumParams;
            Json values = Json::object();
            for (int i = 0; i < P::kNumParams; ++i) {
                if (cur[(size_t)i] == old[i]) continue;
                const bool part = isPartParam(i);
                if (!part && p > 0) continue;  // global ones once
                (part ? values : global).set(table[(size_t)i].name, valueJson(i, cur[(size_t)i]));
            }
            std::copy(cur.begin(), cur.end(), old);
            Json mix = Json::object();
            const auto& mt = partMixTable();
            for (int i = 0; i < Mix::Count; ++i) {
                float& o = snapMix_[(size_t)p * Mix::Count + i];
                const float v = engine_.getPartMix(p, i);
                if (v == o) continue;
                o = v;
                if (mt[(size_t)i].kind == ParamKind::Bool) mix.set(mt[(size_t)i].name, v > 0.5f);
                else mix.set(mt[(size_t)i].name, (double)v);
            }
            Json samples = Json::object();
            for (int l = 0; l < kNumLayers; ++l) {
                std::string& o = snapSamples_[(size_t)p * kNumLayers + l];
                const std::string v = engine_.partSampleSetName(p, l);
                if (v == o) continue;
                o = v;
                samples.set(layerKey(l), v);
            }
            if (values.size() || mix.size() || samples.size()) {
                Json n = Json::object();
                n.set("event", "changed");
                n.set("part", p);
                if (values.size()) n.set("values", values);
                if (mix.size()) n.set("mix", mix);
                if (samples.size()) n.set("samples", samples);
                out.push_back(std::move(n));
            }
        }
        if (global.size()) {
            Json n = Json::object();
            n.set("event", "changed");
            n.set("part", -1);
            n.set("values", global);
            out.push_back(std::move(n));
        }
        const bool multi = engine_.multiTimbral();
        const int sel = engine_.selectedPart();
        if (multi != snapMulti_ || sel != snapSelected_) {
            snapMulti_ = multi;
            snapSelected_ = sel;
            Json n = Json::object();
            n.set("event", "mode");
            n.set("multi", multi);
            n.set("selected", sel);
            out.push_back(std::move(n));
        }
    }
    const bool due = now - lastStatus_ >= std::chrono::milliseconds(std::max(50, intervalMs_ * 4));
    if (due && (subStatus_ || subMeters_)) {
        lastStatus_ = now;
        if (subStatus_) {
            Json n = Json::object();
            n.set("event", "status");
            for (const auto& m : statusJson().members())
                if (m.first != "ok") n.set(m.first, m.second);
            out.push_back(std::move(n));
        }
        if (subMeters_) {
            Json q = Json::object();
            q.set("op", "meters");
            Json n = Json::object();
            n.set("event", "meters");
            for (const auto& m : handle(q).members())
                if (m.first != "ok") n.set(m.first, m.second);
            out.push_back(std::move(n));
        }
    }
    return out;
}

// ---------------------------------------------------------------- values

Json RemoteControl::valueJson(int id, float v) const {
    const ParamInfo& info = paramTable()[(size_t)id];
    if (info.kind == ParamKind::Enum) {
        const int idx = (int)v;
        if (idx >= 0 && idx < (int)info.options.size()) return Json(info.options[(size_t)idx]);
    }
    if (info.kind == ParamKind::Bool) return Json(v > 0.5f);
    return Json((double)v);
}

bool RemoteControl::valueFrom(int id, const Json& j, float& out, std::string& error) const {
    const ParamInfo& info = paramTable()[(size_t)id];
    if (j.isString()) {
        for (size_t k = 0; k < info.options.size(); ++k)
            if (info.options[k] == j.string() || lower(info.options[k]) == lower(j.string())) {
                out = (float)k;
                return true;
            }
        error = "invalid value '" + j.string() + "' for " + info.name;
        return false;
    }
    if (j.isNumber() || j.isBool()) {
        out = (float)j.number();
        return true;
    }
    error = "invalid type for " + info.name;
    return false;
}

int RemoteControl::partOf(const Json& req) const {
    const Json* v = req.find("part");
    if (!v || !v->isNumber()) return engine_.multiTimbral() ? engine_.selectedPart() : 0;
    return std::max(0, std::min(kNumParts - 1, (int)v->number()));
}

Json RemoteControl::paramsJson(int part, const Json* names, bool changedOnly) const {
    const auto& table = paramTable();
    std::vector<float> cur((size_t)P::kNumParams);
    engine_.getPartParams(part, cur.data());
    Json values = Json::object();
    if (names && names->isArray()) {
        for (size_t k = 0; k < names->size(); ++k) {
            const Json& n = names->at(k);
            const std::string name = n.isString() ? n.string() : std::string();
            int id = n.isNumber() ? (int)n.number() : findParam(name);
            if (id < 0 || id >= P::kNumParams) continue;
            values.set(table[(size_t)id].name, valueJson(id, cur[(size_t)id]));
        }
        return values;
    }
    if (names && names->isString()) {  // prefix, e.g. "a." or "fx."
        const std::string pre = names->string();
        for (int i = 0; i < P::kNumParams; ++i)
            if (table[(size_t)i].name.rfind(pre, 0) == 0 && (!changedOnly || cur[(size_t)i] != table[(size_t)i].def))
                values.set(table[(size_t)i].name, valueJson(i, cur[(size_t)i]));
        return values;
    }
    for (int i = 0; i < P::kNumParams; ++i)
        if (!changedOnly || cur[(size_t)i] != table[(size_t)i].def) values.set(table[(size_t)i].name, valueJson(i, cur[(size_t)i]));
    return values;
}

Json RemoteControl::mixJson(int part) const {
    Json mix = Json::object();
    const auto& mt = partMixTable();
    for (int i = 0; i < Mix::Count; ++i) {
        const float v = engine_.getPartMix(part, i);
        if (mt[(size_t)i].kind == ParamKind::Bool) mix.set(mt[(size_t)i].name, v > 0.5f);
        else mix.set(mt[(size_t)i].name, (double)v);
    }
    return mix;
}

std::string RemoteControl::resolve(const std::string& path, const std::string& root) const {
    if (path.empty()) return path;
    const auto p = utf8Path(path);
    if (p.is_absolute() || root.empty()) return path;
    return utf8String(utf8Path(root) / p);
}

// ---------------------------------------------------------------- state

Json RemoteControl::stateJson(bool compact) {
    const bool multi = engine_.multiTimbral();
    std::string text = multi ? multiToJson(captureMulti(engine_, "Remote", config_.libraryRoot))
                             : presetToJson(capturePreset(engine_, "Remote", config_.libraryRoot));
    Json state;
    std::string err;
    Json::parse(text, state, err);
    if (compact) {  // only the values that differ from the defaults (missing = default when loading)
        const auto& table = paramTable();
        auto strip = [&](const Json& params) {
            Json out = Json::object();
            for (const auto& kv : params.members()) {
                const int id = findParam(kv.first);
                if (id >= 0) {
                    float v = 0.0f;
                    std::string e;
                    if (valueFrom(id, kv.second, v, e) && v == table[(size_t)id].def) continue;
                }
                out.set(kv.first, kv.second);
            }
            return out;
        };
        Json c = Json::object();
        for (const auto& m : state.members()) {
            if (m.first == "params") {
                c.set("params", strip(m.second));
            } else if (m.first == "global" && m.second.isObject()) {
                Json g = Json::object();
                for (const auto& gm : m.second.members()) g.set(gm.first, gm.first == "params" ? strip(gm.second) : gm.second);
                c.set("global", g);
            } else if (m.first == "parts" && m.second.isArray()) {
                Json parts = Json::array();
                for (size_t k = 0; k < m.second.size(); ++k) {
                    const Json& part = m.second.at(k);
                    Json np = Json::object();
                    for (const auto& pm : part.members()) {
                        if (pm.first == "preset" && pm.second.isObject()) {
                            Json pr = Json::object();
                            for (const auto& x : pm.second.members()) pr.set(x.first, x.first == "params" ? strip(x.second) : x.second);
                            np.set("preset", pr);
                        } else {
                            np.set(pm.first, pm.second);
                        }
                    }
                    parts.push(np);
                }
                c.set("parts", parts);
            } else {
                c.set(m.first, m.second);
            }
        }
        state = std::move(c);
    }
    Json r = okReply();
    r.set("multi", multi);
    r.set("selected", engine_.selectedPart());
    r.set("state", state);
    return r;
}

bool RemoteControl::setState(const Json& state, Json& reply) {
    const std::string format = strField(state, "format");
    std::vector<std::string> warnings;
    std::string err;
    if (format == "hsynth-multi") {
        MultiPreset m;
        if (!multiFromJson(state.dump(0), m, err)) { reply = errorReply(err); return false; }
        warnings = m.warnings;
        if (hooks_.setMultiTimbral) hooks_.setMultiTimbral(true);
        if (!applyMulti(engine_, m, config_.libraryRoot, warnings)) { reply = errorReply("could not apply the multi"); return false; }
        engine_.setMultiTimbral(true);
        if (hooks_.presetLoaded) hooks_.presetLoaded(-1, m.name);
    } else {
        Preset p;
        if (!presetFromJson(state.dump(0), p, err)) { reply = errorReply(err); return false; }
        warnings = p.warnings;
        if (hooks_.setMultiTimbral) hooks_.setMultiTimbral(false);
        engine_.setMultiTimbral(false);
        if (!applyPreset(engine_, p, config_.libraryRoot, warnings)) { reply = errorReply("could not apply the preset"); return false; }
        if (hooks_.presetLoaded) hooks_.presetLoaded(0, p.name);
    }
    reply = okReply();
    if (!warnings.empty()) reply.set("warnings", stringArray(warnings));
    changed();
    return true;
}

// ---------------------------------------------------------------- lists

Json RemoteControl::presetsJson(const Json& req) {
    if (!index_ || boolField(req, "rescan", false)) {
        if (!index_) index_ = std::make_unique<PresetIndex>();
        std::vector<std::pair<std::string, bool>> folders;
        for (const auto& d : config_.presetDirs) folders.push_back({d, d == config_.userPresetDir});
        if (!config_.userPresetDir.empty() &&
            std::find(config_.presetDirs.begin(), config_.presetDirs.end(), config_.userPresetDir) == config_.presetDirs.end())
            folders.push_back({config_.userPresetDir, true});
        index_->setFolders(folders);
        index_->rescan();
    }
    PresetQuery q;
    q.text = strField(req, "query");
    q.category = strField(req, "category");
    q.favoritesOnly = boolField(req, "favorites", false);
    q.userOnly = boolField(req, "user", false);
    const std::vector<int> hits = index_->query(q);
    const int offset = std::max(0, intField(req, "offset", 0));
    const int count = std::max(0, std::min(1000, intField(req, "count", 100)));
    Json list = Json::array();
    for (int k = offset; k < (int)hits.size() && k < offset + count; ++k) {
        const PresetInfo& p = index_->all()[(size_t)hits[(size_t)k]];
        Json o = Json::object();
        o.set("name", p.name);
        o.set("category", p.category);
        o.set("file", p.file);
        if (p.user) o.set("user", true);
        if (p.favorite) o.set("favorite", true);
        if (!p.tags.empty()) o.set("tags", stringArray(p.tags));
        list.push(o);
    }
    Json r = okReply();
    r.set("total", (int)hits.size());
    r.set("offset", offset);
    r.set("presets", list);
    if (boolField(req, "categories", false)) {
        Json cats = Json::array();
        for (const auto& c : index_->categories()) {
            Json o = Json::array();
            o.push(c.first);
            o.push(c.second);
            cats.push(o);
        }
        r.set("categories", cats);
    }
    return r;
}

Json RemoteControl::filesJson(const Json& req) {
    // kind: "samples" (library), "impulses" (library), "midi" (MIDI folder), "presets" (user presets)
    const std::string kind = strField(req, "kind");
    std::string root = kind == "midi" ? config_.midiDir : kind == "presets" ? config_.userPresetDir : config_.libraryRoot;
    std::vector<std::string> exts;
    if (kind == "midi") exts = {".mid", ".midi", ".smf"};
    else if (kind == "presets") exts = {".json", ".hsp", ".hsm"};
    else if (kind == "impulses") exts = {".wav", ".flac"};
    else exts = {".sfz", ".wav", ".flac"};
    const std::string sub = strField(req, "dir");
    if (sub.find("..") != std::string::npos) return errorReply("dir must stay inside the folder");
    if (root.empty()) return errorReply("no folder for '" + kind + "'");
    const auto base = utf8Path(root);
    const auto dir = sub.empty() ? base : base / utf8Path(sub);
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return errorReply("not a folder: " + sub);
    Json dirs = Json::array(), files = Json::array();
    std::vector<std::string> d, f;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utf8String(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(ec)) {
            d.push_back(name);
        } else {
            const std::string ext = lower(utf8String(it->path().extension()));
            if (std::find(exts.begin(), exts.end(), ext) != exts.end()) f.push_back(name);
        }
    }
    std::sort(d.begin(), d.end());
    std::sort(f.begin(), f.end());
    const int offset = std::max(0, intField(req, "offset", 0));
    const int count = std::max(0, std::min(2000, intField(req, "count", 500)));
    for (const auto& s : d) dirs.push(s);
    for (int k = offset; k < (int)f.size() && k < offset + count; ++k) files.push(f[(size_t)k]);
    Json r = okReply();
    r.set("kind", kind.empty() ? "samples" : kind);
    r.set("dir", sub);
    r.set("dirs", dirs);
    r.set("files", files);
    r.set("total", (int)f.size());
    return r;
}

// ---------------------------------------------------------------- songs

std::string RemoteControl::songLoad(const std::string& path, Json& info) {
    if (hooks_.loadSong) return hooks_.loadSong(path, info);
    // built in (hosts without their own player)
    auto song = std::make_unique<Song>();
    std::string err;
    if (!loadSmf(path, *song, err)) return err;
    if (programs_) programs_->stop();
    if (!index_) presetsJson(Json::object());
    GmBank bank;
    bank.build(index_->all());
    engine_.songPlay(false);
    std::vector<std::string> report;
    std::vector<SongPart> parts;
    if (hooks_.setMultiTimbral) hooks_.setMultiTimbral(true);
    prepareSong(engine_, *song, bank, config_.libraryRoot, songLoaded_, report, &parts);
    if (!programs_) programs_ = std::make_unique<SongProgramLoader>();
    programs_->start(engine_, *song, bank, config_.libraryRoot);
    info.set("name", song->name);
    info.set("length", song->length);
    info.set("tempo", (double)song->initialTempo);
    Json ch = Json::array();
    for (const SongPart& p : parts) {
        Json o = Json::object();
        o.set("ch", p.channel + 1);
        o.set("drum", p.drum);
        o.set("gm", p.gmName);
        o.set("preset", p.presetName);
        ch.push(o);
    }
    info.set("channels", ch);
    if (!report.empty()) info.set("report", stringArray(report));
    songName_ = song->name;
    engine_.setSong(std::move(song));
    engine_.songSeek(0.0);
    return "";
}

// ---------------------------------------------------------------- status

Json RemoteControl::statusJson() {
    Json r = okReply();
    const EngineStats st = engine_.stats();
    r.set("multi", engine_.multiTimbral());
    r.set("selected", engine_.selectedPart());
    r.set("voices", st.activeVoices);
    r.set("cpu", (double)st.cpuLoad);
    r.set("sample_rate", (double)engine_.sampleRate());
    r.set("transport", engine_.transportPlaying());
    r.set("clock_bpm", (double)engine_.midiClockTempo());
    Json song = Json::object();
    song.set("playing", engine_.songPlaying());
    song.set("position", engine_.songPosition());
    song.set("length", engine_.songLength());
    if (!songName_.empty()) song.set("name", songName_);
    r.set("song", song);
    static const char* kPhrase[] = {"empty", "recording", "playing", "overdub", "stopped"};
    Json ph = Json::object();
    ph.set("state", kPhrase[std::max(0, std::min(4, (int)engine_.phraseState()))]);
    ph.set("bars", engine_.phraseBars());
    ph.set("length", engine_.phraseLength());
    ph.set("position", engine_.phrasePosition());
    ph.set("events", engine_.phraseEventCount());
    r.set("phrase", ph);
    static const char* kLooper[] = {"empty", "recording", "playing", "overdub", "stopped"};
    const fx::LooperStatus ls = engine_.looperStatus();
    Json lo = Json::object();
    lo.set("state", kLooper[std::max(0, std::min(4, (int)ls.state))]);
    lo.set("length", (double)ls.lengthSec);
    lo.set("position", (double)ls.positionSec);
    lo.set("undo", ls.canUndo);
    r.set("looper", lo);
    Json parts = Json::array();
    const int n = engine_.multiTimbral() ? kNumParts : 1;
    for (int p = 0; p < n; ++p) {
        Json o = Json::object();
        Json s = Json::object();
        for (int l = 0; l < kNumLayers; ++l) s.set(layerKey(l), engine_.partSampleSetName(p, l));
        o.set("samples", s);
        o.set("voices", engine_.partActiveVoices(p));
        o.set("program", engine_.partProgram(p));
        parts.push(o);
    }
    r.set("parts", parts);
    Json irs = Json::object();
    irs.set("cab", engine_.fxImpulseName(0));
    irs.set("reverb", engine_.fxImpulseName(1));
    r.set("impulses", irs);
    if (hooks_.status) hooks_.status(r);
    return r;
}

// ---------------------------------------------------------------- requests

Json RemoteControl::handle(const Json& req) {
    const std::string op = strField(req, "op");
    const auto& table = paramTable();

    if (op == "hello") {
        Json r = okReply();
        r.set("device", config_.deviceName);
        r.set("device_id", config_.deviceId);
        r.set("version", config_.version);
        r.set("protocol", (int)sysex::kVersion);
        r.set("params", P::kNumParams);
        r.set("parts", kNumParts);
        r.set("layers", kNumLayers);
        r.set("multi", engine_.multiTimbral());
        r.set("selected", engine_.selectedPart());
        Json ops = Json::array();
        for (const char* o : kOps) ops.push(o);
        r.set("ops", ops);
        return r;
    }

    if (op == "param_info") {
        const int offset = std::max(0, intField(req, "offset", 0));
        const int count = std::max(0, std::min(P::kNumParams, intField(req, "count", 100)));
        const std::string prefix = strField(req, "prefix");
        Json list = Json::array();
        int total = 0;
        for (int i = 0; i < P::kNumParams; ++i) {
            const ParamInfo& info = table[(size_t)i];
            if (!prefix.empty() && info.name.rfind(prefix, 0) != 0) continue;
            const int k = total++;
            if (k < offset || k >= offset + count) continue;
            Json o = Json::object();
            o.set("id", i);
            o.set("name", info.name);
            o.set("group", info.group);
            o.set("label", info.label);
            o.set("kind", kKinds[(int)info.kind]);
            o.set("min", (double)info.min);
            o.set("max", (double)info.max);
            o.set("def", valueJson(i, info.def));
            if (!info.unit.empty()) o.set("unit", info.unit);
            if (info.curve == ParamCurve::Exp) o.set("curve", "exp");
            if (!info.options.empty()) o.set("options", stringArray(info.options));
            o.set("part", isPartParam(i));
            list.push(o);
        }
        Json r = okReply();
        r.set("total", total);
        r.set("offset", offset);
        r.set("params", list);
        return r;
    }

    if (op == "mix_info") {
        Json list = Json::array();
        for (const ParamInfo& info : partMixTable()) {
            Json o = Json::object();
            o.set("name", info.name);
            o.set("label", info.label);
            o.set("kind", kKinds[(int)info.kind]);
            o.set("min", (double)info.min);
            o.set("max", (double)info.max);
            list.push(o);
        }
        Json r = okReply();
        r.set("mix", list);
        return r;
    }

    if (op == "get") {
        const int part = partOf(req);
        Json r = okReply();
        r.set("part", part);
        r.set("values", paramsJson(part, req.find("names"), boolField(req, "changed", false)));
        return r;
    }

    if (op == "set") {
        const int part = partOf(req);
        const Json* values = req.find("values");
        if (!values || !values->isObject()) return errorReply("'values' must be an object {name: value}");
        Json applied = Json::object();
        std::vector<std::string> errors;
        for (const auto& kv : values->members()) {
            int id = findParam(kv.first);
            if (id < 0) { errors.push_back("unknown parameter '" + kv.first + "'"); continue; }
            float v = 0.0f;
            std::string e;
            if (!valueFrom(id, kv.second, v, e)) { errors.push_back(e); continue; }
            const bool relative = boolField(req, "relative", false);
            if (relative && !kv.second.isString()) v += engine_.getPartParam(part, id);
            engine_.setPartParam(part, id, v);
            applied.set(table[(size_t)id].name, valueJson(id, engine_.getPartParam(part, id)));
        }
        if (applied.size()) changed();
        Json r = errors.empty() ? okReply() : errorReply(errors.front());
        r.set("part", part);
        r.set("values", applied);
        if (errors.size() > 1) r.set("errors", stringArray(errors));
        return r;
    }

    if (op == "mix_get") {
        Json r = okReply();
        if (const Json* p = req.find("part"); p && p->isNumber()) {
            const int part = partOf(req);
            r.set("part", part);
            r.set("mix", mixJson(part));
        } else {
            Json all = Json::array();
            for (int p = 0; p < kNumParts; ++p) all.push(mixJson(p));
            r.set("parts", all);
        }
        return r;
    }

    if (op == "mix_set") {
        const int part = partOf(req);
        const Json* values = req.find("values");
        if (!values || !values->isObject()) return errorReply("'values' must be an object {name: value}");
        const auto& mt = partMixTable();
        std::vector<std::string> errors;
        for (const auto& kv : values->members()) {
            int id = -1;
            for (int i = 0; i < Mix::Count; ++i)
                if (mt[(size_t)i].name == kv.first) id = i;
            if (id < 0 || !(kv.second.isNumber() || kv.second.isBool())) { errors.push_back("bad mix setting '" + kv.first + "'"); continue; }
            engine_.setPartMix(part, id, (float)kv.second.number());
        }
        changed();
        Json r = errors.empty() ? okReply() : errorReply(errors.front());
        r.set("part", part);
        r.set("mix", mixJson(part));
        return r;
    }

    if (op == "get_state") return stateJson(boolField(req, "compact", true));

    if (op == "set_state") {
        const Json* state = req.find("state");
        if (!state || !state->isObject()) return errorReply("'state' must be a preset or multi object");
        Json r;
        setState(*state, r);
        return r;
    }

    // setup files (setup.h): state + MIDI learn + host settings in one object
    if (op == "setup_get") {
        Json host = Json::object();
        if (hooks_.hostSettings) hooks_.hostSettings(host);
        Json setup;
        std::string err;
        Json::parse(makeSetupJson(engine_, strField(req, "name").empty() ? "Setup" : strField(req, "name"),
                                  config_.libraryRoot, config_.learn, host), setup, err);
        Json r = okReply();
        r.set("setup", setup);
        return r;
    }
    if (op == "setup_set") {
        const Json* setup = req.find("setup");
        if (!setup || !setup->isObject()) return errorReply("'setup' must be an hsynth-setup object");
        SetupResult res;
        std::string err;
        if (!applySetupJson(engine_, setup->dump(0), config_.libraryRoot, config_.learn, res, err)) return errorReply(err);
        if (res.hasState && hooks_.setMultiTimbral) hooks_.setMultiTimbral(res.multi);
        if (hooks_.applyHostSettings) hooks_.applyHostSettings(res.host);
        if (res.hasState && hooks_.presetLoaded) hooks_.presetLoaded(res.multi ? -1 : 0, res.name);
        changed();
        Json r = okReply();
        r.set("name", res.name);
        r.set("multi", engine_.multiTimbral());
        if (!res.warnings.empty()) r.set("warnings", stringArray(res.warnings));
        return r;
    }

    if (op == "mode") {
        if (const Json* m = req.find("multi")) {
            const bool on = m->boolean();
            if (hooks_.setMultiTimbral) hooks_.setMultiTimbral(on);
            engine_.setMultiTimbral(on);
            changed();
        }
        if (const Json* m = req.find("mpe"); m && hooks_.setMpe) {
            hooks_.setMpe(m->boolean());
            changed();
        }
        Json r = okReply();
        r.set("multi", engine_.multiTimbral());
        r.set("selected", engine_.selectedPart());
        return r;
    }

    if (op == "select_part") {
        engine_.selectPart(intField(req, "part", 0));
        changed();
        Json r = okReply();
        r.set("selected", engine_.selectedPart());
        return r;
    }

    if (op == "presets") return presetsJson(req);

    if (op == "load_preset") {
        std::string file = strField(req, "file");
        if (file.empty()) {  // by name
            const std::string name = lower(strField(req, "name"));
            if (name.empty()) return errorReply("give 'file' or 'name'");
            if (!index_) presetsJson(Json::object());
            for (const auto& p : index_->all())
                if (lower(p.name) == name) { file = p.file; break; }
            if (file.empty()) return errorReply("no preset named '" + strField(req, "name") + "'");
        }
        Preset p;
        std::string err;
        if (!loadPresetFile(file, p, err)) return errorReply(err);
        std::vector<std::string> warnings = p.warnings;
        const bool multi = engine_.multiTimbral();
        const int part = partOf(req);
        bool ok;
        if (multi) ok = applyPartPreset(engine_, part, p, config_.libraryRoot, warnings);
        else ok = applyPreset(engine_, p, config_.libraryRoot, warnings);
        if (!ok) return errorReply("could not load " + file);
        if (hooks_.presetLoaded) hooks_.presetLoaded(multi ? part : 0, p.name);
        changed();
        Json r = okReply();
        r.set("name", p.name);
        r.set("part", multi ? part : 0);
        if (!warnings.empty()) r.set("warnings", stringArray(warnings));
        return r;
    }

    if (op == "save_preset") {
        const std::string name = strField(req, "name");
        if (name.empty()) return errorReply("give a 'name'");
        if (config_.userPresetDir.empty()) return errorReply("no user preset folder");
        const int part = partOf(req);
        Preset p = capturePartPreset(engine_, part, name, config_.libraryRoot);
        p.category = strField(req, "category");
        p.comment = strField(req, "comment");
        if (const Json* t = req.find("tags"); t && t->isArray())
            for (size_t k = 0; k < t->size(); ++k)
                if (t->at(k).isString()) p.tags.push_back(t->at(k).string());
        std::string safe;
        for (char c : name) safe.push_back(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ? '_' : c);
        const std::string path = utf8String(utf8Path(config_.userPresetDir) / utf8Path(safe + ".json"));
        std::error_code ec;
        std::filesystem::create_directories(utf8Path(config_.userPresetDir), ec);
        std::string err;
        if (!savePresetFile(path, p, err)) return errorReply(err);
        if (index_) index_->rescan();
        Json r = okReply();
        r.set("file", path);
        return r;
    }

    if (op == "samples") {  // load a sample set / SFZ into a layer ("" clears it)
        const int part = partOf(req);
        const int layer = layerOf(req);
        const std::string file = strField(req, "file");
        if (file.empty()) {
            engine_.setPartSampleSet(part, layer, nullptr);
        } else {
            std::string err;
            std::vector<std::string> warnings;
            if (!loadPartSamples(engine_, part, layer, resolve(file, config_.libraryRoot), err, warnings))
                return errorReply(err);
            if (!warnings.empty()) {
                changed();
                Json r = okReply();
                r.set("warnings", stringArray(warnings));
                return r;
            }
        }
        changed();
        Json r = okReply();
        r.set("part", part);
        r.set("layer", layerKey(layer));
        r.set("name", engine_.partSampleSetName(part, layer));
        return r;
    }

    if (op == "impulse") {  // target "cab" / "reverb"
        const std::string t = strField(req, "target");
        const int target = t == "reverb" || t == "1" ? 1 : 0;
        const std::string file = strField(req, "file");
        if (file.empty()) {
            engine_.setFxImpulse(target, nullptr);
        } else {
            std::string err;
            auto ir = fx::loadImpulseResponse(resolve(file, config_.libraryRoot), engine_.sampleRate(), target, err);
            if (!ir) return errorReply(err);
            engine_.setFxImpulse(target, std::move(ir));
        }
        changed();
        Json r = okReply();
        r.set("name", engine_.fxImpulseName(target));
        return r;
    }

    if (op == "files") return filesJson(req);

    if (op == "note") {  // {"key":60,"vel":0.8,"on":true,"channel":1..16 (omitted: the selected part)}
        const int key = std::max(0, std::min(127, intField(req, "key", 60)));
        const float vel = (float)std::max(0.0, std::min(1.0, numField(req, "vel", 0.8)));
        const int ch = intField(req, "channel", 0);
        const uint32_t id = 0x7E000000u | (uint32_t)(ch & 0x1F) << 8 | (uint32_t)key;  // remote notes: own id space
        Event e = boolField(req, "on", true) ? Event::noteOn(id, key, vel) : Event::noteOff(id, key, vel);
        if (ch >= 1 && ch <= 16) e.channel = (uint8_t)(ch - 1);
        engine_.sendEvent(e);
        return okReply();
    }

    if (op == "event") {  // channel-wide controls
        const std::string type = strField(req, "type");
        const float v = (float)numField(req, "value", 0.0);
        const int ch = intField(req, "channel", 0);
        Event e;
        if (type == "bend") e = Event::control(EventType::PitchBend, std::max(-1.0f, std::min(1.0f, v)));
        else if (type == "mod") e = Event::control(EventType::ModWheel, v);
        else if (type == "pressure") e = Event::control(EventType::ChannelPressure, v);
        else if (type == "sustain") e = Event::control(EventType::Sustain, v);
        else if (type == "cc") e = Event::control(EventType::Controller, v, std::max(0, std::min(127, intField(req, "cc", 1))));
        else if (type == "all_notes_off") e = Event::control(EventType::AllNotesOff, 0.0f);
        else if (type == "all_sound_off") e = Event::control(EventType::AllSoundOff, 0.0f);
        else if (type == "program") {
            const int part = ch >= 1 && ch <= 16 ? ch - 1 : partOf(req);
            engine_.setPartProgram(part, std::max(0, std::min(127, intField(req, "program", (int)v))));
            changed();
            return okReply();
        } else return errorReply("unknown event type '" + type + "'");
        if (ch >= 1 && ch <= 16) e.channel = (uint8_t)(ch - 1);
        engine_.sendEvent(e);
        return okReply();
    }

    if (op == "panic") {
        for (int ch = 0; ch < 16; ++ch) {
            Event e = Event::control(EventType::AllSoundOff, 0.0f);
            e.channel = (uint8_t)ch;
            engine_.sendEvent(e);
        }
        engine_.sendEvent(Event::control(EventType::AllSoundOff, 0.0f));
        return okReply();
    }

    if (op == "transport") {
        if (const Json* p = req.find("play")) engine_.setTransport(p->boolean());
        if (const Json* t = req.find("tempo"); t && t->isNumber()) {
            engine_.setParam(P::fxChain(P::FxTempo), (float)t->number());
            changed();
        }
        Json r = okReply();
        r.set("playing", engine_.transportPlaying());
        r.set("tempo", (double)engine_.getParam(P::fxChain(P::FxTempo)));
        return r;
    }

    if (op == "looper") {
        const std::string cmd = strField(req, "cmd");
        fx::LooperCmd c = fx::LooperCmd::None;
        if (cmd == "tap" || cmd == "record") c = fx::LooperCmd::Tap;
        else if (cmd == "stop") c = fx::LooperCmd::Stop;
        else if (cmd == "clear") c = fx::LooperCmd::Clear;
        else if (cmd == "undo") c = fx::LooperCmd::Undo;
        else if (cmd == "play") c = fx::LooperCmd::Play;
        else if (!cmd.empty()) return errorReply("unknown looper cmd '" + cmd + "'");
        if (c != fx::LooperCmd::None) engine_.looperCommand(c);
        Json r = okReply();
        Json st = statusJson();
        if (const Json* l = st.find("looper")) r.set("looper", *l);
        return r;
    }

    if (op == "phrase") {
        const std::string cmd = strField(req, "cmd");
        if (const Json* b = req.find("bars"); b && b->isNumber()) engine_.setPhraseBars((int)b->number());
        if (cmd == "record") engine_.phraseRecord();
        else if (cmd == "play") engine_.phrasePlay();
        else if (cmd == "stop") engine_.phraseStop();
        else if (cmd == "clear") engine_.phraseClear();
        else if (cmd == "undo") engine_.phraseUndo();
        else if (cmd == "quantize" || cmd == "export") {
            engine_.requestPhraseSnapshot();
            std::vector<Engine::PhraseEvent> ev;
            double len = 0.0;
            bool got = false;
            for (int i = 0; i < 200 && !got; ++i) {
                got = engine_.takePhraseSnapshot(ev, len);
                if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!got) return errorReply("the audio is not running");
            if (cmd == "quantize") {
                if (len <= 0.0) return errorReply("no phrase");
                quantizePhrase(ev, len, numField(req, "grid", 0.25), numField(req, "strength", 1.0));
                engine_.replacePhrase(ev, len);
            } else {
                std::string name = strField(req, "file");
                if (name.empty()) name = "phrase.mid";
                if (name.find("..") != std::string::npos || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
                    return errorReply("'file' is a plain file name (saved in the MIDI folder)");
                if (config_.midiDir.empty()) return errorReply("no MIDI folder");
                std::string err;
                const double bpm = engine_.getParam(P::fxChain(P::FxTempo));
                const std::string path = utf8String(utf8Path(config_.midiDir) / utf8Path(name));
                if (!writePhraseSmf(path, ev, len, bpm, std::max(1, intField(req, "repeats", 1)), err)) return errorReply(err);
                Json r = okReply();
                r.set("file", path);
                return r;
            }
        } else if (!cmd.empty()) return errorReply("unknown phrase cmd '" + cmd + "'");
        Json r = okReply();
        Json st = statusJson();
        if (const Json* p = st.find("phrase")) r.set("phrase", *p);
        return r;
    }

    if (op == "songs") {
        Json q = Json::object();
        q.set("kind", "midi");
        if (const Json* d = req.find("dir")) q.set("dir", *d);
        if (const Json* o = req.find("offset")) q.set("offset", *o);
        if (const Json* c = req.find("count")) q.set("count", *c);
        return filesJson(q);
    }

    if (op == "song_load") {
        const std::string file = strField(req, "file");
        if (file.empty()) return errorReply("give a 'file'");
        if (file.find("..") != std::string::npos) return errorReply("the file must be inside the MIDI folder");
        Json info = Json::object();
        const std::string err = songLoad(resolve(file, config_.midiDir), info);
        if (!err.empty()) return errorReply(err);
        changed();
        Json r = okReply();
        for (const auto& m : info.members()) r.set(m.first, m.second);
        return r;
    }

    if (op == "song") {
        const std::string cmd = strField(req, "cmd");
        if (cmd == "play") engine_.songPlay(true);
        else if (cmd == "stop") engine_.songPlay(false);
        else if (cmd == "seek") engine_.songSeek(std::max(0.0, numField(req, "position", 0.0)));
        else if (cmd == "rewind") engine_.songSeek(0.0);
        else if (!cmd.empty()) return errorReply("unknown song cmd '" + cmd + "'");
        Json r = okReply();
        Json st = statusJson();
        if (const Json* s = st.find("song")) r.set("song", *s);
        return r;
    }

    if (op == "status") return statusJson();

    if (op == "meters") {
        Engine::PartMeter m[kNumParts];
        engine_.partMeters(m);
        Json parts = Json::array();
        for (int p = 0; p < kNumParts; ++p) {
            Json o = Json::object();
            o.set("level", (double)m[p].level);
            o.set("voices", m[p].voices);
            o.set("volume", (double)m[p].volume);
            o.set("expression", (double)m[p].expression);
            o.set("pan", (double)m[p].pan);
            o.set("bend", (double)m[p].bend);
            o.set("mod", (double)m[p].mod);
            Json notes = Json::array();
            for (int k = 0; k < 128; ++k)
                if (m[p].notes[k / 32] & (1u << (k % 32))) notes.push(k);
            o.set("notes", notes);
            parts.push(o);
        }
        Json r = okReply();
        r.set("parts", parts);
        r.set("output_peak", (double)engine_.stats().outputPeak);
        return r;
    }

    if (op == "settings") {
        if (const Json* v = req.find("max_voices"); v && v->isNumber()) engine_.setMaxVoices((int)v->number());
        if (const Json* v = req.find("quality")) {
            const std::string q = v->isString() ? lower(v->string()) : std::string();
            const int qi = v->isNumber() ? (int)v->number() : q == "low" ? 0 : q == "high" ? 2 : 1;
            engine_.setQuality((Quality)std::max(0, std::min(2, qi)));
        }
        if (const Json* v = req.find("render_threads"); v && v->isNumber()) engine_.setRenderThreads((int)v->number());
        static const char* kQ[] = {"low", "normal", "high"};
        Json r = okReply();
        r.set("max_voices", engine_.maxVoices());
        r.set("quality", kQ[std::max(0, std::min(2, (int)engine_.quality()))]);
        r.set("render_threads", engine_.renderThreads());
        return r;
    }

    if (op == "learn") {
        if (!config_.learn) return errorReply("MIDI learn is not available on this device");
        MidiLearn& learn = *config_.learn;
        const std::string cmd = strField(req, "cmd");
        if (cmd == "arm") {
            const int id = findParam(strField(req, "param"));
            if (id < 0) return errorReply("unknown parameter");
            const Json* p = req.find("part");
            learn.arm(id, p && p->isNumber() ? (int)p->number() : -1);
        } else if (cmd == "cancel") {
            learn.cancel();
        } else if (cmd == "set") {
            const Json* m = req.find("mappings");
            if (!m) return errorReply("give 'mappings' (the learn file format)");
            std::string err;
            if (!learn.fromJson(m->dump(0), err)) return errorReply(err);
            changed();
        } else if (cmd == "clear") {
            learn.setMappings({});
            changed();
        } else if (cmd == "remove") {
            const int id = findParam(strField(req, "param"));
            if (id >= 0) learn.removeParam(id);
            changed();
        } else if (!cmd.empty() && cmd != "get") {
            return errorReply("unknown learn cmd '" + cmd + "'");
        }
        Json r = okReply();
        Json maps;
        std::string err;
        Json::parse(learn.toJson(), maps, err);
        r.set("mappings", maps);
        const int armed = learn.armedParam();
        r.set("armed", armed >= 0 ? Json(table[(size_t)armed].name) : Json());
        return r;
    }

    if (op == "subscribe" || op == "unsubscribe") {
        const bool on = op == "subscribe";
        const Json* what = req.find("what");  // ["params","status","meters"]; omitted = params
        auto has = [&](const char* w) {
            if (!what) return std::string(w) == "params";
            if (what->isString()) return what->string() == w || what->string() == "all";
            for (size_t k = 0; what->isArray() && k < what->size(); ++k)
                if (what->at(k).isString() && (what->at(k).string() == w || what->at(k).string() == "all")) return true;
            return false;
        };
        if (has("params")) subParams_ = on;
        if (has("status")) subStatus_ = on;
        if (has("meters")) subMeters_ = on;
        if (const Json* i = req.find("interval_ms"); i && i->isNumber()) intervalMs_ = std::max(10, std::min(2000, (int)i->number()));
        Json r = okReply();
        Json w = Json::array();
        if (subParams_) w.push("params");
        if (subStatus_) w.push("status");
        if (subMeters_) w.push("meters");
        r.set("subscribed", w);
        r.set("interval_ms", intervalMs_);
        return r;
    }

    if (op.rfind("host.", 0) == 0 && hooks_.extra) {
        Json r = okReply();
        if (hooks_.extra(op, req, r)) return r;
    }
    return errorReply(op.empty() ? "no 'op'" : "unknown op '" + op + "'");
}

}  // namespace hsynth
