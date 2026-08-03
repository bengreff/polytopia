#include "poly/json.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace poly::json {
namespace {

class Parser {
 public:
  Parser(std::string_view text, std::string& error) : s_(text), error_(error) {}

  bool run(Value& out) {
    skip_ws();
    if (!parse_value(out)) return false;
    skip_ws();
    if (pos_ != s_.size()) return fail("trailing content after top-level value");
    return true;
  }

 private:
  bool fail(const char* what) {
    // Line number rather than byte offset: rule files are hand-edited, and "line 214" is
    // actionable in a way that "byte 8823" is not.
    int line = 1;
    for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
      if (s_[i] == '\n') ++line;
    }
    error_ = "line " + std::to_string(line) + ": " + what;
    return false;
  }

  void skip_ws() {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool eat(char c) {
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool literal(std::string_view word) {
    if (s_.compare(pos_, word.size(), word) == 0) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  bool parse_value(Value& out) {
    if (depth_ > 64) return fail("nesting too deep");
    if (pos_ >= s_.size()) return fail("unexpected end of input");

    switch (s_[pos_]) {
      case '{': return parse_object(out);
      case '[': return parse_array(out);
      case '"': {
        std::string str;
        if (!parse_string(str)) return false;
        out = Value(std::move(str));
        return true;
      }
      case 't':
        if (!literal("true")) return fail("invalid literal");
        out = Value(true);
        return true;
      case 'f':
        if (!literal("false")) return fail("invalid literal");
        out = Value(false);
        return true;
      case 'n':
        if (!literal("null")) return fail("invalid literal");
        out = Value();
        return true;
      default: return parse_number(out);
    }
  }

  bool parse_object(Value& out) {
    ++depth_;
    ++pos_;  // '{'
    Object object;
    skip_ws();
    if (eat('}')) {
      out = Value(std::move(object));
      --depth_;
      return true;
    }
    for (;;) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (!eat(':')) return fail("expected ':' after object key");
      skip_ws();
      Value member;
      if (!parse_value(member)) return false;
      object.emplace(std::move(key), std::move(member));
      skip_ws();
      if (eat(',')) continue;
      if (eat('}')) break;
      return fail("expected ',' or '}' in object");
    }
    out = Value(std::move(object));
    --depth_;
    return true;
  }

  bool parse_array(Value& out) {
    ++depth_;
    ++pos_;  // '['
    Array array;
    skip_ws();
    if (eat(']')) {
      out = Value(std::move(array));
      --depth_;
      return true;
    }
    for (;;) {
      skip_ws();
      Value element;
      if (!parse_value(element)) return false;
      array.push_back(std::move(element));
      skip_ws();
      if (eat(',')) continue;
      if (eat(']')) break;
      return fail("expected ',' or ']' in array");
    }
    out = Value(std::move(array));
    --depth_;
    return true;
  }

  bool parse_string(std::string& out) {
    if (!eat('"')) return fail("expected string");
    out.clear();
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return fail("unterminated escape");
      const char esc = s_[pos_++];
      switch (esc) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
          // Not decoded. Our own data files are plain ASCII; a \u here means someone pasted
          // wiki text in, and silently mangling it would be worse than saying so.
          return fail("\\u escapes are not supported in rule data");
        default: return fail("invalid escape character");
      }
    }
    return fail("unterminated string");
  }

  bool parse_number(Value& out) {
    const std::size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) return fail("expected a value");

    const std::string text(s_.substr(start, pos_ - start));
    try {
      std::size_t consumed = 0;
      const double parsed = std::stod(text, &consumed);
      if (consumed != text.size()) return fail("malformed number");
      out = Value(parsed);
      return true;
    } catch (...) {
      return fail("malformed number");
    }
  }

  std::string_view s_;
  std::string& error_;
  std::size_t pos_{0};
  int depth_{0};
};

}  // namespace

bool parse(std::string_view text, Value& out, std::string& error) {
  error.clear();
  Parser parser(text, error);
  return parser.run(out);
}

bool parse_file(const std::string& path, Value& out, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "could not open " + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  if (!parse(text, out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

void Writer::escape(std::string_view s) {
  for (const char c : s) {
    switch (c) {
      case '"': out_ += "\\\""; break;
      case '\\': out_ += "\\\\"; break;
      case '\n': out_ += "\\n"; break;
      case '\r': out_ += "\\r"; break;
      case '\t': out_ += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out_ += buf;
        } else {
          out_ += c;
        }
    }
  }
}

void Writer::value(double v) {
  prefix();
  if (!std::isfinite(v)) {
    // JSON has no NaN or Infinity. Emitting them produces a document the browser's own
    // JSON.parse rejects, which surfaces as an unexplained blank screen.
    out_ += "null";
    return;
  }
  if (v == static_cast<double>(static_cast<long long>(v))) {
    out_ += std::to_string(static_cast<long long>(v));
    return;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  out_ += buf;
}

}  // namespace poly::json
