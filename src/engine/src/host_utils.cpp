#include "hsynth/host_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "hsynth/fileio.h"
#include "hsynth/fx/ir_load.h"
#include "hsynth/tuning.h"
#include "hsynth/wav.h"

namespace hsynth {

namespace {
bool isAbsolute(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
}
std::string joinPath(const std::string& root, const std::string& rel) {
    if (root.empty() || isAbsolute(rel)) return rel;
    const char last = root.back();
    return (last == '/' || last == '\\') ? root + rel : root + "/" + rel;
}
}  // namespace

bool loadPartSamples(Engine& engine, int part, int layer, const std::string& path, std::string& error,
                     std::vector<std::string>& warnings) {
    auto set = loadSampleSet(path, error);
    if (!set) return false;
    for (auto& w : set->warnings) warnings.push_back(w);
    engine.setPartSampleSet(part, layer, std::move(set));
    engine.collectGarbage();
    return true;
}

bool loadPartWavetable(Engine& engine, int part, int layer, const std::string& path, std::string& error) {
    auto wt = loadWavetableFile(path, error);
    if (!wt) return false;
    auto set = std::make_unique<SampleSet>();
    set->name = wt->name;
    set->sourcePath = path;
    set->wavetable = std::move(wt);
    engine.setPartSampleSet(part, layer, std::move(set));
    engine.collectGarbage();
    return true;
}

bool loadLayerSamples(Engine& engine, int layer, const std::string& path, std::string& error,
                      std::vector<std::string>& warnings) {
    return loadPartSamples(engine, 0, layer, path, error, warnings);
}

bool loadLayerWavetable(Engine& engine, int layer, const std::string& path, std::string& error) {
    return loadPartWavetable(engine, 0, layer, path, error);
}

namespace {
void applySamples(Engine& engine, int part, const Preset& preset, const std::string& libraryRoot,
                  std::vector<std::string>& warnings) {
    const std::string tag = part == 0 ? std::string("layer ") : "part " + std::to_string(part + 1) + " layer ";
    for (int l = 0; l < kNumLayers; ++l) {
        const auto type = (LayerType)(int)preset.params[(size_t)P::layer(l, P::LType)];
        const std::string& rel = preset.samples[l];
        if (rel.empty()) {
            if (type == LayerType::PCM) warnings.push_back(tag + (char)('A' + l) + " is PCM but has no samples");
            engine.setPartSampleSet(part, l, nullptr);
            continue;
        }
        const std::string full = joinPath(libraryRoot, rel);
        if (engine.partSampleSetPath(part, l) == full) continue;  // already loaded
        std::string err;
        const bool ok = type == LayerType::WT ? loadPartWavetable(engine, part, l, full, err)
                                              : loadPartSamples(engine, part, l, full, err, warnings);
        if (!ok) {
            warnings.push_back(tag + (char)('A' + l) + ": " + err);
            engine.setPartSampleSet(part, l, nullptr);
        }
    }
}

void applyImpulses(Engine& engine, const std::string* impulses, const std::string& libraryRoot,
                   std::vector<std::string>& warnings) {
    for (int t = 0; t < 2; ++t) {
        const std::string& rel = impulses[t];
        if (rel.empty()) {
            engine.setFxImpulse(t, nullptr);
            continue;
        }
        const std::string full = joinPath(libraryRoot, rel);
        if (engine.fxImpulsePath(t) == full) continue;  // already loaded
        std::string err;
        auto ir = fx::loadImpulseResponse(full, engine.sampleRate(), t, err);
        if (!ir) warnings.push_back(std::string(t == 0 ? "cabinet IR: " : "reverb IR: ") + err);
        engine.setFxImpulse(t, std::move(ir));
    }
}

struct Relativizer {
    std::string root;
    explicit Relativizer(const std::string& libraryRoot) : root(libraryRoot) {
        std::replace(root.begin(), root.end(), '\\', '/');
        if (!root.empty() && root.back() != '/') root += '/';
    }
    std::string operator()(std::string path) const {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (!root.empty() && path.rfind(root, 0) == 0) path = path.substr(root.size());
        return path;
    }
};
}  // namespace

bool applyPreset(Engine& engine, const Preset& preset, const std::string& libraryRoot,
                 std::vector<std::string>& warnings) {
    for (auto& w : preset.warnings) warnings.push_back(w);
    applySamples(engine, 0, preset, libraryRoot, warnings);
    applyImpulses(engine, preset.impulses, libraryRoot, warnings);
    engine.setAllParams(preset.params.data());
    engine.collectGarbage();
    return true;
}

bool applyPartPreset(Engine& engine, int part, const Preset& preset, const std::string& libraryRoot,
                     std::vector<std::string>& warnings) {
    if (part < 0 || part >= kNumParts || (int)preset.params.size() != P::kNumParams) return false;
    for (auto& w : preset.warnings) warnings.push_back(w);
    applySamples(engine, part, preset, libraryRoot, warnings);
    engine.setPartParams(part, preset.params.data());
    engine.collectGarbage();
    return true;
}

void sampleBounds(const std::vector<float>& l, const std::vector<float>& r, float thresholdDb, int64_t& start, int64_t& end) {
    const int64_t n = (int64_t)l.size();
    auto amp = [&](int64_t i) { return std::max(std::fabs(l[(size_t)i]), r.size() == l.size() ? std::fabs(r[(size_t)i]) : 0.0f); };
    float peak = 0.0f;
    for (int64_t i = 0; i < n; ++i) peak = std::max(peak, amp(i));
    start = 0;
    end = n;
    if (peak <= 0.0f) return;
    const float th = peak * std::pow(10.0f, thresholdDb / 20.0f);
    while (start < n && amp(start) < th) ++start;
    while (end > start && amp(end - 1) < th) --end;
    // a little before the attack, a little after the tail
    const int64_t pre = 64;
    start = std::max<int64_t>(0, start - pre);
    end = std::min<int64_t>(n, end + pre);
}

float normalizeGain(const std::vector<float>& l, const std::vector<float>& r, int64_t start, int64_t end, float targetDb) {
    float peak = 0.0f;
    for (int64_t i = std::max<int64_t>(0, start); i < std::min<int64_t>((int64_t)l.size(), end); ++i) {
        peak = std::max(peak, std::fabs(l[(size_t)i]));
        if (r.size() == l.size()) peak = std::max(peak, std::fabs(r[(size_t)i]));
    }
    return peak > 1e-6f ? std::pow(10.0f, targetDb / 20.0f) / peak : 1.0f;
}

bool findLoop(const std::vector<float>& l, const std::vector<float>& r, int64_t start, int64_t end, int64_t& loopStart,
              int64_t& loopEnd) {
    const bool st = r.size() == l.size();
    auto x = [&](int64_t i) { return st ? 0.5f * (l[(size_t)i] + r[(size_t)i]) : l[(size_t)i]; };
    const int64_t len = end - start;
    constexpr int64_t kWin = 256;
    if (len < 8 * kWin) return false;
    // loop end: a rising zero crossing near the end (before the last 10 %)
    int64_t e = end - len / 10;
    while (e > start + len / 2 && !(x(e - 1) < 0.0f && x(e) >= 0.0f)) --e;
    if (e <= start + len / 2) return false;
    // loop start: the rising zero crossing in the middle part whose preceding window
    // matches the window before the loop end best (normalised correlation)
    double best = -2.0;
    int64_t bestS = -1;
    double ee = 0.0;
    for (int64_t k = 0; k < kWin; ++k) ee += (double)x(e - 1 - k) * x(e - 1 - k);
    for (int64_t s = start + len / 4; s < e - len / 5; ++s) {
        if (!(x(s - 1) < 0.0f && x(s) >= 0.0f)) continue;
        double c = 0.0, ss = 0.0;
        for (int64_t k = 0; k < kWin; ++k) {
            const double a = x(s - 1 - k), b = x(e - 1 - k);
            c += a * b;
            ss += a * a;
        }
        const double v = (ss > 0.0 && ee > 0.0) ? c / std::sqrt(ss * ee) : -1.0;
        if (v > best) {
            best = v;
            bestS = s;
        }
    }
    if (bestS < 0 || best < 0.7) return false;
    loopStart = bestS;
    loopEnd = e;
    return true;
}

bool saveSampledInstrument(const std::string& dir, const std::string& name, const std::vector<float>& l,
                           const std::vector<float>& r, int sampleRate, int64_t start, int64_t end, float gain,
                           int64_t loopStart, int64_t loopEnd, int rootKey, std::string& sfzPath, std::string& error) {
    start = std::max<int64_t>(0, start);
    end = std::min<int64_t>((int64_t)l.size(), end);
    if (end - start < 16) { error = "nothing to save"; return false; }
    const bool st = r.size() == l.size();
    const int64_t n = end - start;
    std::vector<float> ol((size_t)n), orr;
    if (st) orr.resize((size_t)n);
    const int64_t fade = std::min<int64_t>(n / 4, sampleRate / 200);  // 5 ms
    const bool loop = loopStart >= start && loopEnd > loopStart && loopEnd <= end;
    for (int64_t i = 0; i < n; ++i) {
        float g = gain;
        if (i < fade) g *= (float)i / (float)fade;
        if (!loop && i >= n - fade) g *= (float)(n - 1 - i) / (float)fade;  // a looped sample keeps its tail
        ol[(size_t)i] = std::max(-1.0f, std::min(1.0f, l[(size_t)(start + i)] * g));
        if (st) orr[(size_t)i] = std::max(-1.0f, std::min(1.0f, r[(size_t)(start + i)] * g));
    }
    const std::string folder = dir + "/" + name;
    std::error_code ec;
    std::filesystem::create_directories(utf8Path(folder), ec);
    const std::string wav = folder + "/" + name + ".wav";
    if (!writeWav(wav, ol, st ? &orr : nullptr, sampleRate, false, error, rootKey, loop ? loopStart - start : -1,
                  loop ? loopEnd - start - 1 : -1))
        return false;
    sfzPath = folder + "/" + name + ".sfz";
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(sfzPath, "wb"), &std::fclose);
    if (!f) { error = "cannot write " + sfzPath; return false; }
    std::string sfz = "// " + name + " - sampled with HSynth\n<region> sample=" + name + ".wav pitch_keycenter=" +
                      std::to_string(rootKey) + " lokey=0 hikey=127";
    if (loop)
        sfz += " loop_mode=loop_continuous loop_start=" + std::to_string(loopStart - start) + " loop_end=" +
               std::to_string(loopEnd - start - 1);
    else
        sfz += " loop_mode=no_loop";
    sfz += "\n";
    if (std::fwrite(sfz.data(), 1, sfz.size(), f.get()) != sfz.size()) { error = "cannot write " + sfzPath; return false; }
    return true;
}

void quantizePhrase(std::vector<Engine::PhraseEvent>& events, double lengthBeats, double grid, double strength) {
    if (grid <= 0.0 || lengthBeats <= 0.0) return;
    strength = std::max(0.0, std::min(1.0, strength));
    auto wrap = [lengthBeats](double b) {
        b = std::fmod(b, lengthBeats);
        return b < 0.0 ? b + lengthBeats : b;
    };
    const size_t n = events.size();
    std::vector<bool> moved(n, false);
    for (size_t i = 0; i < n; ++i) {
        Event& on = events[i].event;
        if (on.type != EventType::NoteOn || on.value <= 0.0f || moved[i]) continue;
        const double b0 = events[i].beat;
        const double delta = (std::round(b0 / grid) * grid - b0) * strength;
        // its note-off: the next one of the same note (wrapping round the loop)
        size_t off = n;
        for (size_t k = 1; k < n && off == n; ++k) {
            const size_t j = (i + k) % n;
            const Event& x = events[j].event;
            const bool isOff = x.type == EventType::NoteOff || (x.type == EventType::NoteOn && x.value <= 0.0f);
            if (isOff && !moved[j] && x.key == on.key && x.channel == on.channel && (on.noteId == 0 || x.noteId == on.noteId)) off = j;
        }
        events[i].beat = wrap(b0 + delta);
        moved[i] = true;
        if (off < n) {
            events[off].beat = wrap(events[off].beat + delta);
            moved[off] = true;
        }
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const Engine::PhraseEvent& a, const Engine::PhraseEvent& b) { return a.beat < b.beat; });
}

namespace {
void vlq(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = v & 0x7F;
    while (v >>= 7) buf[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n) out.push_back(buf[--n]);
}
uint8_t to7(float v) { return (uint8_t)std::max(0, std::min(127, (int)std::lround(v * 127.0f))); }
}  // namespace

bool writePhraseSmf(const std::string& path, const std::vector<Engine::PhraseEvent>& events, double lengthBeats, double bpm,
                    int repeats, std::string& error) {
    constexpr int kPpq = 480;
    if (lengthBeats <= 0.0) { error = "the phrase is empty"; return false; }
    repeats = std::max(1, repeats);
    struct Msg { int64_t tick; int order; uint8_t b[3]; int len; };
    std::vector<Msg> msgs;
    int order = 0;
    for (int r = 0; r < repeats; ++r)
        for (const auto& pe : events) {
            const Event& e = pe.event;
            const uint8_t ch = e.channel == kOmniChannel ? 0 : (uint8_t)(e.channel & 15);
            Msg m{(int64_t)std::llround((pe.beat + r * lengthBeats) * kPpq), order++, {0, 0, 0}, 3};
            switch (e.type) {
                case EventType::NoteOn:
                    if (e.value > 0.0f) { m.b[0] = (uint8_t)(0x90 | ch); m.b[2] = std::max<uint8_t>(1, to7(e.value)); }
                    else { m.b[0] = (uint8_t)(0x80 | ch); m.b[2] = 64; }
                    m.b[1] = e.key;
                    break;
                case EventType::NoteOff: m.b[0] = (uint8_t)(0x80 | ch); m.b[1] = e.key; m.b[2] = to7(e.value); break;
                case EventType::Controller: m.b[0] = (uint8_t)(0xB0 | ch); m.b[1] = (uint8_t)(e.controller & 127); m.b[2] = to7(e.value); break;
                case EventType::ModWheel: m.b[0] = (uint8_t)(0xB0 | ch); m.b[1] = 1; m.b[2] = to7(e.value); break;
                case EventType::Sustain: m.b[0] = (uint8_t)(0xB0 | ch); m.b[1] = 64; m.b[2] = to7(e.value); break;
                case EventType::ChannelPressure: m.b[0] = (uint8_t)(0xD0 | ch); m.b[1] = to7(e.value); m.len = 2; break;
                case EventType::PitchBend: {
                    const int v = std::max(0, std::min(16383, (int)std::lround(e.value * 8192.0f) + 8192));
                    m.b[0] = (uint8_t)(0xE0 | ch); m.b[1] = (uint8_t)(v & 127); m.b[2] = (uint8_t)(v >> 7);
                    break;
                }
                default: continue;
            }
            msgs.push_back(m);
        }
    std::stable_sort(msgs.begin(), msgs.end(), [](const Msg& a, const Msg& b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });
    std::vector<uint8_t> trk;
    const uint32_t us = (uint32_t)std::lround(60e6 / std::max(20.0, std::min(300.0, bpm)));
    trk.insert(trk.end(), {0x00, 0xFF, 0x51, 0x03, (uint8_t)(us >> 16), (uint8_t)(us >> 8), (uint8_t)us});
    int64_t t = 0;
    for (const Msg& m : msgs) {
        vlq(trk, (uint32_t)std::max<int64_t>(0, m.tick - t));
        t = std::max(t, m.tick);
        trk.insert(trk.end(), m.b, m.b + m.len);
    }
    const int64_t end = (int64_t)std::llround(lengthBeats * repeats * kPpq);
    vlq(trk, (uint32_t)std::max<int64_t>(0, end - t));
    trk.insert(trk.end(), {0xFF, 0x2F, 0x00});
    std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, (uint8_t)(kPpq >> 8), (uint8_t)(kPpq & 0xFF),
                              'M', 'T', 'r', 'k'};
    const uint32_t len = (uint32_t)trk.size();
    f.insert(f.end(), {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len});
    f.insert(f.end(), trk.begin(), trk.end());
    std::unique_ptr<FILE, int (*)(FILE*)> out(fopenUtf8(path, "wb"), &std::fclose);
    if (!out || std::fwrite(f.data(), 1, f.size(), out.get()) != f.size()) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

std::unique_ptr<Engine::StagedProgram> loadStagedProgram(const Preset& preset, const std::string& libraryRoot,
                                                         std::vector<std::string>& warnings) {
    if ((int)preset.params.size() != P::kNumParams) return nullptr;
    auto st = std::make_unique<Engine::StagedProgram>();
    std::copy(preset.params.begin(), preset.params.end(), st->params);
    for (auto& w : preset.warnings) warnings.push_back(w);
    for (int l = 0; l < kNumLayers; ++l) {
        const auto type = (LayerType)(int)preset.params[(size_t)P::layer(l, P::LType)];
        const std::string& rel = preset.samples[l];
        std::unique_ptr<SampleSet> set;
        if (!rel.empty()) {
            const std::string full = joinPath(libraryRoot, rel);
            std::string err;
            if (type == LayerType::WT) {
                if (auto wt = loadWavetableFile(full, err)) {
                    set = std::make_unique<SampleSet>();
                    set->name = wt->name;
                    set->sourcePath = full;
                    set->wavetable = std::move(wt);
                }
            } else {
                set = loadSampleSet(full, err);
                if (set) for (auto& w : set->warnings) warnings.push_back(w);
            }
            if (!set) warnings.push_back(std::string("layer ") + (char)('A' + l) + ": " + err);
        }
        if (!set) set = std::make_unique<SampleSet>();  // empty: unloads the layer
        st->setNames[l] = set->name;
        st->setPaths[l] = set->sourcePath;
        st->sets[l] = set.release();
    }
    return st;
}

Preset capturePreset(const Engine& engine, const std::string& name, const std::string& libraryRoot) {
    return capturePartPreset(engine, 0, name, libraryRoot);
}

Preset capturePartPreset(const Engine& engine, int part, const std::string& name, const std::string& libraryRoot) {
    Preset p;
    p.name = name;
    engine.getPartParams(part, p.params.data());
    const Relativizer relative(libraryRoot);
    for (int l = 0; l < kNumLayers; ++l) p.samples[l] = relative(engine.partSampleSetPath(part, l));
    for (int t = 0; t < 2; ++t) p.impulses[t] = relative(engine.fxImpulsePath(t));
    return p;
}

bool applyMulti(Engine& engine, const MultiPreset& multi, const std::string& libraryRoot,
                std::vector<std::string>& warnings) {
    for (auto& w : multi.warnings) warnings.push_back(w);
    engine.setMultiTimbral(true);
    applyImpulses(engine, multi.global.impulses, libraryRoot, warnings);
    engine.setGlobalParams(multi.global.params.data());
    for (int k = 0; k < kNumParts; ++k) {
        applyPartPreset(engine, k, multi.parts[k], libraryRoot, warnings);
        for (int i = 0; i < Mix::Count; ++i) engine.setPartMix(k, i, multi.mix[k][i]);
    }
    engine.selectPart(multi.selectedPart);
    engine.collectGarbage();
    return true;
}

MultiPreset captureMulti(const Engine& engine, const std::string& name, const std::string& libraryRoot) {
    MultiPreset m;
    m.name = name;
    m.selectedPart = engine.selectedPart();
    engine.getAllParams(m.global.params.data());
    const Relativizer relative(libraryRoot);
    for (int t = 0; t < 2; ++t) m.global.impulses[t] = relative(engine.fxImpulsePath(t));
    for (int k = 0; k < kNumParts; ++k) {
        m.parts[k] = capturePartPreset(engine, k, "Part " + std::to_string(k + 1), libraryRoot);
        m.parts[k].impulses[0].clear();
        m.parts[k].impulses[1].clear();
        for (int i = 0; i < Mix::Count; ++i) m.mix[k][i] = engine.getPartMix(k, i);
    }
    return m;
}

bool applyScalaTuning(Engine& engine, int part, const std::string& sclPath, int root, std::string& error) {
    ScalaScale s;
    if (!loadScalaFile(sclPath, s, error)) return false;
    float off[12];
    std::string warn;
    if (!scalaToOffsets(s, root, off, warn)) { error = warn; return false; }
    for (int i = 0; i < 12; ++i) engine.setPartParam(part, P::TuneC + i, off[i]);
    error = warn;  // non-fatal note (clamped degrees), empty if none
    return true;
}

void addNote(RenderJob& job, double time, double duration, int key, float velocity, uint32_t id) {
    if (id == 0) id = (uint32_t)(job.events.size() + 1000);
    job.events.push_back({time, Event::noteOn(id, key, velocity)});
    job.events.push_back({time + duration, Event::noteOff(id, key)});
    std::stable_sort(job.events.begin(), job.events.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
}

void renderOffline(Engine& engine, const RenderJob& job, std::vector<float>& left, std::vector<float>& right) {
    const int64_t total = (int64_t)(job.seconds * job.sampleRate);
    left.assign((size_t)total, 0.0f);
    right.assign((size_t)total, 0.0f);
    size_t next = 0;
    engine.setOfflineRender(true);
    for (int64_t pos = 0; pos < total; pos += job.blockSize) {
        const double t = (double)(pos + job.blockSize) / job.sampleRate;
        while (next < job.events.size() && job.events[next].time < t) engine.sendEvent(job.events[next++].event);
        const int n = (int)std::min<int64_t>(job.blockSize, total - pos);
        engine.process(left.data() + pos, right.data() + pos, n);
        engine.collectGarbage();
    }
    engine.setOfflineRender(false);
}

}  // namespace hsynth
