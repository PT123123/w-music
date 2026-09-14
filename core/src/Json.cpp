#include "wm/core/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wm::core::json {
namespace {

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    bool Fail(const char* msg) {
        if (err.empty()) {
            err = msg;
            err += " at offset ";
            err += std::to_string(i);
        }
        return false;
    }

    void SkipWs() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
            } else {
                break;
            }
        }
    }

    bool Peek(char c) const { return i < s.size() && s[i] == c; }

    bool Expect(char c) {
        if (!Peek(c)) {
            return Fail("unexpected character");
        }
        ++i;
        return true;
    }

    bool Literal(const char* word) {
        const std::size_t n = std::string(word).size();
        if (s.compare(i, n, word) != 0) {
            return Fail("invalid literal");
        }
        i += n;
        return true;
    }

    bool Hex4(std::uint32_t& out) {
        if (i + 4 > s.size()) {
            return Fail("truncated \\u escape");
        }
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return Fail("bad hex digit");
        }
        i += 4;
        out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        if (!Expect('"')) return false;
        out.clear();
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return Fail("truncated escape");
                const char e = s[i++];
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
                        std::uint32_t cp = 0;
                        if (!Hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            const std::size_t save = i;
                            i += 2;
                            std::uint32_t low = 0;
                            if (!Hex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                i = save;
                            }
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: return Fail("unknown escape");
                }
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(double& out) {
        const std::size_t start = i;
        if (Peek('-') || Peek('+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        if (i == start) return Fail("expected number");
        out = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    bool ParseValue(Value& out);

    bool ParseArray(Value& out) {
        if (!Expect('[')) return false;
        Array arr;
        SkipWs();
        if (Peek(']')) {
            ++i;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            Value v;
            if (!ParseValue(v)) return false;
            arr.push_back(std::move(v));
            SkipWs();
            if (Peek(',')) { ++i; continue; }
            if (Peek(']')) { ++i; break; }
            return Fail("expected ',' or ']'");
        }
        out = Value(std::move(arr));
        return true;
    }

    bool ParseObject(Value& out) {
        if (!Expect('{')) return false;
        Object obj;
        SkipWs();
        if (Peek('}')) {
            ++i;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (!Expect(':')) return false;
            Value v;
            if (!ParseValue(v)) return false;
            obj[key] = std::move(v);
            SkipWs();
            if (Peek(',')) { ++i; continue; }
            if (Peek('}')) { ++i; break; }
            return Fail("expected ',' or '}'");
        }
        out = Value(std::move(obj));
        return true;
    }
};

bool Parser::ParseValue(Value& out) {
    SkipWs();
    if (i >= s.size()) return Fail("unexpected end of input");

    const char c = s[i];
    switch (c) {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"': {
            std::string str;
            if (!ParseString(str)) return false;
            out = Value(std::move(str));
            return true;
        }
        case 't':
            if (!Literal("true")) return false;
            out = Value(true);
            return true;
        case 'f':
            if (!Literal("false")) return false;
            out = Value(false);
            return true;
        case 'n':
            if (!Literal("null")) return false;
            out = Value();
            return true;
        default: {
            double d = 0;
            if (!ParseNumber(d)) return false;
            out = Value(d);
            return true;
        }
    }
}

void EscapeString(const std::string& in, std::string& out) {
    out.push_back('"');
    for (const char ch : in) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void SerializeInto(const Value& v, std::string& out, bool pretty, int indent);

void SerializeObject(const Object& obj, std::string& out, bool pretty, int indent) {
    if (obj.empty()) {
        out += "{}";
        return;
    }
    out += '{';
    bool first = true;
    for (const auto& [key, value] : obj) {
        if (!first) out += ',';
        first = false;
        if (pretty) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent + 1) * 2, ' ');
        }
        EscapeString(key, out);
        out += ':';
        if (pretty) out += ' ';
        SerializeInto(value, out, pretty, indent + 1);
    }
    if (pretty) {
        out += '\n';
        out.append(static_cast<std::size_t>(indent) * 2, ' ');
    }
    out += '}';
}

void SerializeArray(const Array& arr, std::string& out, bool pretty, int indent) {
    if (arr.empty()) {
        out += "[]";
        return;
    }
    out += '[';
    for (std::size_t idx = 0; idx < arr.size(); ++idx) {
        if (idx != 0) out += ',';
        if (pretty) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent + 1) * 2, ' ');
        }
        SerializeInto(arr[idx], out, pretty, indent + 1);
    }
    if (pretty) {
        out += '\n';
        out.append(static_cast<std::size_t>(indent) * 2, ' ');
    }
    out += ']';
}

void SerializeInto(const Value& v, std::string& out, bool pretty, int indent) {
    if (v.isNull()) {
        out += "null";
    } else if (v.isBool()) {
        out += v.asBool() ? "true" : "false";
    } else if (v.isNumber()) {
        const double d = v.asNumber();
        if (d == std::floor(d) && std::fabs(d) < 9.0e15) {
            out += std::to_string(static_cast<std::int64_t>(d));
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            out += buf;
        }
    } else if (v.isString()) {
        EscapeString(v.asString(), out);
    } else if (v.isArray()) {
        SerializeArray(v.asArray(), out, pretty, indent);
    } else if (v.isObject()) {
        SerializeObject(v.asObject(), out, pretty, indent);
    }
}

} // namespace

bool Value::asBool(bool fallback) const {
    if (const auto* p = std::get_if<bool>(&storage_)) return *p;
    if (const auto* p = std::get_if<double>(&storage_)) return *p != 0.0;
    if (const auto* p = std::get_if<std::string>(&storage_)) return *p == "true";
    return fallback;
}

double Value::asNumber(double fallback) const {
    if (const auto* p = std::get_if<double>(&storage_)) return *p;
    if (const auto* p = std::get_if<bool>(&storage_)) return *p ? 1.0 : 0.0;
    if (const auto* p = std::get_if<std::string>(&storage_)) {
        try {
            return std::stod(*p);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::int64_t Value::asInt(std::int64_t fallback) const {
    if (const auto* p = std::get_if<double>(&storage_)) return static_cast<std::int64_t>(*p);
    if (const auto* p = std::get_if<bool>(&storage_)) return *p ? 1 : 0;
    if (const auto* p = std::get_if<std::string>(&storage_)) {
        try {
            return static_cast<std::int64_t>(std::stoll(*p));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string Value::asString(const std::string& fallback) const {
    if (const auto* p = std::get_if<std::string>(&storage_)) return *p;
    if (const auto* p = std::get_if<double>(&storage_)) return std::to_string(*p);
    if (const auto* p = std::get_if<bool>(&storage_)) return *p ? "true" : "false";
    return fallback;
}

const Array& Value::asArray() const {
    static const Array kEmpty;
    if (const auto* p = std::get_if<Array>(&storage_)) return *p;
    return kEmpty;
}

const Object& Value::asObject() const {
    static const Object kEmpty;
    if (const auto* p = std::get_if<Object>(&storage_)) return *p;
    return kEmpty;
}

const Value* Value::Find(const std::string& key) const {
    if (const auto* p = std::get_if<Object>(&storage_)) {
        const auto it = p->find(key);
        if (it != p->end()) return &it->second;
    }
    return nullptr;
}

Value& Value::operator[](const std::string& key) {
    if (!std::holds_alternative<Object>(storage_)) {
        storage_ = Object{};
    }
    return std::get<Object>(storage_)[key];
}

void Value::Push(Value v) {
    if (!std::holds_alternative<Array>(storage_)) {
        storage_ = Array{};
    }
    std::get<Array>(storage_).push_back(std::move(v));
}

std::optional<Value> Parse(const std::string& text, std::string* error) {
    Parser parser(text);
    Value root;
    if (!parser.ParseValue(root)) {
        if (error) *error = parser.err;
        return std::nullopt;
    }
    parser.SkipWs();
    if (parser.i != text.size()) {
        if (error) *error = "trailing content at offset " + std::to_string(parser.i);
        return std::nullopt;
    }
    return root;
}

std::string Serialize(const Value& value, bool pretty) {
    std::string out;
    SerializeInto(value, out, pretty, 0);
    return out;
}

} // namespace wm::core::json
