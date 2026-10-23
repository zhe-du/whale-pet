// ============================================================================
// json.hpp —— 极简 JSON 解析 / 序列化（C++17，无第三方依赖）
//
// 为什么自己写：本机 MinGW 环境没有 nlohmann/json，GitHub 又不通；
// 而这里只需要「解析 JSONC 配置、序列化 /config 响应、读写 DSH 配置」这几件事，
// 一个三百来行的递归下降解析器足够，也避免引入外部单头文件带来的版本风险。
//
// 特性：
//   - 容忍尾随逗号（JSONC 配置文件常见）
//   - 字符串按 UTF-8 原样保留，\uXXXX（含代理对）转 UTF-8
//   - 数字输出按「整数值就用整数格式」处理，避免 size 变成 440.000000
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace js {

struct Value;

struct Member {
  std::string key;
  std::shared_ptr<Value> value;
  Member() = default;
  Member(std::string k, std::shared_ptr<Value> v) : key(std::move(k)), value(std::move(v)) {}
};

enum class Kind { Null, Bool, Num, Str, Arr, Obj };

struct Value {
  Kind kind = Kind::Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<std::shared_ptr<Value>> arr;
  std::vector<Member> obj;

  static std::shared_ptr<Value> null() { return std::make_shared<Value>(); }
  static std::shared_ptr<Value> boolean(bool v) {
    auto p = std::make_shared<Value>();
    p->kind = Kind::Bool;
    p->b = v;
    return p;
  }
  static std::shared_ptr<Value> number(double v) {
    auto p = std::make_shared<Value>();
    p->kind = Kind::Num;
    p->num = v;
    return p;
  }
  static std::shared_ptr<Value> string(std::string v) {
    auto p = std::make_shared<Value>();
    p->kind = Kind::Str;
    p->str = std::move(v);
    return p;
  }
  static std::shared_ptr<Value> array() {
    auto p = std::make_shared<Value>();
    p->kind = Kind::Arr;
    return p;
  }
  static std::shared_ptr<Value> object() {
    auto p = std::make_shared<Value>();
    p->kind = Kind::Obj;
    return p;
  }

  bool isNull() const { return kind == Kind::Null; }
  bool isBool() const { return kind == Kind::Bool; }
  bool isNum() const { return kind == Kind::Num; }
  bool isStr() const { return kind == Kind::Str; }
  bool isArr() const { return kind == Kind::Arr; }
  bool isObj() const { return kind == Kind::Obj; }

  const Value* get(const std::string& key) const {
    if (kind != Kind::Obj) return nullptr;
    for (const auto& m : obj) {
      if (m.key == key) return m.value.get();
    }
    return nullptr;
  }
  Value* get(const std::string& key) {
    if (kind != Kind::Obj) return nullptr;
    for (auto& m : obj) {
      if (m.key == key) return m.value.get();
    }
    return nullptr;
  }
  void set(const std::string& key, std::shared_ptr<Value> v) {
    if (kind != Kind::Obj) {
      kind = Kind::Obj;
      arr.clear();
    }
    for (auto& m : obj) {
      if (m.key == key) {
        m.value = std::move(v);
        return;
      }
    }
    obj.emplace_back(key, std::move(v));
  }
  void push(std::shared_ptr<Value> v) {
    if (kind != Kind::Arr) {
      kind = Kind::Arr;
      obj.clear();
    }
    arr.push_back(std::move(v));
  }

  size_t size() const {
    if (kind == Kind::Arr) return arr.size();
    if (kind == Kind::Obj) return obj.size();
    return 0;
  }

  std::string asString(const std::string& def = "") const { return kind == Kind::Str ? str : def; }
  double asNum(double def = 0) const { return kind == Kind::Num ? num : def; }
  bool asBool(bool def = false) const { return kind == Kind::Bool ? b : def; }
  long long asInt(long long def = 0) const {
    return kind == Kind::Num && std::isfinite(num) ? static_cast<long long>(num) : def;
  }
};

namespace detail {

inline void appendUtf8(std::string& out, unsigned int cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace detail

class Parser {
 public:
  explicit Parser(std::string text) : src_(std::move(text)) {}

  std::shared_ptr<Value> parse() {
    skipWs();
    auto v = parseValue();
    if (ok_) {
      skipWs();
    }
    return v;
  }

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

 private:
  std::string src_;
  size_t pos_ = 0;
  bool ok_ = true;
  std::string error_;

  void fail(const std::string& message) {
    if (!ok_) return;
    ok_ = false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (offset %zu)", pos_);
    error_ = message + buf;
  }

  void skipWs() {
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
        // JSONC 行注释（配置文件里很常见）
        pos_ += 2;
        while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
      } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) ++pos_;
        pos_ = pos_ + 2 <= src_.size() ? pos_ + 2 : src_.size();
      } else {
        break;
      }
    }
  }

  char peek() { return pos_ < src_.size() ? src_[pos_] : '\0'; }

  bool consume(char c) {
    if (peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char c) {
    if (!consume(c)) fail(std::string("expected '") + c + "'");
  }

  std::shared_ptr<Value> parseValue() {
    skipWs();
    switch (peek()) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return Value::string(parseString());
      case 't':
        if (src_.compare(pos_, 4, "true") == 0) { pos_ += 4; return Value::boolean(true); }
        fail("bad literal");
        return Value::null();
      case 'f':
        if (src_.compare(pos_, 5, "false") == 0) { pos_ += 5; return Value::boolean(false); }
        fail("bad literal");
        return Value::null();
      case 'n':
        if (src_.compare(pos_, 4, "null") == 0) { pos_ += 4; return Value::null(); }
        fail("bad literal");
        return Value::null();
      default: return parseNumber();
    }
  }

  std::shared_ptr<Value> parseObject() {
    auto obj = Value::object();
    expect('{');
    skipWs();
    if (consume('}')) return obj;
    while (ok_) {
      skipWs();
      if (peek() != '"') {
        fail("expected object key");
        break;
      }
      std::string key = parseString();
      skipWs();
      expect(':');
      auto val = parseValue();
      if (!ok_) break;
      obj->obj.emplace_back(std::move(key), std::move(val));
      skipWs();
      if (consume(',')) {
        skipWs();
        if (consume('}')) break;  // 容忍尾随逗号
        continue;
      }
      if (consume('}')) break;
      fail("expected ',' or '}'");
      break;
    }
    return obj;
  }

  std::shared_ptr<Value> parseArray() {
    auto arr = Value::array();
    expect('[');
    skipWs();
    if (consume(']')) return arr;
    while (ok_) {
      auto val = parseValue();
      if (!ok_) break;
      arr->arr.push_back(std::move(val));
      skipWs();
      if (consume(',')) {
        skipWs();
        if (consume(']')) break;  // 容忍尾随逗号
        continue;
      }
      if (consume(']')) break;
      fail("expected ',' or ']'");
      break;
    }
    return arr;
  }

  std::string parseString() {
    std::string out;
    expect('"');
    while (ok_ && pos_ < src_.size()) {
      const unsigned char c = static_cast<unsigned char>(src_[pos_]);
      if (c == '"') {
        ++pos_;
        return out;
      }
      if (c == '\\') {
        ++pos_;
        const char e = peek();
        ++pos_;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            unsigned int cp = parseHex4();
            if (!ok_) return out;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // 代理对：紧跟 \uDC00-\uDFFF
              if (pos_ + 1 < src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                pos_ += 2;
                const unsigned int low = parseHex4();
                if (ok_ && low >= 0xDC00 && low <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
              }
            }
            detail::appendUtf8(out, cp);
            break;
          }
          default: fail("bad escape"); return out;
        }
        continue;
      }
      out += static_cast<char>(c);
      ++pos_;
    }
    fail("unterminated string");
    return out;
  }

  unsigned int parseHex4() {
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = peek();
      ++pos_;
      int digit = -1;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      if (digit < 0) {
        fail("bad \\u escape");
        return 0;
      }
      value = (value << 4) | static_cast<unsigned int>(digit);
    }
    return value;
  }

  std::shared_ptr<Value> parseNumber() {
    const size_t start = pos_;
    if (peek() == '-' || peek() == '+') ++pos_;
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        ++pos_;
      } else {
        break;
      }
    }
    if (start == pos_) {
      fail("unexpected character");
      return Value::null();
    }
    const std::string text = src_.substr(start, pos_ - start);
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
      fail("bad number");
      return Value::null();
    }
    return Value::number(value);
  }
};

inline std::shared_ptr<Value> parse(const std::string& text, std::string* error = nullptr) {
  Parser parser(text);
  auto value = parser.parse();
  if (!parser.ok() && error) *error = parser.error();
  return value;
}

namespace detail {

inline void escapeString(std::string& out, const std::string& s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

inline void writeNum(std::string& out, double v) {
  if (!std::isfinite(v)) {
    out += '0';
    return;
  }
  // 整数值直接用整数格式：尺寸/边距这些字段不该出现 440.000000
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    out += buf;
    return;
  }
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  out += buf;
}

inline void stringify(const Value& v, std::string& out, int indent, int level) {
  const bool pretty = indent >= 0;
  auto newline = [&](int lvl) {
    if (!pretty) return;
    out += '\n';
    out.append(static_cast<size_t>(indent * lvl), ' ');
  };
  switch (v.kind) {
    case Kind::Null: out += "null"; break;
    case Kind::Bool: out += v.b ? "true" : "false"; break;
    case Kind::Num: writeNum(out, v.num); break;
    case Kind::Str: escapeString(out, v.str); break;
    case Kind::Arr: {
      if (v.arr.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i) out += ',';
        newline(level + 1);
        stringify(*v.arr[i], out, indent, level + 1);
      }
      newline(level);
      out += ']';
      break;
    }
    case Kind::Obj: {
      if (v.obj.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i) out += ',';
        newline(level + 1);
        escapeString(out, v.obj[i].key);
        out += ':';
        if (pretty) out += ' ';
        stringify(*v.obj[i].value, out, indent, level + 1);
      }
      newline(level);
      out += '}';
      break;
    }
  }
}

}  // namespace detail

inline std::string stringify(const Value& v, int indent = -1) {
  std::string out;
  detail::stringify(v, out, indent, 0);
  return out;
}

}  // namespace js
