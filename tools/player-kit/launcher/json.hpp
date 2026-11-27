// A small JSON reader for the launcher: enough for GitHub's releases list.
// Portable (no platform headers), tested by launcher_test.cpp.
//
// The whole text is parsed into a tree. Numbers are doubles; strings are
// UTF-8 (\u escapes, surrogate pairs included, are decoded). Anything
// malformed is an error with its byte offset, never a guess.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::launcher::json {

struct Member;

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;     // an array's
    std::vector<Member> members;  // an object's, in the text's order

    bool is_null() const { return kind == Kind::Null; }
    bool is_bool() const { return kind == Kind::Bool; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_string() const { return kind == Kind::String; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_object() const { return kind == Kind::Object; }

    // An object's member by name (the first, if repeated); nullptr if there
    // is none or this isn't an object.
    const Value* find(std::string_view key) const;
    // A member's string or boolean, or the fallback if it's missing or of
    // another kind.
    std::string_view string_or(std::string_view key, std::string_view fallback) const;
    bool bool_or(std::string_view key, bool fallback) const;
};

struct Member {
    std::string key;
    Value value;
};

inline const Value* Value::find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const Member& m : members)
        if (m.key == key) return &m.value;
    return nullptr;
}

inline std::string_view Value::string_or(std::string_view key, std::string_view fallback) const {
    const Value* v = find(key);
    return v && v->is_string() ? std::string_view(v->string) : fallback;
}

inline bool Value::bool_or(std::string_view key, bool fallback) const {
    const Value* v = find(key);
    return v && v->is_bool() ? v->boolean : fallback;
}

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::expected<Value, std::string> document() {
        Value v;
        if (!value(v, 0)) return std::unexpected(error_);
        space();
        if (at_ < s_.size()) return std::unexpected(fail("text after the value"));
        return v;
    }

private:
    static constexpr int kMaxDepth = 200;

    std::string_view s_;
    std::size_t at_ = 0;
    std::string error_;

    std::string fail(std::string_view what) {
        error_ = "JSON: " + std::string(what) + " at byte " + std::to_string(at_);
        return error_;
    }
    bool stop(std::string_view what) {
        fail(what);
        return false;
    }

    void space() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
    }

    bool literal(std::string_view word) {
        if (s_.substr(at_, word.size()) != word) return stop("an unknown word");
        at_ += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return stop("nesting too deep");
        space();
        if (at_ >= s_.size()) return stop("the text ends where a value should be");
        const char c = s_[at_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.string);
        }
        if (c == 't') {
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.kind = Value::Kind::Bool;
            out.boolean = false;
            return literal("false");
        }
        if (c == 'n') {
            out.kind = Value::Kind::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return stop("an unexpected character");
    }

    bool object(Value& out, int depth) {
        out.kind = Value::Kind::Object;
        ++at_;  // {
        space();
        if (at_ < s_.size() && s_[at_] == '}') {
            ++at_;
            return true;
        }
        for (;;) {
            space();
            if (at_ >= s_.size() || s_[at_] != '"') return stop("an object's key isn't a string");
            Member m;
            if (!string(m.key)) return false;
            space();
            if (at_ >= s_.size() || s_[at_] != ':') return stop("no ':' after an object's key");
            ++at_;
            if (!value(m.value, depth + 1)) return false;
            out.members.push_back(std::move(m));
            space();
            if (at_ >= s_.size()) return stop("the text ends inside an object");
            if (s_[at_] == ',') {
                ++at_;
                continue;
            }
            if (s_[at_] == '}') {
                ++at_;
                return true;
            }
            return stop("no ',' or '}' after an object's member");
        }
    }

    bool array(Value& out, int depth) {
        out.kind = Value::Kind::Array;
        ++at_;  // [
        space();
        if (at_ < s_.size() && s_[at_] == ']') {
            ++at_;
            return true;
        }
        for (;;) {
            Value item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            space();
            if (at_ >= s_.size()) return stop("the text ends inside an array");
            if (s_[at_] == ',') {
                ++at_;
                continue;
            }
            if (s_[at_] == ']') {
                ++at_;
                return true;
            }
            return stop("no ',' or ']' after an array's item");
        }
    }

    static void put_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
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

    bool hex4(uint32_t& out) {
        if (at_ + 4 > s_.size()) return stop("a \\u escape is cut short");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[at_++];
            uint32_t d = 0;
            if (h >= '0' && h <= '9') d = static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') d = static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') d = static_cast<uint32_t>(h - 'A' + 10);
            else return stop("a \\u escape has a non-hex digit");
            out = out << 4 | d;
        }
        return true;
    }

    bool string(std::string& out) {
        ++at_;  // "
        for (;;) {
            if (at_ >= s_.size()) return stop("the text ends inside a string");
            const char c = s_[at_];
            if (c == '"') {
                ++at_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) return stop("a control character inside a string");
            if (c != '\\') {
                out += c;
                ++at_;
                continue;
            }
            ++at_;
            if (at_ >= s_.size()) return stop("the text ends inside an escape");
            const char e = s_[at_++];
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
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        // A high surrogate: its low half must follow.
                        uint32_t low = 0;
                        if (s_.substr(at_, 2) != "\\u") return stop("a lone high surrogate");
                        at_ += 2;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low >= 0xE000) return stop("a high surrogate without its low half");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return stop("a lone low surrogate");
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return stop("an unknown escape");
            }
        }
    }

    bool number(Value& out) {
        const std::size_t start = at_;
        if (s_[at_] == '-') ++at_;
        const auto digits = [&] {
            const std::size_t from = at_;
            while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_;
            return at_ > from;
        };
        if (at_ < s_.size() && s_[at_] == '0') ++at_;
        else if (!digits()) return stop("a number without digits");
        if (at_ < s_.size() && s_[at_] == '.') {
            ++at_;
            if (!digits()) return stop("a number with no digits after '.'");
        }
        if (at_ < s_.size() && (s_[at_] == 'e' || s_[at_] == 'E')) {
            ++at_;
            if (at_ < s_.size() && (s_[at_] == '+' || s_[at_] == '-')) ++at_;
            if (!digits()) return stop("a number with no digits in its exponent");
        }
        out.kind = Value::Kind::Number;
        const std::string text(s_.substr(start, at_ - start));
        out.number = std::strtod(text.c_str(), nullptr);
        return true;
    }
};

}  // namespace detail

inline std::expected<Value, std::string> parse(std::string_view text) { return detail::Parser(text).document(); }

}  // namespace qilin::launcher::json
