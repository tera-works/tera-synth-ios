#include "hsynth/preset_index.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "hsynth/fileio.h"
#include "hsynth/json.h"
#include "hsynth/preset.h"

namespace hsynth {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);  // ASCII only: UTF-8 bytes >= 0x80 stay
    return s;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Metadata of one preset file without building the parameter array.
bool readInfo(const std::string& file, PresetInfo& info) {
    std::string text, err;
    if (!readFile(file, text)) return false;
    Json root;
    if (!Json::parse(text, root, err) || !root.isObject()) return false;
    if (auto f = root.find("format"); f && f->isString() && f->string() != "hsynth-preset") return false;  // multis etc.
    info.file = file;
    if (auto n = root.find("name"); n && n->isString()) info.name = n->string();
    if (info.name.empty()) info.name = utf8String(utf8Path(file).stem());
    if (auto n = root.find("category"); n && n->isString()) info.category = n->string();
    if (auto n = root.find("author"); n && n->isString()) info.author = n->string();
    if (auto n = root.find("comment"); n && n->isString()) info.comment = n->string();
    if (auto t = root.find("tags"); t && t->isArray())
        for (size_t i = 0; i < t->size(); ++i)
            if (t->at(i).isString() && !t->at(i).string().empty()) info.tags.push_back(t->at(i).string());
    // automatic tags: the layer types that are on, sampled content
    static const char* kLayers[3] = {"a", "b", "c"};
    std::set<std::string> types;
    if (auto p = root.find("params"); p && p->isObject()) {
        for (int l = 0; l < 3; ++l) {
            const Json* t = p->find(std::string(kLayers[l]) + ".type");
            std::string v = t && t->isString() ? t->string() : (l == 0 ? "va" : "off");  // layer A defaults to VA
            v = lower(v);
            if (v != "off" && !v.empty()) types.insert(v);
        }
    }
    for (auto t : types) {
        for (auto& c : t) c = (char)std::toupper((unsigned char)c);
        info.autoTags.push_back(t);
    }
    if (auto s = root.find("samples"); s && s->isObject())
        for (int l = 0; l < 3; ++l)
            if (auto v = s->find(kLayers[l]); v && v->isString() && !v->string().empty()) {
                info.autoTags.push_back("sampled");
                break;
            }
    if (types.size() > 1) info.autoTags.push_back("layered");
    info.sortKey = lower(info.name);
    return true;
}

}  // namespace

std::vector<std::string> PresetIndex::splitTags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',' || c == ';') {
            cur = trim(cur);
            if (!cur.empty() && std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

std::string PresetIndex::joinTags(const std::vector<std::string>& t) {
    std::string s;
    for (const auto& x : t) s += (s.empty() ? "" : ", ") + x;
    return s;
}

void PresetIndex::setFavoritesFile(const std::string& path) {
    favoritesFile_ = path;
    favorites_.clear();
    std::string text, err;
    Json root;
    if (readFile(path, text) && Json::parse(text, root, err) && root.isArray())
        for (size_t i = 0; i < root.size(); ++i)
            if (root.at(i).isString()) favorites_.insert(root.at(i).string());
    for (auto& it : items_) it.favorite = favorites_.count(it.file) > 0;
}

void PresetIndex::saveFavorites() const {
    if (favoritesFile_.empty()) return;
    Json a = Json::array();
    for (const auto& f : favorites_) a.push(f);
    std::ofstream out(utf8Path(favoritesFile_), std::ios::binary);
    out << a.dump();
}

void PresetIndex::rescan() {
    items_.clear();
    for (const auto& [dir, user] : folders_) {
        std::error_code ec;
        const fs::path root = utf8Path(dir);
        if (!fs::exists(root, ec)) continue;
        std::vector<std::string> files;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (lower(utf8String(it->path().extension())) == ".json") files.push_back(utf8String(it->path()));
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            PresetInfo info;
            if (!readInfo(f, info)) continue;
            info.user = user;
            if (user) info.autoTags.push_back("user");
            info.favorite = favorites_.count(f) > 0;
            items_.push_back(std::move(info));
        }
    }
}

std::vector<int> PresetIndex::query(const PresetQuery& q) const {
    std::vector<std::string> words;
    {
        std::istringstream ss(lower(q.text));
        std::string w;
        while (ss >> w) words.push_back(w);
    }
    std::vector<int> out;
    for (int i = 0; i < (int)items_.size(); ++i) {
        const PresetInfo& p = items_[(size_t)i];
        if (q.favoritesOnly && !p.favorite) continue;
        if (q.userOnly && !p.user) continue;
        if (!q.category.empty() && p.category != q.category) continue;
        bool ok = true;
        for (const auto& w : words) {
            if (w.size() > 1 && w[0] == '#') {  // tag (prefix match)
                const std::string t = w.substr(1);
                bool hit = false;
                for (const auto* list : {&p.tags, &p.autoTags})
                    for (const auto& x : *list) hit = hit || lower(x).compare(0, t.size(), t) == 0;
                ok = hit;
            } else {
                std::string hay = p.sortKey + "\n" + lower(p.category) + "\n" + lower(p.author) + "\n" + lower(p.comment);
                for (const auto* list : {&p.tags, &p.autoTags})
                    for (const auto& x : *list) hay += "\n" + lower(x);
                ok = hay.find(w) != std::string::npos;
            }
            if (!ok) break;
        }
        if (ok) out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
        const PresetInfo& x = items_[(size_t)a];
        const PresetInfo& y = items_[(size_t)b];
        if (x.category != y.category) return lower(x.category) < lower(y.category);
        return x.sortKey < y.sortKey;
    });
    return out;
}

std::vector<std::pair<std::string, int>> PresetIndex::categories() const {
    std::map<std::string, int> m;
    for (const auto& p : items_) ++m[p.category];
    std::vector<std::pair<std::string, int>> out(m.begin(), m.end());
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return lower(a.first) < lower(b.first); });
    return out;
}

std::vector<std::pair<std::string, int>> PresetIndex::tags() const {
    std::map<std::string, int> m;
    for (const auto& p : items_) {
        for (const auto& t : p.tags) ++m[t];
        for (const auto& t : p.autoTags) ++m[t];
    }
    std::vector<std::pair<std::string, int>> out(m.begin(), m.end());
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
    return out;
}

int PresetIndex::find(const std::string& file) const {
    for (int i = 0; i < (int)items_.size(); ++i)
        if (items_[(size_t)i].file == file) return i;
    return -1;
}

void PresetIndex::setFavorite(int index, bool on) {
    if (index < 0 || index >= (int)items_.size()) return;
    PresetInfo& p = items_[(size_t)index];
    p.favorite = on;
    if (on) favorites_.insert(p.file);
    else favorites_.erase(p.file);
    saveFavorites();
}

bool PresetIndex::updateMetadata(int index, const std::string& category, const std::vector<std::string>& tags,
                                 std::string& error) {
    if (index < 0 || index >= (int)items_.size()) { error = "no such preset"; return false; }
    PresetInfo& info = items_[(size_t)index];
    if (!info.user) { error = "factory presets are read-only (save a copy first)"; return false; }
    Preset p;
    if (!loadPresetFile(info.file, p, error)) return false;
    p.category = category;
    p.tags = tags;
    if (!savePresetFile(info.file, p, error)) return false;
    info.category = category;
    info.tags = tags;
    return true;
}

}  // namespace hsynth
