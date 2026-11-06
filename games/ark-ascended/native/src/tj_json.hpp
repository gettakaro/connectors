#pragma once
// Small strict JSON DOM for the Takaro Generic protocol. Bounded depth and size,
// UTF-8 validated, \u escapes (including surrogate pairs) decoded. Objects keep
// insertion order; a repeated key replaces the earlier value (JSON.parse semantics).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tj {

class Value {
 public:
  enum class Type { null, boolean, number, string, array, object };
  using Array = std::vector<Value>;
  using Object = std::vector<std::pair<std::string, Value>>;

  Value() = default;
  static Value null() { return Value(); }
  static Value boolean(bool v) { Value x; x.type_ = Type::boolean; x.bool_ = v; return x; }
  static Value number(double v) { Value x; x.type_ = Type::number; x.number_ = v; return x; }
  static Value string(std::string v) { Value x; x.type_ = Type::string; x.string_ = std::move(v); return x; }
  static Value array(Array v = {}) {
    Value x; x.type_ = Type::array; x.array_ = std::make_shared<Array>(std::move(v)); return x;
  }
  static Value object(Object v = {}) {
    Value x; x.type_ = Type::object; x.object_ = std::make_shared<Object>(std::move(v)); return x;
  }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::null; }
  bool is_bool() const { return type_ == Type::boolean; }
  bool is_number() const { return type_ == Type::number; }
  bool is_string() const { return type_ == Type::string; }
  bool is_array() const { return type_ == Type::array; }
  bool is_object() const { return type_ == Type::object; }

  bool as_bool() const { return bool_; }
  double as_number() const { return number_; }
  const std::string& as_string() const { return string_; }
  const Array& as_array() const { static const Array empty; return array_ ? *array_ : empty; }
  const Object& as_object() const { static const Object empty; return object_ ? *object_ : empty; }
  Array& mutable_array() { if (!array_) array_ = std::make_shared<Array>(); return *array_; }

  // Missing key and explicit JSON null are both "absent" (modules send explicit nulls).
  const Value* get(std::string_view key) const {
    if (type_ != Type::object || !object_) return nullptr;
    for (auto it = object_->rbegin(); it != object_->rend(); ++it)
      if (it->first == key) return it->second.is_null() ? nullptr : &it->second;
    return nullptr;
  }
  bool has_key(std::string_view key) const {
    if (type_ != Type::object || !object_) return false;
    for (const auto& entry : *object_) if (entry.first == key) return true;
    return false;
  }
  Value& set(std::string key, Value value) {
    if (type_ != Type::object) { type_ = Type::object; object_ = std::make_shared<Object>(); }
    for (auto& entry : *object_)
      if (entry.first == key) { entry.second = std::move(value); return entry.second; }
    object_->emplace_back(std::move(key), std::move(value));
    return object_->back().second;
  }
  void push(Value value) {
    if (type_ != Type::array) { type_ = Type::array; array_ = std::make_shared<Array>(); }
    array_->push_back(std::move(value));
  }

 private:
  Type type_ = Type::null;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::shared_ptr<Array> array_;
  std::shared_ptr<Object> object_;
};

inline void append_utf8(std::string& out, uint32_t c) {
  if (c < 0x80) out.push_back(static_cast<char>(c));
  else if (c < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 63)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (c & 63)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (c & 63)));
  }
}

// Returns false on any invalid UTF-8 (overlong, surrogate, > U+10FFFF, truncated).
inline bool valid_utf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) continue;
    unsigned count; uint32_t code, minimum;
    if (c >= 0xc2 && c <= 0xdf) { count = 1; code = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { count = 2; code = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 3; code = c & 7; minimum = 0x10000; }
    else return false;
    if (i + count > s.size()) return false;
    for (unsigned j = 0; j < count; ++j) {
      const unsigned char next = static_cast<unsigned char>(s[i++]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}

class Parser {
 public:
  static constexpr size_t kMaxDepth = 64;
  static constexpr size_t kMaxSize = 4 * 1024 * 1024;

  static std::optional<Value> parse(std::string_view text) {
    if (text.size() > kMaxSize || !valid_utf8(text)) return std::nullopt;
    Parser p(text);
    Value v;
    p.spaces();
    if (!p.value(v, 0)) return std::nullopt;
    p.spaces();
    if (p.i_ != text.size()) return std::nullopt;
    return v;
  }

 private:
  explicit Parser(std::string_view s) : s_(s) {}
  std::string_view s_;
  size_t i_ = 0;

  void spaces() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool literal(std::string_view word) {
    if (s_.substr(i_, word.size()) != word) return false;
    i_ += word.size();
    return true;
  }
  bool hex4(uint32_t& out) {
    if (i_ + 4 > s_.size()) return false;
    out = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size()) {
      const unsigned char c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
      if (i_ >= s_.size()) return false;
      const char e = s_[i_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t code;
          if (!hex4(code)) return false;
          if (code >= 0xd800 && code <= 0xdbff) {
            uint32_t low;
            if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
            i_ += 2;
            if (!hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
          } else if (code >= 0xdc00 && code <= 0xdfff) {
            return false;
          }
          append_utf8(out, code);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool number(Value& out) {
    const size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size()) return false;
    if (s_[i_] == '0') ++i_;
    else if (s_[i_] >= '1' && s_[i_] <= '9') { while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_; }
    else return false;
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      const size_t digits = i_;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
      if (digits == i_) return false;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      const size_t digits = i_;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
      if (digits == i_) return false;
    }
    if (i_ - start > 64) return false;
    const std::string text(s_.substr(start, i_ - start));
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(v)) return false;
    out = Value::number(v);
    return true;
  }
  bool value(Value& out, size_t depth) {
    if (depth > kMaxDepth || i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == 'n') { if (!literal("null")) return false; out = Value::null(); return true; }
    if (c == 't') { if (!literal("true")) return false; out = Value::boolean(true); return true; }
    if (c == 'f') { if (!literal("false")) return false; out = Value::boolean(false); return true; }
    if (c == '"') { std::string s; if (!string(s)) return false; out = Value::string(std::move(s)); return true; }
    if (c == '[') {
      ++i_;
      Value::Array items;
      spaces();
      if (i_ < s_.size() && s_[i_] == ']') { ++i_; out = Value::array(std::move(items)); return true; }
      for (;;) {
        Value item;
        spaces();
        if (!value(item, depth + 1)) return false;
        items.push_back(std::move(item));
        spaces();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ']') { ++i_; out = Value::array(std::move(items)); return true; }
        if (s_[i_++] != ',') return false;
      }
    }
    if (c == '{') {
      ++i_;
      Value obj = Value::object();
      spaces();
      if (i_ < s_.size() && s_[i_] == '}') { ++i_; out = std::move(obj); return true; }
      for (;;) {
        spaces();
        std::string key;
        if (!string(key)) return false;
        spaces();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        spaces();
        Value item;
        if (!value(item, depth + 1)) return false;
        obj.set(std::move(key), std::move(item));
        spaces();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == '}') { ++i_; out = std::move(obj); return true; }
        if (s_[i_++] != ',') return false;
      }
    }
    return number(out);
  }
};

inline std::optional<Value> parse(std::string_view text) { return Parser::parse(text); }

inline void write_string(std::string& out, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c == '\b') out += "\\b";
    else if (c == '\f') out += "\\f";
    else if (c < 0x20) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    else out.push_back(static_cast<char>(c));
  }
  out.push_back('"');
}

inline void write_number(std::string& out, double v) {
  if (!std::isfinite(v)) { out += "null"; return; }
  if (std::floor(v) == v && std::fabs(v) < 9007199254740992.0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    out += buf;
    return;
  }
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  // Prefer the shortest representation that round-trips.
  for (int precision = 6; precision < 17; ++precision) {
    char shorter[40];
    std::snprintf(shorter, sizeof(shorter), "%.*g", precision, v);
    if (std::strtod(shorter, nullptr) == v) { out += shorter; return; }
  }
  out += buf;
}

inline void write(std::string& out, const Value& v) {
  switch (v.type()) {
    case Value::Type::null: out += "null"; break;
    case Value::Type::boolean: out += v.as_bool() ? "true" : "false"; break;
    case Value::Type::number: write_number(out, v.as_number()); break;
    case Value::Type::string: write_string(out, v.as_string()); break;
    case Value::Type::array: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : v.as_array()) { if (!first) out.push_back(','); first = false; write(out, item); }
      out.push_back(']');
      break;
    }
    case Value::Type::object: {
      out.push_back('{');
      bool first = true;
      for (const auto& [key, item] : v.as_object()) {
        if (!first) out.push_back(',');
        first = false;
        write_string(out, key);
        out.push_back(':');
        write(out, item);
      }
      out.push_back('}');
      break;
    }
  }
}

inline std::string dump(const Value& v) { std::string out; write(out, v); return out; }

// Builder helpers.
inline Value str(std::string v) { return Value::string(std::move(v)); }
inline Value num(double v) { return Value::number(v); }
inline Value obj(std::initializer_list<std::pair<std::string, Value>> entries) {
  Value o = Value::object();
  for (const auto& [k, v] : entries) o.set(k, v);
  return o;
}

}  // namespace tj
