#pragma once
// takaro.cfg: KEY=VALUE lines next to libtakaro-ark-native.so (or TAKARO_CONFIG_FILE).
// Only TAKARO_* keys are read. A non-empty environment variable always wins.
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ark_config {

inline std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

struct File {
  std::string path;
  bool found = false;
  std::map<std::string, std::string> values;
  std::vector<std::string> warnings;  // never contains a value
};

inline File parse(const std::string& path, const std::string& input) {
  File file;
  file.path = path;
  file.found = true;
  std::string text = input;
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
  size_t start = 0, line_no = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = trim(text.substr(start, end - start));
    start = end + 1;
    ++line_no;
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      file.warnings.push_back("line " + std::to_string(line_no) + ": expected KEY=VALUE");
      continue;
    }
    const std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
      value = value.substr(1, value.size() - 2);
    bool valid = key.rfind("TAKARO_", 0) == 0;
    for (unsigned char c : key)
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) valid = false;
    if (!valid) {
      file.warnings.push_back("line " + std::to_string(line_no) + ": ignored key (only TAKARO_* keys are read)");
      continue;
    }
    file.values[key] = value;
  }
  return file;
}

// A file shipped next to libtakaro-ark-native.so.
inline std::string plugin_file(const std::string& name) {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&plugin_file), &info) && info.dli_fname) {
    std::string lib(info.dli_fname);
    const size_t slash = lib.rfind('/');
    if (slash != std::string::npos) return lib.substr(0, slash + 1) + name;
  }
  return name;
}

inline std::string default_path() {
  const char* forced = std::getenv("TAKARO_CONFIG_FILE");
  if (forced && *forced) return forced;
  return plugin_file("takaro.cfg");
}

inline File load(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { File file; file.path = path; return file; }
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 65536) text.append(buf, n);
  std::fclose(f);
  return parse(path, text);
}

// Non-empty environment value, else the file's value, else empty.
inline std::string get(const File& file, const char* name) {
  const char* env = std::getenv(name);
  if (env && *env) return env;
  auto it = file.values.find(name);
  return it == file.values.end() ? std::string() : it->second;
}

}  // namespace ark_config
