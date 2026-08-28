#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// JsonValue accessors
// ---------------------------------------------------------------------------

const JsonValue* JsonValue::find(const std::string &key) const {
    if (type != Object) return nullptr;
    for (size_t i = 0; i < obj.size(); ++i) {
        if (obj[i].first == key) return &obj[i].second;
    }
    return nullptr;
}

std::string JsonValue::asString() const {
    switch (type) {
    case String: return strVal;
    case Bool:   return boolVal ? "true" : "false";
    case Null:   return "";
    case Number: {
        char buf[64];
        // Render integral values without a decimal point so that setParam sees
        // "100" rather than "100.000000".
        //
        // The range test comes first and && short-circuits, which is the whole
        // point: converting a double outside long long's range is undefined
        // behaviour, and strtod happily parses "1e19" - or "1e400", which
        // becomes +Infinity - straight out of a request body. Testing the
        // range after the cast would mean the UB had already happened.
        if (numVal < 1e15 && numVal > -1e15 &&
            numVal == (double)(long long)numVal) {
            snprintf(buf, sizeof(buf), "%lld", (long long)numVal);
        } else {
            snprintf(buf, sizeof(buf), "%.6g", numVal);
        }
        return std::string(buf);
    }
    default: return "";
    }
}

double JsonValue::asNumber() const {
    switch (type) {
    case Number: return numVal;
    case Bool:   return boolVal ? 1.0 : 0.0;
    case String: return atof(strVal.c_str());
    default:     return 0.0;
    }
}

bool JsonValue::asBool() const {
    switch (type) {
    case Bool:   return boolVal;
    case Number: return numVal != 0.0;
    case String: return strVal == "true" || strVal == "1" ||
                        strVal == "on"   || strVal == "yes";
    default:     return false;
    }
}

std::string JsonValue::str(const std::string &key, const std::string &fallback) const {
    const JsonValue *v = find(key);
    return v ? v->asString() : fallback;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Parser {
    const char *p;
    const char *end;
    int depth;

    explicit Parser(const std::string &s)
        : p(s.c_str()), end(s.c_str() + s.size()), depth(0) {}

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    }

    bool parseValue(JsonValue &out);
    bool parseString(std::string &out);
    bool parseObject(JsonValue &out);
    bool parseArray(JsonValue &out);
    bool parseNumber(JsonValue &out);
};

// Guard against a hostile deeply nested body blowing the stack.
const int MAX_DEPTH = 32;

bool Parser::parseString(std::string &out) {
    if (p >= end || *p != '"') return false;
    p++;
    out.clear();
    while (p < end) {
        char c = *p++;
        if (c == '"') return true;
        if (c != '\\') { out.push_back(c); continue; }
        if (p >= end) return false;
        char esc = *p++;
        switch (esc) {
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/');  break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u':
            // \uXXXX is passed through as-is; clumsy's keys and filters are ASCII.
            out.append("\\u");
            for (int i = 0; i < 4 && p < end; ++i) out.push_back(*p++);
            break;
        default:
            out.push_back(esc);
            break;
        }
    }
    return false; // unterminated
}

bool Parser::parseNumber(JsonValue &out) {
    char *stop = nullptr;
    double v = strtod(p, &stop);
    if (stop == p) return false;
    p = stop;
    out.type = JsonValue::Number;
    out.numVal = v;
    return true;
}

bool Parser::parseArray(JsonValue &out) {
    if (p >= end || *p != '[') return false;
    p++;
    out.type = JsonValue::Array;
    skipWs();
    if (p < end && *p == ']') { p++; return true; }
    for (;;) {
        JsonValue item;
        skipWs();
        if (!parseValue(item)) return false;
        out.arr.push_back(item);
        skipWs();
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == ']') { p++; return true; }
        return false;
    }
}

bool Parser::parseObject(JsonValue &out) {
    if (p >= end || *p != '{') return false;
    p++;
    out.type = JsonValue::Object;
    skipWs();
    if (p < end && *p == '}') { p++; return true; }
    for (;;) {
        std::string key;
        JsonValue   val;
        skipWs();
        if (!parseString(key)) return false;
        skipWs();
        if (p >= end || *p != ':') return false;
        p++;
        skipWs();
        if (!parseValue(val)) return false;
        out.obj.push_back(std::make_pair(key, val));
        skipWs();
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; return true; }
        return false;
    }
}

bool Parser::parseValue(JsonValue &out) {
    if (depth >= MAX_DEPTH) return false;
    skipWs();
    if (p >= end) return false;

    switch (*p) {
    case '{': {
        depth++;
        bool ok = parseObject(out);
        depth--;
        return ok;
    }
    case '[': {
        depth++;
        bool ok = parseArray(out);
        depth--;
        return ok;
    }
    case '"': {
        std::string s;
        if (!parseString(s)) return false;
        out.type = JsonValue::String;
        out.strVal = s;
        return true;
    }
    case 't':
        if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
            p += 4;
            out.type = JsonValue::Bool;
            out.boolVal = true;
            return true;
        }
        return false;
    case 'f':
        if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
            p += 5;
            out.type = JsonValue::Bool;
            out.boolVal = false;
            return true;
        }
        return false;
    case 'n':
        if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
            p += 4;
            out.type = JsonValue::Null;
            return true;
        }
        return false;
    default:
        return parseNumber(out);
    }
}

} // namespace

bool jsonParse(const std::string &text, JsonValue &out) {
    Parser parser(text);
    JsonValue parsed;
    if (!parser.parseValue(parsed)) {
        out = JsonValue();
        return false;
    }
    parser.skipWs();
    // Trailing content after the top-level value is tolerated: the Named Pipe
    // client may pad its message buffer with NULs.
    out = parsed;
    return true;
}

std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
            break;
        }
    }
    return out;
}
