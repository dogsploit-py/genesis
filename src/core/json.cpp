#include "core/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gen {

namespace {
const JsonValue kNull;

struct Parser {
    const char* p = nullptr;
    const char* end = nullptr;
    int line = 1;
    std::string error;

    bool fail(const char* msg) {
        if (error.empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "line %d: %s", line, msg);
            error = buf;
        }
        return false;
    }

    void skipSpace() {
        while (p < end) {
            const char c = *p;
            if (c == '\n') { ++line; ++p; }
            else if (c == ' ' || c == '\t' || c == '\r') ++p;
            else if (c == '/' && p + 1 < end && p[1] == '/') {
                // Line comments are not standard JSON, but this file is meant to
                // be edited by hand and a data file you cannot annotate is a
                // data file nobody will maintain.
                while (p < end && *p != '\n') ++p;
            } else if (c == '/' && p + 1 < end && p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(*p == '*' && p[1] == '/')) {
                    if (*p == '\n') ++line;
                    ++p;
                }
                p = (p + 1 < end) ? p + 2 : end;
            } else {
                break;
            }
        }
    }

    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return fail("expected a string");
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) return fail("unterminated escape");
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u': {
                        if (p + 4 >= end) return fail("truncated \\u escape");
                        char hex[5] = {p[1], p[2], p[3], p[4], 0};
                        const unsigned cp = static_cast<unsigned>(std::strtoul(hex, nullptr, 16));
                        // UTF-8 encode. The data files are ASCII in practice,
                        // but a stray escape should not corrupt the string.
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        p += 4;
                        break;
                    }
                    default: return fail("unknown escape");
                }
                ++p;
            } else {
                if (*p == '\n') ++line;
                out += *p++;
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }

    bool parseValue(JsonValue& v) {
        skipSpace();
        if (p >= end) return fail("unexpected end of input");

        switch (*p) {
            case '{': {
                ++p;
                v.type = JsonValue::Type::Object;
                skipSpace();
                if (p < end && *p == '}') { ++p; return true; }
                for (;;) {
                    skipSpace();
                    std::string key;
                    if (!parseString(key)) return false;
                    skipSpace();
                    if (p >= end || *p != ':') return fail("expected ':'");
                    ++p;
                    JsonValue child;
                    if (!parseValue(child)) return false;
                    v.fields.emplace_back(std::move(key), std::move(child));
                    skipSpace();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; return true; }
                    return fail("expected ',' or '}'");
                }
            }
            case '[': {
                ++p;
                v.type = JsonValue::Type::Array;
                skipSpace();
                if (p < end && *p == ']') { ++p; return true; }
                for (;;) {
                    JsonValue child;
                    if (!parseValue(child)) return false;
                    v.items.push_back(std::move(child));
                    skipSpace();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; return true; }
                    return fail("expected ',' or ']'");
                }
            }
            case '"':
                v.type = JsonValue::Type::String;
                return parseString(v.text);
            case 't':
                if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
                    p += 4;
                    v.type = JsonValue::Type::Bool;
                    v.boolean = true;
                    return true;
                }
                return fail("expected 'true'");
            case 'f':
                if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
                    p += 5;
                    v.type = JsonValue::Type::Bool;
                    v.boolean = false;
                    return true;
                }
                return fail("expected 'false'");
            case 'n':
                if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
                    p += 4;
                    v.type = JsonValue::Type::Null;
                    return true;
                }
                return fail("expected 'null'");
            default: {
                char* stop = nullptr;
                const double d = std::strtod(p, &stop);
                if (stop == p) return fail("expected a value");
                p = stop;
                v.type = JsonValue::Type::Number;
                v.number = d;
                return true;
            }
        }
    }
};
}  // namespace

const JsonValue& JsonValue::operator[](const char* key) const {
    if (type != Type::Object) return kNull;
    for (const auto& kv : fields)
        if (kv.first == key) return kv.second;
    return kNull;
}

const JsonValue& JsonValue::operator[](size_t index) const {
    if (type != Type::Array || index >= items.size()) return kNull;
    return items[index];
}

double JsonValue::asNumber(double fallback) const {
    return (type == Type::Number) ? number : fallback;
}

std::string JsonValue::asString(const std::string& fallback) const {
    return (type == Type::String) ? text : fallback;
}

bool JsonValue::asBool(bool fallback) const {
    return (type == Type::Bool) ? boolean : fallback;
}

bool parseJson(const std::string& source, JsonValue& out, std::string& error) {
    Parser parser;
    parser.p = source.c_str();
    parser.end = parser.p + source.size();
    out = JsonValue();
    if (!parser.parseValue(out)) {
        error = parser.error;
        return false;
    }
    parser.skipSpace();
    if (parser.p != parser.end) {
        error = "trailing content after the top-level value";
        return false;
    }
    return true;
}

bool loadJsonFile(const std::string& path, JsonValue& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    std::string source;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) source.append(buf, n);
    std::fclose(f);
    if (!parseJson(source, out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

}  // namespace gen
