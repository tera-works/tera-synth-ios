#include "hsynth/setup.h"

#include "hsynth/host_utils.h"
#include "hsynth/midi_learn.h"
#include "hsynth/preset.h"

namespace hsynth {

std::string makeSetupJson(const Engine& engine, const std::string& name, const std::string& libraryRoot,
                          const MidiLearn* learn, const Json& host, const std::string& comment,
                          const std::vector<std::string>& partNames) {
    Json root = Json::object();
    root.set("format", "hsynth-setup");
    root.set("version", 1);
    root.set("name", name);
    if (!comment.empty()) root.set("comment", comment);
    std::string state;
    if (engine.multiTimbral()) {
        MultiPreset m = captureMulti(engine, name, libraryRoot);
        for (int k = 0; k < kNumParts && k < (int)partNames.size(); ++k)
            if (!partNames[(size_t)k].empty()) m.parts[k].name = partNames[(size_t)k];
        state = multiToJson(m);
    } else {
        state = presetToJson(capturePreset(engine, partNames.empty() || partNames[0].empty() ? name : partNames[0], libraryRoot));
    }
    Json s;
    std::string err;
    if (Json::parse(state, s, err)) root.set("state", s);
    if (learn) {
        Json m;
        if (Json::parse(learn->toJson(), m, err)) root.set("midiLearn", m);
    }
    if (host.isObject() && !host.members().empty()) root.set("host", host);
    return root.dump(2);
}

bool applySetupJson(Engine& engine, const std::string& text, const std::string& libraryRoot, MidiLearn* learn,
                    SetupResult& out, std::string& error) {
    Json root;
    if (!Json::parse(text, root, error)) return false;
    if (!root.isObject()) { error = "not a setup file"; return false; }
    const Json* fmt = root.find("format");
    if (!fmt || !fmt->isString() || fmt->string() != "hsynth-setup") {
        error = "not an hsynth-setup file";
        return false;
    }
    if (const Json* n = root.find("name"); n && n->isString()) out.name = n->string();
    const Json* state = root.find("state");
    const Json* map = root.find("midiLearn");
    if (!(state && state->isObject()) && !(map && map->isObject())) {
        error = "the setup has neither a state nor MIDI learn assignments";
        return false;
    }
    if (state && state->isObject()) {
        const Json* sf = state->find("format");
        const std::string sformat = sf && sf->isString() ? sf->string() : "";
        if (sformat == "hsynth-multi") {
            MultiPreset m;
            if (!multiFromJson(state->dump(0), m, error)) { error = "state: " + error; return false; }
            out.warnings.insert(out.warnings.end(), m.warnings.begin(), m.warnings.end());
            if (!applyMulti(engine, m, libraryRoot, out.warnings)) { error = "could not apply the multi"; return false; }
            engine.setMultiTimbral(true);
            out.multi = true;
            for (int k = 0; k < kNumParts; ++k) out.partNames.push_back(m.parts[k].name);
        } else {
            Preset p;
            if (!presetFromJson(state->dump(0), p, error)) { error = "state: " + error; return false; }
            out.warnings.insert(out.warnings.end(), p.warnings.begin(), p.warnings.end());
            engine.setMultiTimbral(false);
            if (!applyPreset(engine, p, libraryRoot, out.warnings)) { error = "could not apply the preset"; return false; }
            out.multi = false;
            out.partNames.push_back(p.name);
        }
        out.hasState = true;
    }
    if (learn && map && map->isObject()) {
        std::string e;
        if (learn->fromJson(map->dump(0), e)) {
            out.hasMidiLearn = true;
            learn->syncStandardOverrides(engine);
        }
        else out.warnings.push_back("MIDI learn: " + e);
    }
    if (const Json* h = root.find("host"); h && h->isObject()) out.host = *h;
    return true;
}

}  // namespace hsynth
