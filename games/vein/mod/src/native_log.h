#pragma once

#include <memory>
#include <string>
#include <vector>

namespace NativeLog {
struct Parsed {
    std::string type, dataJson;
};
class Parser {
public:
    Parser();
    ~Parser();
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    // Compiles PCRE2 grammar and validates custom captures against legacy fixtures.
    // An incompatible override returns its exact VEIN_LOG_*_RE key.
    bool Configure(std::string& errorKey, std::string& detail);
    std::vector<Parsed> Feed(const std::string& rawLine);
    bool Ready() const;
    bool CustomJoin() const;
    bool CustomChat() const;
    bool TailConnections() const;
    std::string LastError() const;
    std::string Redact(const std::string& rawLine) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace NativeLog
