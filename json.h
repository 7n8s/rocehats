// Minimal JSON reader (DOM). Enough for the WEAO API responses; no dependencies.
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;                 // UTF-8
    std::vector<Value> items;        // array elements, or object values
    std::vector<std::string> keys;   // object keys, parallel to `items`

    bool isBool() const { return type == Type::Bool; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    const Value* find(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
};

class Parser {
public:
    Parser(const char* begin, const char* end) : p_(begin), end_(end) {}

    bool parse(Value& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return p_ == end_;
    }

private:
    static const int kMaxDepth = 64;
    const char* p_;
    const char* end_;

    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    bool literal(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(end_ - p_) < n || memcmp(p_, s, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool value(Value& v, int depth) {
        if (p_ >= end_ || depth > kMaxDepth) return false;
        switch (*p_) {
        case '{': return object(v, depth);
        case '[': return array(v, depth);
        case '"': v.type = Type::String; return string(v.str);
        case 't': if (!literal("true")) return false; v.type = Type::Bool; v.b = true; return true;
        case 'f': if (!literal("false")) return false; v.type = Type::Bool; v.b = false; return true;
        case 'n': if (!literal("null")) return false; v.type = Type::Null; return true;
        default: return number(v);
        }
    }

    bool number(Value& v) {
        const char* s = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        if (p_ >= end_ || *p_ < '0' || *p_ > '9') return false;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
                             *p_ == '+' || *p_ == '-'))
            ++p_;
        std::string tmp(s, p_);
        char* e = nullptr;
        v.num = strtod(tmp.c_str(), &e);
        if (!e || *e != '\0') return false;
        v.type = Type::Number;
        return true;
    }

    bool hex4(unsigned& out) {
        if (end_ - p_ < 4) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++p_;  // opening quote
        while (p_ < end_) {
            unsigned char c = (unsigned char)*p_++;
            if (c == '"') return true;
            if (c != '\\') {
                out += (char)c;
                continue;
            }
            if (p_ >= end_) return false;
            char e = *p_++;
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
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: expect a low one
                    unsigned lo = 0;
                    const char* save = p_;
                    if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u' && (p_ += 2, hex4(lo)) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p_ = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;  // unterminated
    }

    bool array(Value& v, int depth) {
        v.type = Type::Array;
        ++p_;
        ws();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            ws();
            v.items.emplace_back();
            if (!value(v.items.back(), depth + 1)) return false;
            ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }

    bool object(Value& v, int depth) {
        v.type = Type::Object;
        ++p_;
        ws();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            ws();
            if (p_ >= end_ || *p_ != '"') return false;
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            ws();
            v.keys.push_back(std::move(key));
            v.items.emplace_back();
            if (!value(v.items.back(), depth + 1)) return false;
            ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }
};

inline bool Parse(const std::string& text, Value& out) {
    const char* b = text.data();
    const char* e = b + text.size();
    if (text.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
        (unsigned char)b[2] == 0xBF)
        b += 3;  // UTF-8 BOM
    Parser p(b, e);
    return p.parse(out);
}

}  // namespace json
