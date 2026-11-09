#include "hsynth/fileio.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hsynth {

std::filesystem::path utf8Path(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    for (UINT cp : {(UINT)CP_UTF8, (UINT)CP_ACP}) {
        const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int n = MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0) continue;
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), w.data(), n);
        return std::filesystem::path(w);
    }
    return std::filesystem::path(std::wstring(s.begin(), s.end()));  // last resort: bytes as code units
#else
    // POSIX paths are byte strings: no conversion, nothing to throw
    return std::filesystem::path(s);
#endif
}

}  // namespace hsynth
