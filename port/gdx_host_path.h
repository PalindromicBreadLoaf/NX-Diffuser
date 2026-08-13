#pragma once

#include <filesystem>
#include <string>

namespace gdx {

// std::filesystem does not know about Horizon's device prefixes.
inline bool HostPathHasDevicePrefix(const std::filesystem::path& p) {
    const std::string s = p.string();
    const std::string::size_type colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return false;
    }
    if (s[colon + 1] != '/' && s[colon + 1] != '\\') {
        return false;
    }
    return s.find_first_of("/\\") > colon;
}

inline std::filesystem::path HostAbsolute(const std::filesystem::path& p) {
    if (HostPathHasDevicePrefix(p)) {
        return p;
    }
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    return ec ? p : abs;
}

} // namespace gdx
