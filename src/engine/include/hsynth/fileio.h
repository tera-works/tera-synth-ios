// UTF-8 file path helpers. All engine APIs take UTF-8 paths; on Windows the
// narrow C runtime functions would interpret them in the ANSI code page, so
// files are opened through the wide-character APIs there.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace hsynth {

// Never throws. On Windows a string that is not valid UTF-8 (e.g. Shift-JIS
// sample names in older SFZ libraries) is read in the system ANSI code page
// instead; std::filesystem would throw on it.
std::filesystem::path utf8Path(const std::string& s);

inline std::string utf8String(const std::filesystem::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

inline FILE* fopenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring w = utf8Path(path).wstring();
    const std::wstring m(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(w.c_str(), m.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace hsynth
