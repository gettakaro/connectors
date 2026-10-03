// Portable path helpers. The file primitives live in platform/<os>/fileio_*.cpp.
#include "takaro/fileio.h"

namespace takaro {

bool IsAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
}

static char SepFor(const std::string& dir) { return dir.find('\\') != std::string::npos ? '\\' : '/'; }

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + SepFor(dir) + name;
}

std::string DirName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string() : path.substr(0, s);
}

std::string ResolvePath(const std::string& baseDir, const std::string& p) {
    if (p.empty() || IsAbsolutePath(p)) return p;
    std::string rel = p;
    if (SepFor(baseDir) == '\\')
        for (auto& c : rel)
            if (c == '/') c = '\\';
    return JoinPath(baseDir, rel);
}

}  // namespace takaro
