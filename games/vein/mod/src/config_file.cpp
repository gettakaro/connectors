#include "config_file.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

namespace ConfigFile {
namespace {

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

bool ValidKey(const std::string& k) {
    if (k.rfind("TAKARO_", 0) != 0 && k.rfind("VEIN_", 0) != 0) return false;
    for (unsigned char c : k)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

}  // namespace

Parsed Parse(const std::string& input) {
    Parsed out;
    std::string text = input;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    size_t start = 0, lineNo = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(start, end - start));
        ++lineNo;
        start = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            if (end == text.size()) break;
            continue;
        }
        if (line.rfind("export ", 0) == 0) line = Trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            out.warnings.push_back("line " + std::to_string(lineNo) + ": no '=' (expected KEY=VALUE)");
        } else {
            std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            if (!ValidKey(key))
                out.warnings.push_back("line " + std::to_string(lineNo) + ": ignored key '" + key +
                                       "' (only TAKARO_* and VEIN_* keys are read)");
            else
                out.entries.emplace_back(key, value);
        }
        if (end == text.size()) break;
    }
    return out;
}

std::string DefaultPath() {
    const char* forced = getenv("TAKARO_CONFIG_FILE");
    if (forced && *forced) return forced;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&DefaultPath), &info) && info.dli_fname) {
        std::string lib(info.dli_fname);
        size_t slash = lib.rfind('/');
        if (slash != std::string::npos) return lib.substr(0, slash + 1) + "takaro.cfg";
    }
    return "takaro.cfg";
}

Applied Apply(const std::string& path) {
    Applied a;
    a.path = path;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return a;
    a.found = true;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 256 * 1024) text.append(buf, n);
    fclose(f);
    Parsed p = Parse(text);
    a.warnings = p.warnings;
    // Last occurrence wins, matching how shells read env files.
    std::vector<std::pair<std::string, std::string>> last;
    for (auto& e : p.entries) {
        bool replaced = false;
        for (auto& l : last)
            if (l.first == e.first) { l.second = e.second; replaced = true; }
        if (!replaced) last.push_back(e);
    }
    for (auto& e : last) {
        const char* cur = getenv(e.first.c_str());
        if (cur && *cur) { a.keptEnv.push_back(e.first); continue; }
        if (setenv(e.first.c_str(), e.second.c_str(), 1) == 0) ++a.applied;
    }
    return a;
}

}  // namespace ConfigFile
