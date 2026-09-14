#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace wm::core::json {

class Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

/// Minimal JSON value: null / bool / number / string / array / object.
class Value {
public:
    using Storage = std::variant<std::monostate, bool, double, std::string, Array, Object>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : storage_(v) {}
    Value(double v) : storage_(v) {}
    Value(int v) : storage_(static_cast<double>(v)) {}
    Value(std::int64_t v) : storage_(static_cast<double>(v)) {}
    Value(std::uint64_t v) : storage_(static_cast<double>(v)) {}
    Value(std::string v) : storage_(std::move(v)) {}
    Value(const char* v) : storage_(std::string(v ? v : "")) {}
    Value(Array v) : storage_(std::move(v)) {}
    Value(Object v) : storage_(std::move(v)) {}

    bool isNull() const noexcept { return std::holds_alternative<std::monostate>(storage_); }
    bool isBool() const noexcept { return std::holds_alternative<bool>(storage_); }
    bool isNumber() const noexcept { return std::holds_alternative<double>(storage_); }
    bool isString() const noexcept { return std::holds_alternative<std::string>(storage_); }
    bool isArray() const noexcept { return std::holds_alternative<Array>(storage_); }
    bool isObject() const noexcept { return std::holds_alternative<Object>(storage_); }

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    std::int64_t asInt(std::int64_t fallback = 0) const;
    std::string asString(const std::string& fallback = {}) const;
    const Array& asArray() const;
    const Object& asObject() const;

    /// Object member access; returns nullptr when missing or not an object.
    const Value* Find(const std::string& key) const;
    Value& operator[](const std::string& key);
    void Push(Value v);

private:
    Storage storage_;
};

/// Parses JSON text. Returns nullopt and fills |error| on failure.
std::optional<Value> Parse(const std::string& text, std::string* error = nullptr);

/// Serializes to JSON text. |pretty| uses 2-space indentation.
std::string Serialize(const Value& value, bool pretty = true);

} // namespace wm::core::json
