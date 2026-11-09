#include "hsynth/preset.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "hsynth/json.h"
#include "hsynth/fileio.h"

namespace hsynth {

Preset::Preset() : params((size_t)P::kNumParams) { defaultParams(params.data()); }

static const char* kLayerKeys[kNumLayers] = {"a", "b", "c"};

namespace {
void setFx(Preset& p, int block, int local, float v) { p.params[(size_t)P::fxp(block, local)] = sanitizeParam(P::fxp(block, local), v); }
}  // namespace

// Presets written before the effect rack (chorus/delay/reverb/2-band EQ as
// fixed globals). The default chain is chorus -> delay -> reverb, as before;
// levels are converted so the wet/dry balance stays close to the old one.
bool applyLegacyParam(Preset& p, const std::string& name, float v) {
    namespace F = fx;
    if (name == "chorus.mix") {
        setFx(p, F::Chorus, F::chorus::On, v > 0.0001f ? 1.0f : 0.0f);
        setFx(p, F::Chorus, F::chorus::Mix, 0.35f * v / (1.0f - 0.5f * v));
    } else if (name == "chorus.rate") setFx(p, F::Chorus, F::chorus::Rate, v);
    else if (name == "chorus.depth") setFx(p, F::Chorus, F::chorus::Depth, v);
    else if (name == "delay.mix") {
        setFx(p, F::Delay, F::delay::On, v > 0.0001f ? 1.0f : 0.0f);
        setFx(p, F::Delay, F::delay::Mix, 0.5f * v);
    } else if (name == "delay.time") setFx(p, F::Delay, F::delay::Time, v);
    else if (name == "delay.feedback") setFx(p, F::Delay, F::delay::Feedback, v);
    else if (name == "delay.tone") {
        const float a = 0.05f + 0.9f * v;  // old one-pole coefficient -> corner frequency
        setFx(p, F::Delay, F::delay::HighCut, -std::log(1.0f - a) * 48000.0f / (2.0f * kPi));
    } else if (name == "delay.pingpong") setFx(p, F::Delay, F::delay::Mode, v > 0.5f ? 1.0f : 0.0f);
    else if (name == "reverb.mix") {
        setFx(p, F::Reverb, F::reverb::On, v > 0.0001f ? 1.0f : 0.0f);
        setFx(p, F::Reverb, F::reverb::Mix, 0.5f * v);
    } else if (name == "reverb.size") {
        setFx(p, F::Reverb, F::reverb::Size, v);
        setFx(p, F::Reverb, F::reverb::Decay, 0.3f + v * v * 8.0f);
    } else if (name == "reverb.damp") setFx(p, F::Reverb, F::reverb::Damp, v);
    else if (name == "reverb.predelay") setFx(p, F::Reverb, F::reverb::PreDelay, v * 1000.0f);
    else if (name == "eq.low" || name == "eq.high") {
        if (std::fabs(v) < 0.01f) return true;
        const bool low = name == "eq.low";
        setFx(p, F::Peq, low ? F::peq::LsFreq : F::peq::HsFreq, low ? 200.0f : 4000.0f);
        setFx(p, F::Peq, low ? F::peq::LsGain : F::peq::HsGain, v);
        bool placed = false;
        for (int s = 0; s < F::kFxSlots && !placed; ++s) placed = (int)p.params[(size_t)P::fxSlot(s)] == F::Peq;
        for (int s = 3; s < F::kFxSlots && !placed; ++s)
            if ((int)p.params[(size_t)P::fxSlot(s)] == F::None) { p.params[(size_t)P::fxSlot(s)] = (float)F::Peq; placed = true; }
    } else {
        return false;
    }
    return true;
}

namespace {
enum class ParamFilter { All, Part, Global };
bool keepParam(ParamFilter f, int id) {
    return f == ParamFilter::All || (f == ParamFilter::Part) == isPartParam(id);
}

Json presetToValue(const Preset& p, ParamFilter filter) {
    Json root = Json::object();
    root.set("format", "hsynth-preset");
    root.set("version", 1);
    root.set("name", p.name);
    if (!p.category.empty()) root.set("category", p.category);
    if (!p.author.empty()) root.set("author", p.author);
    if (!p.comment.empty()) root.set("comment", p.comment);
    if (!p.tags.empty()) {
        Json tags = Json::array();
        for (const auto& t : p.tags) tags.push(t);
        root.set("tags", tags);
    }
    Json samples = Json::object();
    for (int l = 0; l < kNumLayers; ++l) samples.set(kLayerKeys[l], p.samples[l]);
    root.set("samples", samples);
    if (!p.impulses[0].empty() || !p.impulses[1].empty()) {
        Json irs = Json::object();
        irs.set("cab", p.impulses[0]);
        irs.set("reverb", p.impulses[1]);
        root.set("impulses", irs);
    }
    Json params = Json::object();
    const auto& table = paramTable();
    for (int i = 0; i < P::kNumParams; ++i) {
        if (!keepParam(filter, i)) continue;
        const ParamInfo& info = table[(size_t)i];
        const float v = i < (int)p.params.size() ? p.params[(size_t)i] : info.def;
        if (info.kind == ParamKind::Enum) {
            const int idx = (int)v;
            if (idx >= 0 && idx < (int)info.options.size()) { params.set(info.name, info.options[(size_t)idx]); continue; }
        }
        if (info.kind == ParamKind::Bool) { params.set(info.name, v > 0.5f); continue; }
        params.set(info.name, (double)v);
    }
    root.set("params", params);
    return root;
}

bool presetFromValue(const Json& root, Preset& out, std::string& error) {
    if (!root.isObject()) { error = "preset root must be an object"; return false; }
    Preset p;
    if (auto f = root.find("format"); f && f->isString() && f->string() != "hsynth-preset")
        p.warnings.push_back("unexpected format '" + f->string() + "'");
    if (auto v = root.find("version"); v && v->number() > 1)
        p.warnings.push_back("preset version " + std::to_string((int)v->number()) + " is newer than this engine");
    if (auto n = root.find("name"); n && n->isString()) p.name = n->string();
    if (auto n = root.find("category"); n && n->isString()) p.category = n->string();
    if (auto n = root.find("author"); n && n->isString()) p.author = n->string();
    if (auto n = root.find("comment"); n && n->isString()) p.comment = n->string();
    if (auto t = root.find("tags"); t && t->isArray())
        for (size_t i = 0; i < t->size(); ++i)
            if (t->at(i).isString() && !t->at(i).string().empty()) p.tags.push_back(t->at(i).string());
    if (auto s = root.find("samples"); s && s->isObject()) {
        for (int l = 0; l < kNumLayers; ++l)
            if (auto v = s->find(kLayerKeys[l]); v && v->isString()) p.samples[l] = v->string();
    }
    if (auto s = root.find("impulses"); s && s->isObject()) {
        if (auto v = s->find("cab"); v && v->isString()) p.impulses[0] = v->string();
        if (auto v = s->find("reverb"); v && v->isString()) p.impulses[1] = v->string();
    }
    const Json* params = root.find("params");
    if (!params || !params->isObject()) { error = "preset has no 'params' object"; return false; }
    const auto& table = paramTable();
    for (const auto& kv : params->members()) {
        const int id = findParam(kv.first);
        if (id < 0 && (kv.second.isNumber() || kv.second.isBool()) &&
            applyLegacyParam(p, kv.first, (float)kv.second.number()))
            continue;
        if (id < 0) { p.warnings.push_back("unknown parameter '" + kv.first + "' ignored"); continue; }
        const ParamInfo& info = table[(size_t)id];
        float v = info.def;
        if (kv.second.isString()) {
            bool found = false;
            for (size_t k = 0; k < info.options.size(); ++k)
                if (info.options[k] == kv.second.string()) { v = (float)k; found = true; break; }
            if (!found) {
                p.warnings.push_back("invalid value '" + kv.second.string() + "' for " + kv.first);
                continue;
            }
        } else if (kv.second.isNumber() || kv.second.isBool()) {
            v = (float)kv.second.number();
        } else {
            p.warnings.push_back("invalid type for " + kv.first);
            continue;
        }
        const float s = sanitizeParam(id, v);
        if (s != v && info.kind == ParamKind::Float) p.warnings.push_back(kv.first + " clamped to range");
        p.params[(size_t)id] = s;
    }
    out = std::move(p);
    return true;
}
}  // namespace

std::string presetToJson(const Preset& p) { return presetToValue(p, ParamFilter::All).dump(2) + "\n"; }

bool presetFromJson(const std::string& text, Preset& out, std::string& error) {
    Json root;
    if (!Json::parse(text, root, error)) { error = "JSON parse error: " + error; return false; }
    return presetFromValue(root, out, error);
}

// ------------------------------------------------------------------ multi

MultiPreset::MultiPreset() {
    global.name = "";
    for (int k = 0; k < kNumParts; ++k) defaultPartMix(k, mix[k]);
}

std::string multiToJson(const MultiPreset& m) {
    Json root = Json::object();
    root.set("format", "hsynth-multi");
    root.set("version", 1);
    root.set("name", m.name);
    if (!m.comment.empty()) root.set("comment", m.comment);
    root.set("selected", m.selectedPart + 1);
    Json global = Json::object();
    if (!m.global.impulses[0].empty() || !m.global.impulses[1].empty()) {
        Json irs = Json::object();
        irs.set("cab", m.global.impulses[0]);
        irs.set("reverb", m.global.impulses[1]);
        global.set("impulses", irs);
    }
    Json gp = presetToValue(m.global, ParamFilter::Global);
    global.set("params", *gp.find("params"));
    root.set("global", global);
    Json parts = Json::array();
    const auto& mixTable = partMixTable();
    for (int k = 0; k < kNumParts; ++k) {
        Json part = Json::object();
        Json mix = Json::object();
        for (int i = 0; i < Mix::Count; ++i) {
            const ParamInfo& info = mixTable[(size_t)i];
            if (info.kind == ParamKind::Bool) mix.set(info.name, m.mix[k][i] > 0.5f);
            else mix.set(info.name, (double)m.mix[k][i]);
        }
        part.set("mix", mix);
        Preset pp = m.parts[k];
        pp.impulses[0].clear();  // impulses are global
        pp.impulses[1].clear();
        part.set("preset", presetToValue(pp, ParamFilter::Part));
        parts.push(part);
    }
    root.set("parts", parts);
    return root.dump(2) + "\n";
}

bool multiFromJson(const std::string& text, MultiPreset& out, std::string& error) {
    Json root;
    if (!Json::parse(text, root, error)) { error = "JSON parse error: " + error; return false; }
    if (!root.isObject()) { error = "multi root must be an object"; return false; }
    MultiPreset m;
    if (auto f = root.find("format"); !f || !f->isString() || f->string() != "hsynth-multi") {
        error = "not an hsynth-multi file";
        return false;
    }
    if (auto v = root.find("version"); v && v->number() > 1)
        m.warnings.push_back("multi version " + std::to_string((int)v->number()) + " is newer than this engine");
    if (auto n = root.find("name"); n && n->isString()) m.name = n->string();
    if (auto n = root.find("comment"); n && n->isString()) m.comment = n->string();
    if (auto n = root.find("selected"); n && n->isNumber())
        m.selectedPart = std::max(0, std::min(kNumParts - 1, (int)n->number() - 1));
    if (auto g = root.find("global"); g && g->isObject()) {
        Json gp = Json::object();
        if (auto params = g->find("params")) gp.set("params", *params);
        else gp.set("params", Json::object());
        if (auto irs = g->find("impulses")) gp.set("impulses", *irs);
        if (!presetFromValue(gp, m.global, error)) { error = "global: " + error; return false; }
        for (auto& w : m.global.warnings) m.warnings.push_back("global: " + w);
        m.global.warnings.clear();
    }
    const Json* parts = root.find("parts");
    if (!parts || !parts->isArray()) { error = "multi has no 'parts' array"; return false; }
    if ((int)parts->size() > kNumParts) m.warnings.push_back("more than 16 parts, extra parts ignored");
    const auto& mixTable = partMixTable();
    for (int k = 0; k < kNumParts && k < (int)parts->size(); ++k) {
        const Json& part = parts->at((size_t)k);
        const std::string tag = "part " + std::to_string(k + 1) + ": ";
        if (!part.isObject()) { m.warnings.push_back(tag + "not an object"); continue; }
        if (auto mix = part.find("mix"); mix && mix->isObject()) {
            for (const auto& kv : mix->members()) {
                int id = -1;
                for (int i = 0; i < Mix::Count; ++i)
                    if (mixTable[(size_t)i].name == kv.first) id = i;
                if (id < 0 || !(kv.second.isNumber() || kv.second.isBool())) {
                    m.warnings.push_back(tag + "invalid mix setting '" + kv.first + "'");
                    continue;
                }
                m.mix[k][id] = sanitizePartMix(id, (float)kv.second.number());
            }
        }
        if (auto pr = part.find("preset"); pr && pr->isObject()) {
            std::string err;
            if (!presetFromValue(*pr, m.parts[k], err)) { m.warnings.push_back(tag + err); continue; }
            for (auto& w : m.parts[k].warnings) m.warnings.push_back(tag + w);
            m.parts[k].warnings.clear();
        }
    }
    out = std::move(m);
    return true;
}

namespace {
bool readFile(const std::string& path, std::string& text, std::string& error) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}
bool writeFile(const std::string& path, const std::string& text, std::string& error) {
    std::ofstream f(utf8Path(path), std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot write " + path; return false; }
    f << text;
    if (!f) { error = "write failed " + path; return false; }
    return true;
}
}  // namespace

bool loadMultiFile(const std::string& path, MultiPreset& out, std::string& error) {
    std::string text;
    if (!readFile(path, text, error)) return false;
    if (!multiFromJson(text, out, error)) { error = path + ": " + error; return false; }
    return true;
}

bool saveMultiFile(const std::string& path, const MultiPreset& m, std::string& error) {
    return writeFile(path, multiToJson(m), error);
}

bool loadPresetFile(const std::string& path, Preset& out, std::string& error) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!presetFromJson(ss.str(), out, error)) { error = path + ": " + error; return false; }
    return true;
}

bool savePresetFile(const std::string& path, const Preset& p, std::string& error) {
    std::ofstream f(utf8Path(path), std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot write " + path; return false; }
    f << presetToJson(p);
    if (!f) { error = "write failed " + path; return false; }
    return true;
}

std::string paramMetadataJson() {
    Json arr = Json::array();
    const auto& table = paramTable();
    for (int i = 0; i < (int)table.size(); ++i) {
        const ParamInfo& info = table[(size_t)i];
        Json o = Json::object();
        o.set("id", i);
        o.set("name", info.name);
        o.set("group", info.group);
        o.set("label", info.label);
        const char* kind = info.kind == ParamKind::Float ? "float" : info.kind == ParamKind::Int ? "int"
                           : info.kind == ParamKind::Enum ? "enum" : "bool";
        o.set("kind", kind);
        o.set("curve", info.curve == ParamCurve::Exp ? "exp" : "linear");
        o.set("min", (double)info.min);
        o.set("max", (double)info.max);
        o.set("def", (double)info.def);
        if (!info.unit.empty()) o.set("unit", info.unit);
        if (!info.options.empty()) {
            Json opts = Json::array();
            for (auto& s : info.options) opts.push(s);
            o.set("options", opts);
        }
        arr.push(o);
    }
    return arr.dump(0);
}

}  // namespace hsynth
