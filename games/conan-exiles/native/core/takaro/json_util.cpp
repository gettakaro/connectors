#include "takaro/json_util.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace takaro {

namespace {

bool PlainInteger(const std::string& s) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

void DumpTo(const JsonValue& v, std::string& o) {
    switch (v.type) {
        case JsonValue::Null: o += "null"; break;
        case JsonValue::Bool: o += v.b ? "true" : "false"; break;
        case JsonValue::Number:
            // Keep the source text so 64-bit ids survive; a number built in code has no text.
            o += v.str.empty() ? NumText(v.num) : v.str;
            break;
        case JsonValue::String: o += JsonStr(v.str); break;
        case JsonValue::Array:
            o += '[';
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) o += ',';
                DumpTo(v.arr[i], o);
            }
            o += ']';
            break;
        case JsonValue::Object:
            o += '{';
            for (size_t i = 0; i < v.obj.size(); i++) {
                if (i) o += ',';
                o += JsonStr(v.obj[i].first);
                o += ':';
                DumpTo(v.obj[i].second, o);
            }
            o += '}';
            break;
    }
}

}  // namespace

std::string JsonDump(const JsonValue& v) {
    std::string o;
    DumpTo(v, o);
    return o;
}

std::string NumText(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d < 0 ? "-Infinity" : "Infinity";
    if (d == 0) return "0";
    std::string sign = d < 0 ? "-" : "";
    double a = std::fabs(d);
    // Shortest digit string that round-trips, then JavaScript's placement rules (ECMA-262 Number::toString).
    char buf[64];
    int prec = 1;
    for (; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, a);
        if (strtod(buf, nullptr) == a) break;
    }
    // buf = d[.ddd]e[+-]XX
    std::string digits;
    const char* p = buf;
    for (; *p && *p != 'e'; p++)
        if (*p >= '0' && *p <= '9') digits += *p;
    int exp10 = *p == 'e' ? atoi(p + 1) : 0;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int k = (int)digits.size();
    int n = exp10 + 1;
    std::string out;
    if (k <= n && n <= 21) {
        out = digits + std::string((size_t)(n - k), '0');
    } else if (0 < n && n <= 21) {
        out = digits.substr(0, (size_t)n) + "." + digits.substr((size_t)n);
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string((size_t)(-n), '0') + digits;
    } else {
        int e = n - 1;
        out = digits.substr(0, 1) + (k > 1 ? "." + digits.substr(1) : "") + "e" + (e >= 0 ? "+" : "-") +
              std::to_string(e >= 0 ? e : -e);
    }
    return sign + out;
}

bool ParseJson(const std::string& text, JsonValue& out) { return JsonParse(text, out); }

bool JsonDepthExceeds(const std::string& text, int limit) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (inStr) {
            if (c == '\\') i++;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{' || c == '[') {
            if (++depth > limit) return true;
        } else if (c == '}' || c == ']') depth--;
    }
    return false;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::optional<std::string> Str(const JsonValue* v) {
    if (!v) return std::nullopt;
    if (v->type == JsonValue::Number) {
        if (!std::isfinite(v->num)) return std::nullopt;
        return PlainInteger(v->str) ? v->str : NumText(v->num);
    }
    if (v->type != JsonValue::String) return std::nullopt;
    std::string t = Trim(v->str);
    if (t.empty()) return std::nullopt;
    return t;
}

std::optional<double> Num(const JsonValue* v) {
    if (!v) return std::nullopt;
    if (v->type == JsonValue::Number) return std::isfinite(v->num) ? std::optional<double>(v->num) : std::nullopt;
    if (v->type != JsonValue::String) return std::nullopt;
    std::string t = Trim(v->str);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    double d = strtod(t.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(d)) return std::nullopt;
    return d;
}

const JsonValue& AsRecord(const JsonValue* v) {
    static const JsonValue empty = [] {
        JsonValue o;
        o.type = JsonValue::Object;
        return o;
    }();
    return v && v->type == JsonValue::Object ? *v : empty;
}

bool IsNonEmptyRecord(const JsonValue* v) { return v && v->type == JsonValue::Object && !v->obj.empty(); }

bool Truthy(const JsonValue* v) {
    if (!v) return false;
    switch (v->type) {
        case JsonValue::Null: return false;
        case JsonValue::Bool: return v->b;
        case JsonValue::Number: return v->num != 0 && !std::isnan(v->num);
        case JsonValue::String: return !v->str.empty();
        default: return true;
    }
}

JsonValue JStr(const std::string& s) {
    JsonValue v;
    v.type = JsonValue::String;
    v.str = s;
    return v;
}
JsonValue JNum(double d) {
    JsonValue v;
    v.type = JsonValue::Number;
    v.num = d;
    return v;
}
JsonValue JBool(bool b) {
    JsonValue v;
    v.type = JsonValue::Bool;
    v.b = b;
    return v;
}
JsonValue JNull() { return JsonValue(); }
JsonValue JObj() {
    JsonValue v;
    v.type = JsonValue::Object;
    return v;
}
JsonValue JArr() {
    JsonValue v;
    v.type = JsonValue::Array;
    return v;
}
void Put(JsonValue& obj, const std::string& key, JsonValue v) {
    for (auto& kv : obj.obj)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    obj.obj.emplace_back(key, std::move(v));
}
bool Has(const JsonValue& obj, const std::string& key) { return obj.get(key) != nullptr; }

std::string JsString(const JsonValue* v) {
    if (!v) return "undefined";
    switch (v->type) {
        case JsonValue::Null: return "null";
        case JsonValue::Bool: return v->b ? "true" : "false";
        case JsonValue::Number: return PlainInteger(v->str) ? v->str : NumText(v->num);
        case JsonValue::String: return v->str;
        case JsonValue::Array: {
            std::string o;
            for (size_t i = 0; i < v->arr.size(); i++) {
                if (i) o += ",";
                const JsonValue& e = v->arr[i];
                if (e.type != JsonValue::Null) o += JsString(&e);
            }
            return o;
        }
        default: return "[object Object]";
    }
}

std::string JsStringify(const JsonValue* v) { return v ? JsonDump(*v) : "undefined"; }

ObjBuilder& ObjBuilder::Raw(const char* key, const std::string& json) {
    if (!first_) out_ += ',';
    first_ = false;
    out_ += JsonStr(key);
    out_ += ':';
    out_ += json;
    return *this;
}

}  // namespace takaro
