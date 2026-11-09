#include "hsynth/tuning.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "hsynth/fileio.h"

namespace hsynth {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
}  // namespace

bool parseScala(const std::string& text, ScalaScale& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int stage = 0;  // 0 = description, 1 = count, 2 = pitches
    long count = 0;
    ScalaScale s;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '!') continue;
        const std::string t = trim(line);
        if (stage == 0) { s.description = t; stage = 1; continue; }
        if (t.empty()) continue;
        if (stage == 1) {
            count = std::strtol(t.c_str(), nullptr, 10);
            if (count <= 0 || count > 1024) { error = "bad note count"; return false; }
            stage = 2;
            continue;
        }
        // the pitch is the first token; anything after it is a comment
        const std::string tok = t.substr(0, t.find_first_of(" \t"));
        double cents;
        if (tok.find('.') != std::string::npos) {
            cents = std::strtod(tok.c_str(), nullptr);
        } else {
            const size_t slash = tok.find('/');
            const double num = std::strtod(tok.c_str(), nullptr);
            const double den = slash == std::string::npos ? 1.0 : std::strtod(tok.c_str() + slash + 1, nullptr);
            if (num <= 0.0 || den <= 0.0) { error = "bad ratio '" + tok + "'"; return false; }
            cents = 1200.0 * std::log2(num / den);
        }
        s.cents.push_back(cents);
        if ((long)s.cents.size() == count) break;
    }
    if (stage < 2 || (long)s.cents.size() != count) { error = "truncated scale"; return false; }
    out = std::move(s);
    return true;
}

bool loadScalaFile(const std::string& path, ScalaScale& out, std::string& error) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!parseScala(ss.str(), out, error)) { error = path + ": " + error; return false; }
    return true;
}

bool scalaToOffsets(const ScalaScale& s, int root, float offsets[12], std::string& error) {
    if (s.cents.size() != 12 || std::fabs(s.cents.back() - 1200.0) > 0.5) {
        error = "only 12-note scales with a 1200-cent octave can be mapped to the keyboard (" +
                std::to_string(s.cents.size()) + " notes)";
        return false;
    }
    root = ((root % 12) + 12) % 12;
    for (int k = 0; k < 12; ++k) {
        const double c = k == 0 ? 0.0 : s.cents[(size_t)k - 1];
        const double off = c - 100.0 * k;
        offsets[(root + k) % 12] = (float)std::max(-100.0, std::min(100.0, off));
        if (std::fabs(off) > 100.0) error = "degree " + std::to_string(k) + " is more than 100 cents from 12-TET (clamped)";
    }
    return true;
}

}  // namespace hsynth
