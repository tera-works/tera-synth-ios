// SFZ loader (subset). See supportedSfzOpcodes() for what is honoured;
// everything else produces a warning instead of failing the load.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include "hsynth/sample_set.h"
#include "hsynth/fileio.h"

namespace hsynth {

std::unique_ptr<Sample> sampleFromWav(const std::string& path, std::string& error, bool stream);

namespace {

using OpcodeMap = std::map<std::string, std::string>;

const std::set<std::string>& supportedSet() {
    static const std::set<std::string> s = {
        "sample", "lokey", "hikey", "key", "lovel", "hivel", "lorand", "hirand",
        "pitch_keycenter", "pitch_keytrack", "tune", "pitch", "transpose", "volume", "pan",
        "loop_mode", "loopmode", "loop_start", "loopstart", "loop_end", "loopend", "offset", "end",
        "ampeg_attack", "ampeg_decay", "ampeg_sustain", "ampeg_release", "amp_veltrack",
        "trigger", "default_path", "note_offset", "octave_offset",
        "delay", "ampeg_delay", "ampeg_hold", "rt_decay", "group", "off_by", "off_mode", "seq_length", "seq_position",
        "loop_crossfade", "fil_type", "filtype", "cutoff", "resonance", "fil_keytrack", "fil_keycenter", "fil_veltrack",
        "amp_keytrack", "amp_keycenter", "sw_lokey", "sw_hikey", "sw_last", "sw_default",
        "xfin_lovel", "xfin_hivel", "xfout_lovel", "xfout_hivel", "xfin_lokey", "xfin_hikey", "xfout_lokey", "xfout_hikey",
        "xf_velcurve", "xf_keycurve"};
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string dirName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string stripComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') ++i;
            out.push_back('\n');
        } else if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            ++i;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Parse note number: integer or note name (c4 = 60).
bool parseNote(const std::string& v, int& out) {
    if (v.empty()) return false;
    if (std::isdigit((unsigned char)v[0]) || v[0] == '-') {
        out = std::atoi(v.c_str());
        return true;
    }
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    const char c = (char)std::tolower((unsigned char)v[0]);
    if (c < 'a' || c > 'g') return false;
    int n = base[c - 'a'];
    size_t i = 1;
    if (i < v.size() && (v[i] == '#' || v[i] == 's')) { ++n; ++i; }
    else if (i < v.size() && v[i] == 'b') { --n; ++i; }
    if (i >= v.size()) return false;
    const int octave = std::atoi(v.c_str() + i);
    out = (octave + 1) * 12 + n;
    return true;
}

struct Parser {
    std::string rootDir;
    std::string defaultPath;
    int noteOffset = 0;
    int octaveOffset = 0;
    std::unordered_map<std::string, std::string> defines;
    std::map<std::string, int> unsupported;
    std::vector<std::string> warnings;
    OpcodeMap global, master, group, control;
    std::vector<OpcodeMap> regions;
    int includeDepth = 0;

    enum class Scope { None, Control, Global, Master, Group, Region, Other } scope = Scope::None;

    std::string substitute(const std::string& s) {
        if (defines.empty() || s.find('$') == std::string::npos) return s;
        std::string r = s;
        // longest names first to avoid prefix clashes
        std::vector<std::pair<std::string, std::string>> d(defines.begin(), defines.end());
        std::sort(d.begin(), d.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
        for (auto& kv : d) {
            size_t p;
            while ((p = r.find(kv.first)) != std::string::npos) r.replace(p, kv.first.size(), kv.second);
        }
        return r;
    }

    void beginHeader(const std::string& h) {
        if (h == "region") {
            scope = Scope::Region;
            OpcodeMap m = global;
            for (auto& kv : master) m[kv.first] = kv.second;
            for (auto& kv : group) m[kv.first] = kv.second;
            regions.push_back(m);
        } else if (h == "group") {
            scope = Scope::Group;
            group.clear();
        } else if (h == "master") {
            scope = Scope::Master;
            master.clear();
            group.clear();
        } else if (h == "global") {
            scope = Scope::Global;
            global.clear();
            master.clear();
            group.clear();
        } else if (h == "control") {
            scope = Scope::Control;
        } else {
            scope = Scope::Other;
            warnings.push_back("header <" + h + "> is not supported; its opcodes are ignored");
        }
    }

    void opcode(const std::string& key, const std::string& value) {
        switch (scope) {
            case Scope::Control:
                if (key == "default_path") defaultPath = value;
                else if (key == "note_offset") noteOffset = std::atoi(value.c_str());
                else if (key == "octave_offset") octaveOffset = std::atoi(value.c_str());
                else unsupported[key]++;
                break;
            case Scope::Global: global[key] = value; break;
            case Scope::Master: master[key] = value; break;
            case Scope::Group: group[key] = value; break;
            case Scope::Region: regions.back()[key] = value; break;
            case Scope::None:
                warnings.push_back("opcode '" + key + "' outside of any header ignored");
                break;
            case Scope::Other: break;
        }
    }

    bool parseText(const std::string& raw, const std::string& baseDir) {
        const std::string text = stripComments(raw);
        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            const char c = text[i];
            if (std::isspace((unsigned char)c)) { ++i; continue; }
            if (c == '<') {
                const size_t close = text.find('>', i);
                if (close == std::string::npos) { warnings.push_back("unterminated header"); return true; }
                beginHeader(trim(text.substr(i + 1, close - i - 1)));
                i = close + 1;
                continue;
            }
            if (c == '#') {
                size_t eol = text.find('\n', i);
                if (eol == std::string::npos) eol = n;
                const std::string line = trim(text.substr(i, eol - i));
                i = eol;
                if (line.rfind("#define", 0) == 0) {
                    std::istringstream ss(line.substr(7));
                    std::string name, val;
                    ss >> name;
                    std::getline(ss, val);
                    defines[name] = trim(val);
                } else if (line.rfind("#include", 0) == 0) {
                    const size_t q1 = line.find('"');
                    const size_t q2 = line.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos && includeDepth < 8) {
                        std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
                        std::replace(inc.begin(), inc.end(), '\\', '/');
                        std::string content;
                        if (readTextFile(baseDir + inc, content)) {
                            ++includeDepth;
                            parseText(content, dirName(baseDir + inc));
                            --includeDepth;
                        } else {
                            warnings.push_back("#include not found: " + inc);
                        }
                    }
                } else {
                    warnings.push_back("unsupported directive: " + line);
                }
                continue;
            }
            // opcode: name=value
            const size_t eq = text.find('=', i);
            if (eq == std::string::npos) break;
            const std::string key = trim(text.substr(i, eq - i));
            if (key.empty() || key.find_first_of(" \t\r\n<") != std::string::npos) {
                // stray token; skip it
                size_t sp = text.find_first_of(" \t\r\n", i);
                warnings.push_back("unexpected token near '" + text.substr(i, std::min<size_t>(20, n - i)) + "'");
                i = sp == std::string::npos ? n : sp;
                continue;
            }
            // value runs to end of line or next header, but stops before the
            // next "name=" so that sample paths may contain spaces.
            size_t end = eq + 1;
            while (end < n && text[end] != '\n' && text[end] != '\r' && text[end] != '<') ++end;
            std::string rest = text.substr(eq + 1, end - eq - 1);
            size_t cut = std::string::npos;
            for (size_t k = 0; k < rest.size(); ++k) {
                if (!std::isspace((unsigned char)rest[k])) continue;
                size_t j = k;
                while (j < rest.size() && std::isspace((unsigned char)rest[j])) ++j;
                size_t m = j;
                while (m < rest.size() && (std::isalnum((unsigned char)rest[m]) || rest[m] == '_' || rest[m] == '$')) ++m;
                if (m > j && m < rest.size() && rest[m] == '=') { cut = k; break; }
            }
            std::string value = trim(cut == std::string::npos ? rest : rest.substr(0, cut));
            i = cut == std::string::npos ? end : eq + 1 + cut;
            opcode(substitute(key), substitute(value));
        }
        return true;
    }
};

LoopMode parseLoopMode(const std::string& v, bool& ok) {
    ok = true;
    if (v == "no_loop") return LoopMode::NoLoop;
    if (v == "one_shot") return LoopMode::OneShot;
    if (v == "loop_continuous") return LoopMode::Continuous;
    if (v == "loop_sustain") return LoopMode::Sustain;
    ok = false;
    return LoopMode::NoLoop;
}

}  // namespace

const std::vector<std::string>& supportedSfzOpcodes() {
    static const std::vector<std::string> v(supportedSet().begin(), supportedSet().end());
    return v;
}

namespace {
// Sample decoding helpers run below normal priority, like the loader thread
// that starts them (Android: the audio callback must never wait for them).
void lowerThreadPriority() {
#if defined(__ANDROID__) || defined(__linux__)
    setpriority(PRIO_PROCESS, 0, 10);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

// Size of a sample file as 16-bit PCM: the file size for WAV, the decoded size
// from STREAMINFO for FLAC (a third of it on disk would never reach the
// streaming threshold although it needs as much memory as the WAV).
int64_t pcmBytes(const std::string& path) {
    std::error_code ec;
    const int64_t size = (int64_t)std::filesystem::file_size(utf8Path(path), ec);
    if (ec) return 0;
    if (path.size() < 5) return size;
    std::string ext = path.substr(path.size() - 5);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext != ".flac") return size;
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    uint8_t h[42];
    if (!f || std::fread(h, 1, sizeof h, f.get()) != sizeof h || std::memcmp(h, "fLaC", 4) != 0 || (h[4] & 0x7F) != 0)
        return size * 3;
    const uint8_t* si = h + 8;  // STREAMINFO body
    const int channels = ((si[12] >> 1) & 7) + 1;
    const uint64_t frames = ((uint64_t)(si[13] & 0x0F) << 32) | (uint64_t)si[14] << 24 | (uint64_t)si[15] << 16 |
                            (uint64_t)si[16] << 8 | si[17];
    return frames ? (int64_t)(frames * (uint64_t)channels * 2) : size * 3;
}

}  // namespace

std::unique_ptr<SampleSet> loadSfzSampleSet(const std::string& path, std::string& error) {
    std::string text;
    if (!readTextFile(path, text)) { error = "cannot open " + path; return nullptr; }
    Parser parser;
    parser.rootDir = dirName(path);
    parser.parseText(text, parser.rootDir);

    auto set = std::make_unique<SampleSet>();
    set->sourcePath = path;
    {
        const size_t slash = path.find_last_of("/\\");
        std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = b.find_last_of('.');
        set->name = dot == std::string::npos ? b : b.substr(0, dot);
    }
    std::unordered_map<std::string, Sample*> loaded;
    std::set<std::string> failedSamples;
    int skippedTrigger = 0, skippedSamples = 0;
    // disk streaming: decided by the total size of the sample files (streaming.h)
    bool stream = false;
    {
        const SampleLoadOptions opt = sampleLoadOptions();
        if (opt.mode == StreamMode::Always) {
            stream = true;
        } else if (opt.mode == StreamMode::Auto) {
            std::set<std::string> files;
            for (const OpcodeMap& op : parser.regions) {
                auto it = op.find("sample");
                if (it == op.end() || it->second.empty()) continue;
                std::string rel = parser.defaultPath + it->second;
                std::replace(rel.begin(), rel.end(), '\\', '/');
                if (rel[0] == '*') continue;
                files.insert((rel.size() > 1 && (rel[0] == '/' || rel[1] == ':')) ? rel : parser.rootDir + rel);
            }
            int64_t total = 0;
            for (const auto& f : files) total += pcmBytes(f);
            stream = total > opt.autoThresholdBytes;
        }
    }

    // decode every sample file first, several at a time (FLAC decoding is the
    // slow part of loading an instrument); the regions below pick them up
    std::map<std::string, std::pair<std::unique_ptr<Sample>, std::string>> decoded;
    {
        std::vector<std::string> paths;
        for (const OpcodeMap& op : parser.regions) {
            auto it = op.find("sample");
            if (it == op.end() || it->second.empty()) continue;
            std::string rel = parser.defaultPath + it->second;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (rel[0] == '*') continue;
            const std::string full = (rel.size() > 1 && (rel[0] == '/' || rel[1] == ':')) ? rel : parser.rootDir + rel;
            if (decoded.emplace(full, std::make_pair(std::unique_ptr<Sample>(), std::string())).second) paths.push_back(full);
        }
        std::vector<std::pair<std::unique_ptr<Sample>, std::string>*> slots;
        for (const auto& p : paths) slots.push_back(&decoded[p]);
        std::atomic<size_t> next{0};
        auto work = [&](bool helper) {
            if (helper) lowerThreadPriority();
            for (size_t i; (i = next.fetch_add(1)) < paths.size();) {
                std::string err;
                slots[i]->first = sampleFromWav(paths[i], err, stream);
                slots[i]->second = std::move(err);
            }
        };
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const size_t helpers = paths.size() < 8 ? 0 : std::min<size_t>({3, hw > 2 ? hw - 2 : 0, paths.size() / 8});
        std::vector<std::thread> pool;
        for (size_t h = 0; h < helpers; ++h) pool.emplace_back(work, true);
        work(false);
        for (auto& t : pool) t.join();
    }

    for (const OpcodeMap& op : parser.regions) {
        for (auto& kv : op)
            if (!supportedSet().count(kv.first)) parser.unsupported[kv.first]++;

        auto it = op.find("sample");
        if (it == op.end()) { parser.warnings.push_back("region without sample= skipped"); continue; }
        if (it->second.empty()) { parser.warnings.push_back("region with an empty sample= skipped"); continue; }
        RegionTrigger trigger = RegionTrigger::Attack;
        if (auto trig = op.find("trigger"); trig != op.end()) {
            if (trig->second == "release" || trig->second == "release_key") trigger = RegionTrigger::Release;
            else if (trig->second != "attack") { ++skippedTrigger; continue; }
        }

        std::string rel = parser.defaultPath + it->second;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!rel.empty() && rel[0] == '*') {
            parser.warnings.push_back("generator sample '" + rel + "' not supported");
            continue;
        }
        const std::string full = (rel.size() > 1 && (rel[0] == '/' || rel[1] == ':')) ? rel : parser.rootDir + rel;
        Sample* sample = nullptr;
        auto found = loaded.find(full);
        if (found != loaded.end()) {
            sample = found->second;
        } else if (!failedSamples.count(full)) {
            auto pre = decoded.find(full);
            std::string err = pre != decoded.end() ? pre->second.second : std::string();
            std::unique_ptr<Sample> s = pre != decoded.end() ? std::move(pre->second.first) : sampleFromWav(full, err, stream);
            if (!s) {
                failedSamples.insert(full);
                parser.warnings.push_back("sample load failed: " + err);
            } else {
                sample = s.get();
                loaded[full] = sample;
                set->samples.push_back(std::move(s));
            }
        }
        if (!sample) { ++skippedSamples; continue; }

        Region r;
        r.sample = sample;
        r.trigger = trigger;
        auto get = [&](const char* k) -> const std::string* {
            auto f = op.find(k);
            return f == op.end() ? nullptr : &f->second;
        };
        auto note = [&](const char* k, int& dst) {
            if (auto v = get(k)) {
                int n;
                if (parseNote(*v, n)) dst = n + parser.noteOffset + 12 * parser.octaveOffset;
                else parser.warnings.push_back(std::string("bad note value ") + k + "=" + *v);
            }
        };
        auto num = [&](const char* k, float& dst) { if (auto v = get(k)) dst = (float)std::atof(v->c_str()); };
        auto i64 = [&](const char* k, int64_t& dst) { if (auto v = get(k)) dst = std::atoll(v->c_str()); };

        r.keycenter = sample->unityNote >= 0 && sample->unityNote < 128 ? sample->unityNote : 60;
        if (get("key")) {
            int k = 60;
            note("key", k);
            r.lokey = r.hikey = r.keycenter = k;
        }
        note("lokey", r.lokey);
        note("hikey", r.hikey);
        note("pitch_keycenter", r.keycenter);
        if (auto v = get("lovel")) r.lovel = std::atoi(v->c_str());
        if (auto v = get("hivel")) r.hivel = std::atoi(v->c_str());
        num("lorand", r.lorand);
        num("hirand", r.hirand);
        num("pitch_keytrack", r.pitchKeytrack);
        num("tune", r.tune);
        if (auto v = get("pitch")) r.tune = (float)std::atof(v->c_str());
        if (auto v = get("transpose")) r.transpose = std::atoi(v->c_str());
        num("volume", r.volumeDb);
        float pan = 0;
        num("pan", pan);
        r.pan = std::max(-1.0f, std::min(1.0f, pan / 100.0f));
        num("amp_veltrack", r.ampVeltrack);
        i64("offset", r.offset);
        if (auto v = get("end")) {
            const int64_t e = std::atoll(v->c_str());
            r.end = e <= 0 ? -1 : e + 1;  // sfz end is inclusive
        }

        // loop: region opcodes override smpl chunk
        r.loopStart = sample->loopStart;
        r.loopEnd = sample->loopEnd;
        if (auto v = get("loop_start")) r.loopStart = std::atoll(v->c_str());
        if (auto v = get("loopstart")) r.loopStart = std::atoll(v->c_str());
        if (auto v = get("loop_end")) r.loopEnd = std::atoll(v->c_str()) + 1;
        if (auto v = get("loopend")) r.loopEnd = std::atoll(v->c_str()) + 1;
        const std::string* lm = get("loop_mode");
        if (!lm) lm = get("loopmode");
        if (lm) {
            bool ok;
            r.loopMode = parseLoopMode(*lm, ok);
            if (!ok) parser.warnings.push_back("unknown loop_mode " + *lm);
        } else {
            r.loopMode = r.loopStart >= 0 ? LoopMode::Continuous : LoopMode::NoLoop;
        }
        if (r.loopMode == LoopMode::Continuous || r.loopMode == LoopMode::Sustain) {
            if (r.loopStart < 0 || r.loopEnd <= r.loopStart + 1 || r.loopEnd > sample->frames) {
                if (r.loopStart < 0 && r.loopEnd < 0) {
                    r.loopStart = 0;
                    r.loopEnd = sample->frames;
                } else {
                    parser.warnings.push_back("invalid loop points in " + sample->path + "; looping disabled");
                    r.loopMode = LoopMode::NoLoop;
                }
            }
        }

        if (get("ampeg_attack") || get("ampeg_decay") || get("ampeg_sustain") || get("ampeg_release")) {
            r.ampeg.enabled = true;
            r.ampeg.sustain = 100.0f;
            num("ampeg_attack", r.ampeg.attack);
            num("ampeg_decay", r.ampeg.decay);
            num("ampeg_sustain", r.ampeg.sustain);
            num("ampeg_release", r.ampeg.release);
            r.ampeg.sustain = std::max(0.0f, std::min(1.0f, r.ampeg.sustain / 100.0f));
        }
        if (get("ampeg_delay") || get("ampeg_hold")) r.ampeg.enabled = true;
        num("ampeg_delay", r.ampeg.delay);
        num("ampeg_hold", r.ampeg.hold);
        num("delay", r.delay);
        num("rt_decay", r.rtDecay);
        if (auto v = get("group")) r.group = std::atoi(v->c_str());
        if (auto v = get("off_by")) r.offBy = std::atoi(v->c_str());
        if (auto v = get("off_mode")) r.offFast = *v != "normal";
        if (auto v = get("seq_length")) r.seqLength = std::max(1, std::atoi(v->c_str()));
        if (auto v = get("seq_position")) r.seqPosition = std::max(1, std::atoi(v->c_str()));
        num("loop_crossfade", r.loopCrossfade);
        const std::string* ft = get("fil_type");
        if (!ft) ft = get("filtype");
        if (get("cutoff")) {
            num("cutoff", r.cutoff);
            r.filter = RegionFilter::Lp2;
            if (ft) {
                static const std::pair<const char*, RegionFilter> kTypes[] = {
                    {"lpf_1p", RegionFilter::Lp1}, {"hpf_1p", RegionFilter::Hp1}, {"lpf_2p", RegionFilter::Lp2},
                    {"hpf_2p", RegionFilter::Hp2}, {"bpf_2p", RegionFilter::Bp2}, {"brf_2p", RegionFilter::Br2},
                    {"lpf_4p", RegionFilter::Lp2}, {"hpf_4p", RegionFilter::Hp2}};
                bool ok = false;
                for (auto& t : kTypes)
                    if (*ft == t.first) { r.filter = t.second; ok = true; }
                if (!ok) parser.warnings.push_back("fil_type " + *ft + " not supported, using lpf_2p");
                else if (ft->find("_4p") != std::string::npos) parser.warnings.push_back("fil_type " + *ft + " played as 2-pole");
            }
            num("resonance", r.resonance);
            num("fil_keytrack", r.filKeytrack);
            note("fil_keycenter", r.filKeycenter);
            num("fil_veltrack", r.filVeltrack);
            if (r.cutoff <= 0.0f) r.filter = RegionFilter::None;
        }
        num("amp_keytrack", r.ampKeytrack);
        if (auto v = get("xfin_lovel")) r.xfinLoVel = std::atoi(v->c_str());
        if (auto v = get("xfin_hivel")) r.xfinHiVel = std::atoi(v->c_str());
        if (auto v = get("xfout_lovel")) r.xfoutLoVel = std::atoi(v->c_str());
        if (auto v = get("xfout_hivel")) r.xfoutHiVel = std::atoi(v->c_str());
        note("xfin_lokey", r.xfinLoKey);
        note("xfin_hikey", r.xfinHiKey);
        note("xfout_lokey", r.xfoutLoKey);
        note("xfout_hikey", r.xfoutHiKey);
        if (auto v = get("xf_velcurve")) r.xfVelPower = *v != "gain";
        if (auto v = get("xf_keycurve")) r.xfKeyPower = *v != "gain";
        note("amp_keycenter", r.ampKeycenter);
        if (get("sw_last")) note("sw_last", r.swLast);
        {
            int lo = -1, hi = -1;
            if (get("sw_lokey")) note("sw_lokey", lo);
            if (get("sw_hikey")) note("sw_hikey", hi);
            if (lo >= 0 && hi >= lo) {
                set->swLo = set->swLo < 0 ? lo : std::min(set->swLo, lo);
                set->swHi = std::max(set->swHi, hi);
            }
            int def = -1;
            if (get("sw_default")) note("sw_default", def);
            if (def >= 0) set->swDefault = def;
        }
        if (trigger == RegionTrigger::Release) set->hasReleaseRegions = true;
        r.lokey = std::max(0, std::min(127, r.lokey));
        r.hikey = std::max(0, std::min(127, r.hikey));
        set->regions.push_back(r);
    }

    for (auto& kv : parser.unsupported)
        parser.warnings.push_back("unsupported opcode '" + kv.first + "' ignored (" + std::to_string(kv.second) + "x)");
    if (skippedTrigger) parser.warnings.push_back(std::to_string(skippedTrigger) + " first/legato trigger regions skipped (unsupported)");
    if (set->swDefault < 0) {  // no sw_default: start on the lowest switch used by any region
        for (const Region& r : set->regions)
            if (r.swLast >= 0 && (set->swDefault < 0 || r.swLast < set->swDefault)) set->swDefault = r.swLast;
    }
    if (skippedSamples) parser.warnings.push_back(std::to_string(skippedSamples) + " regions skipped because their sample failed to load");
    set->warnings = std::move(parser.warnings);
    set->finalizeStreaming();
    set->prepareMips();
    if (set->regions.empty()) {
        error = "no playable regions in " + path;
        if (!set->warnings.empty()) error += " (" + set->warnings.front() + ")";
        return nullptr;
    }
    return set;
}

}  // namespace hsynth
