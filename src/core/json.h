// core/json.h — a minimal JSON reader.
//
// Deliberately small: enough to load data/chemistry.json and nothing more. It
// exists because the chemistry data has to be editable by hand, and a hand
// editable file needs a real parser that reports WHERE it broke rather than
// silently producing nonsense.
//
// Not a general-purpose library: no writing, no comments, no streaming. It
// parses a whole file into a tree of values and gives typed accessors with
// defaults, so a missing optional field is not an error.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gen {

class JsonValue {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;

    bool isNull()   const { return type == Type::Null; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    // Object lookup. Returns a null value if absent, so chaining is safe.
    const JsonValue& operator[](const char* key) const;
    const JsonValue& operator[](size_t index) const;
    size_t size() const { return isArray() ? items.size() : fields.size(); }

    double      asNumber(double fallback = 0.0) const;
    std::string asString(const std::string& fallback = std::string()) const;
    bool        asBool(bool fallback = false) const;
};

// Parses `source`. On failure returns false and fills `error` with a message
// including the line number.
bool parseJson(const std::string& source, JsonValue& out, std::string& error);
bool loadJsonFile(const std::string& path, JsonValue& out, std::string& error);

}  // namespace gen
