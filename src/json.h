// Minimal JSON reader/writer for the control API  (Phase 2.5).
//
// The pre-Phase-2 control layer parsed flat JSON with hand-rolled pointer
// walking in three different places. Once the API had to carry nested objects
// (ParamSpec arrays, per-module parameter maps) that approach stopped scaling,
// so it is replaced by this one small parser.
//
// Deliberately minimal, matching the project's "no external dependencies"
// rule: enough to read the request bodies clumsy actually receives, no more.
// Unsupported by design: \uXXXX escapes are passed through verbatim, and
// numbers use strtod (so no bignum or exact-decimal semantics).
#pragma once

#include <string>
#include <vector>
#include <utility>

class JsonValue {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Type        type = Null;
    bool        boolVal = false;
    double      numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue> > obj; // ordered; n is small

    bool isNull()   const { return type == Null; }
    bool isObject() const { return type == Object; }
    bool isArray()  const { return type == Array; }

    // Object member lookup. Returns nullptr when absent.
    const JsonValue* find(const std::string &key) const;
    bool has(const std::string &key) const { return find(key) != nullptr; }

    // Scalar accessors. A value of any scalar type converts to a string, which
    // is what Module::setParam takes — numbers render without trailing zeros so
    // that 100 stays "100" and 10.5 stays "10.5".
    std::string asString() const;
    double      asNumber() const;
    bool        asBool()   const;

    // Convenience: member as string, or a fallback when missing.
    std::string str(const std::string &key, const std::string &fallback = "") const;
};

// Parses text into out. Returns false on malformed input (out is left empty).
bool jsonParse(const std::string &text, JsonValue &out);

// Escapes a raw string for use inside a JSON string literal (no quotes added).
std::string jsonEscape(const std::string &s);
