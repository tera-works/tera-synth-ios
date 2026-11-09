// Scale tuning: Scala (.scl) files to the 12 per-pitch-class offsets of a
// part (tune.c .. tune.b, cents relative to equal temperament).
#pragma once

#include <string>
#include <vector>

namespace hsynth {

struct ScalaScale {
    std::string description;
    std::vector<double> cents;  // degrees 1..n in cents (the last one is the period, usually 1200)
};

// Parses Scala text: '!' comments, description line, note count, then one
// pitch per line (cents if it contains '.', otherwise a ratio "a/b" or "a").
bool parseScala(const std::string& text, ScalaScale& out, std::string& error);
bool loadScalaFile(const std::string& path, ScalaScale& out, std::string& error);

// Offsets for pitch classes C..B (cents) with scale degree 0 on pitch class
// `root` (0 = C). Needs a 12-note scale with a 1200-cent period; other scales
// return false.
bool scalaToOffsets(const ScalaScale& s, int root, float offsets[12], std::string& error);

}  // namespace hsynth
