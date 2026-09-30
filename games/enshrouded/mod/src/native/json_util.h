// JSON helpers for the native Takaro connector, on top of common.h's JsonValue parser.
// Everything here is pure (no Windows, no game) so the host tests link it directly.
#pragma once
#include "common.h"

#include <optional>
#include <string>

namespace native {

// Compact JSON text of a value, keys in source order. Numbers keep their source text.
std::string JsonDump(const JsonValue& v);
// JavaScript Number#toString for a finite double (the sidecar's String(n)).
std::string NumText(double d);
// Parses text; false on malformed input or nesting deeper than 64 (common.cpp's limit).
bool ParseJson(const std::string& text, JsonValue& out);

// True when the JSON text nests arrays/objects deeper than `limit` (strings are skipped).
bool JsonDepthExceeds(const std::string& text, int limit);

// The sidecar's mapping.ts helpers, value for value:
//   str(): a finite number as text, a string trimmed (null when blank), anything else null.
//   num(): a finite number, or a string that parses as one; otherwise null.
//   asRecord(): the object itself, otherwise an empty object.
std::optional<std::string> Str(const JsonValue* v);
std::optional<double> Num(const JsonValue* v);
const JsonValue& AsRecord(const JsonValue* v);
inline const JsonValue* Field(const JsonValue& obj, const char* key) { return obj.get(key); }
bool IsNonEmptyRecord(const JsonValue* v);
// JavaScript truthiness of a JSON value (absent = false).
bool Truthy(const JsonValue* v);

// Small builder for JSON objects with a fixed key order.
class ObjBuilder {
public:
    ObjBuilder& Raw(const char* key, const std::string& json);
    ObjBuilder& S(const char* key, const std::string& value) { return Raw(key, JsonStr(value)); }
    ObjBuilder& N(const char* key, double value) { return Raw(key, NumText(value)); }
    ObjBuilder& B(const char* key, bool value) { return Raw(key, value ? "true" : "false"); }
    ObjBuilder& Null(const char* key) { return Raw(key, "null"); }
    std::string Done() const { return out_ + "}"; }

private:
    std::string out_ = "{";
    bool first_ = true;
};

// JsonValue constructors and object helpers.
JsonValue JStr(const std::string& s);
JsonValue JNum(double d);
JsonValue JBool(bool b);
JsonValue JNull();
JsonValue JObj();
JsonValue JArr();
void Put(JsonValue& obj, const std::string& key, JsonValue v);  // replaces an existing key, else appends
bool Has(const JsonValue& obj, const std::string& key);
// JavaScript String(value) of a JSON value (strings untouched, numbers as Number#toString).
std::string JsString(const JsonValue* v);
// JSON.stringify of an optional value ("undefined" when absent, as the sidecar's error messages print it).
std::string JsStringify(const JsonValue* v);

std::string Lower(std::string s);
std::string Trim(const std::string& s);

}  // namespace native
