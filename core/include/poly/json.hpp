// A minimal read-only JSON parser, and a tiny writer.
//
// Not a third-party library, for the reason given in core/tests/testing.hpp: every
// dependency is a thing to keep patched, to keep consistent across native and WASM, and to
// make build under Emscripten later. This reads rule data at startup and serialises state
// views for the UI -- neither is on a hot path, so simplicity beats speed everywhere here.
//
// Scope: enough JSON for our own data files. UTF-8 passes through byte-wise; \u escapes are
// not decoded. Rejects malformed input with a message rather than asserting.

#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace poly::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(double d) : type_(Type::Number), number_(d) {}
  Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
  Value(Array a) : type_(Type::Array), array_(std::move(a)) {}
  Value(Object o) : type_(Type::Object), object_(std::move(o)) {}

  [[nodiscard]] Type type() const { return type_; }
  [[nodiscard]] bool is_null() const { return type_ == Type::Null; }
  [[nodiscard]] bool is_object() const { return type_ == Type::Object; }
  [[nodiscard]] bool is_array() const { return type_ == Type::Array; }
  [[nodiscard]] bool is_number() const { return type_ == Type::Number; }
  [[nodiscard]] bool is_string() const { return type_ == Type::String; }

  // Accessors are TOTAL: a missing key or wrong type yields the fallback rather than
  // throwing. Rule data is allowed to be incomplete -- unknown values are legitimately null
  // (see data/rules "_unverified") -- so a missing key must be a recoverable default, not a
  // crash on startup.
  [[nodiscard]] double num(double fallback = 0.0) const {
    return type_ == Type::Number ? number_ : fallback;
  }
  [[nodiscard]] int integer(int fallback = 0) const {
    return type_ == Type::Number ? static_cast<int>(number_) : fallback;
  }
  [[nodiscard]] bool boolean(bool fallback = false) const {
    return type_ == Type::Bool ? bool_ : fallback;
  }
  [[nodiscard]] const std::string& str() const {
    static const std::string kEmpty;
    return type_ == Type::String ? string_ : kEmpty;
  }
  [[nodiscard]] const Array& arr() const {
    static const Array kEmpty;
    return type_ == Type::Array ? array_ : kEmpty;
  }
  [[nodiscard]] const Object& obj() const {
    static const Object kEmpty;
    return type_ == Type::Object ? object_ : kEmpty;
  }

  // Member lookup. Returns a null Value when absent, so chains like
  // root["units"]["warrior"]["cost"].integer(2) are safe on partial data.
  const Value& operator[](std::string_view key) const {
    static const Value kNull;
    if (type_ != Type::Object) return kNull;
    const auto it = object_.find(std::string(key));
    return it == object_.end() ? kNull : it->second;
  }

  [[nodiscard]] bool contains(std::string_view key) const {
    return type_ == Type::Object && object_.count(std::string(key)) != 0;
  }

 private:
  Type type_{Type::Null};
  bool bool_{false};
  double number_{0.0};
  std::string string_;
  Array array_;
  Object object_;
};

// Parses `text`. On failure returns false and fills `error`.
bool parse(std::string_view text, Value& out, std::string& error);

// Reads and parses a file.
bool parse_file(const std::string& path, Value& out, std::string& error);

// ---------------------------------------------------------------------------------------
// Writer. Streaming and append-only -- the state view is built once per render and thrown
// away, so there is no reason to materialise a tree first.
// ---------------------------------------------------------------------------------------
// Separation is tracked with an explicit container stack rather than by inspecting the last
// character written. Character-sniffing looks simpler but gets empty containers wrong -- an
// empty object nested in an array emits `[{},]` because `}` reads as "something was written"
// at the wrong level. The stack costs one bool per depth and cannot be fooled.
class Writer {
 public:
  Writer() { stack_.reserve(16); }

  void begin_object() { prefix(); out_ += '{'; stack_.push_back(true); }
  void end_object() { stack_.pop_back(); out_ += '}'; }
  void begin_array() { prefix(); out_ += '['; stack_.push_back(true); }
  void end_array() { stack_.pop_back(); out_ += ']'; }

  void key(std::string_view k) {
    prefix();
    out_ += '"';
    escape(k);
    out_ += "\":";
    expecting_value_ = true;
  }

  void value(std::string_view s) { prefix(); out_ += '"'; escape(s); out_ += '"'; }
  void value(const char* s) { value(std::string_view(s)); }
  void value(bool b) { prefix(); out_ += b ? "true" : "false"; }
  void value(std::nullptr_t) { prefix(); out_ += "null"; }
  void value(int v) { prefix(); out_ += std::to_string(v); }
  void value(double v);

  template <typename T>
  void field(std::string_view k, T v) {
    key(k);
    value(v);
  }

  [[nodiscard]] const std::string& text() const { return out_; }
  void clear() {
    out_.clear();
    stack_.clear();
    expecting_value_ = false;
  }

 private:
  // Emits a comma if this is not the first entry in the current container. A value that
  // directly follows a key is never separated -- it belongs to that key.
  void prefix() {
    if (expecting_value_) {
      expecting_value_ = false;
      return;
    }
    if (stack_.empty()) return;
    if (!stack_.back()) out_ += ',';
    stack_.back() = false;
  }

  void escape(std::string_view s);

  std::string out_;
  std::vector<bool> stack_;  // one entry per open container; true = nothing written yet
  bool expecting_value_{false};
};

}  // namespace poly::json
