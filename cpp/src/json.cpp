#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace moba::json {

const char* Value::type_name() const {
    switch (type) {
        case Null: return "null";
        case Bool: return "a boolean";
        case Num:
        case Float: return "a number";
        case Str: return "a string";
        case Arr: return "an array";
        case Obj: return "an object";
    }
    return "null";
}

bool Value::operator==(const Value& o) const {
    if (type != o.type) return false;
    switch (type) {
        case Null: return true;
        case Bool: return b == o.b;
        case Num: return n == o.n;
        case Float: return f == o.f;
        case Str: return s == o.s;
        case Arr: return arr == o.arr;
        case Obj: return obj == o.obj;
    }
    return false;
}

// ------------------------------------------------------------------------------ parsing

namespace {

/// Recursive-descent parser. Errors are not exceptions (WebAssembly builds disable them): the first
/// failure records its message in `err` and every caller returns false straight away.
struct Parser {
    const unsigned char* s;
    size_t n;
    size_t i = 0;
    std::string err;

    bool fail(const std::string& msg) {
        size_t line = 1, col = 1;
        size_t end = i < n ? i : n;
        for (size_t k = 0; k < end; ++k) {
            if (s[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        err = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + msg;
        return false;
    }

    void ws() {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    int peek() const { return i < n ? s[i] : -1; }
    bool digit() const { return i < n && s[i] >= '0' && s[i] <= '9'; }

    bool value(size_t depth, Value& out) {
        if (depth > MAX_DEPTH) return fail("nesting is too deep");
        int c = peek();
        if (c < 0) return fail("unexpected end of file");
        if (c == '{') return object(depth, out);
        if (c == '[') return array(depth, out);
        if (c == '"') {
            out = Value::str("");
            return string(out.s);
        }
        if (c == 't') return literal("true", Value::boolean(true), out);
        if (c == 'f') return literal("false", Value::boolean(false), out);
        if (c == 'n') return literal("null", Value::null(), out);
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool literal(const char* word, Value v, Value& out) {
        size_t len = std::char_traits<char>::length(word);
        if (n - i >= len && std::equal(word, word + len, (const char*)s + i)) {
            i += len;
            out = std::move(v);
            return true;
        }
        return fail("unexpected character");
    }

    bool number(Value& out) {
        size_t start = i;
        if (peek() == '-') ++i;
        if (peek() == '0') {
            ++i;
        } else if (peek() >= '1' && peek() <= '9') {
            while (digit()) ++i;
        } else {
            return fail("invalid number");
        }
        if (peek() == '.') {
            ++i;
            if (!digit()) return fail("invalid number");
            while (digit()) ++i;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++i;
            if (peek() == '+' || peek() == '-') ++i;
            if (!digit()) return fail("invalid number");
            while (digit()) ++i;
        }
        std::string text((const char*)s + start, i - start);
        double v = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(v)) return fail("number is out of range");
        out = Value::num(v);
        return true;
    }

    bool hex4(uint32_t& v) {
        v = 0;
        for (int k = 0; k < 4; ++k) {
            int c = peek();
            uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return fail("invalid \\u escape");
            v = v * 16 + d;
            ++i;
        }
        return true;
    }

    static void push_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++i;  // opening quote
        out.clear();
        for (;;) {
            int c = peek();
            if (c < 0) return fail("unterminated string");
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                ++i;
                int e = peek();
                if (e < 0) return fail("unterminated string");
                ++i;
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
                        uint32_t cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp < 0xDC00) {
                            if (peek() == '\\' && i + 1 < n && s[i + 1] == 'u') {
                                i += 2;
                                uint32_t lo;
                                if (!hex4(lo)) return false;
                                if (!(lo >= 0xDC00 && lo < 0xE000)) return fail("invalid surrogate pair");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                return fail("invalid surrogate pair");
                            }
                        } else if (cp >= 0xDC00 && cp < 0xE000) {
                            return fail("invalid surrogate pair");
                        }
                        push_utf8(out, cp);
                        break;
                    }
                    default:
                        --i;
                        return fail("invalid escape sequence");
                }
                continue;
            }
            if (c <= 0x1f) return fail("control character in string");
            out += (char)c;
            ++i;
        }
    }

    bool array(size_t depth, Value& out) {
        ++i;
        out = Value::array();
        ws();
        if (peek() == ']') {
            ++i;
            return true;
        }
        for (;;) {
            ws();
            Value x;
            if (!value(depth + 1, x)) return false;
            out.arr.push_back(std::move(x));
            ws();
            int c = peek();
            if (c == ',') {
                ++i;
            } else if (c == ']') {
                ++i;
                return true;
            } else if (c < 0) {
                return fail("unexpected end of file");
            } else {
                return fail("expected ',' or ']'");
            }
        }
    }

    bool object(size_t depth, Value& out) {
        ++i;
        out = Value::object();
        ws();
        if (peek() == '}') {
            ++i;
            return true;
        }
        for (;;) {
            ws();
            if (peek() != '"') return fail(peek() < 0 ? "unexpected end of file" : "expected a string key");
            size_t key_at = i;
            std::string key;
            if (!string(key)) return false;
            for (auto& kv : out.obj) {
                if (kv.first == key) {
                    i = key_at;
                    return fail("duplicate key \"" + key + "\"");
                }
            }
            ws();
            if (peek() != ':') return fail(peek() < 0 ? "unexpected end of file" : "expected ':'");
            ++i;
            ws();
            Value x;
            if (!value(depth + 1, x)) return false;
            out.obj.emplace_back(std::move(key), std::move(x));
            ws();
            int c = peek();
            if (c == ',') {
                ++i;
            } else if (c == '}') {
                ++i;
                return true;
            } else if (c < 0) {
                return fail("unexpected end of file");
            } else {
                return fail("expected ',' or '}'");
            }
        }
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
    Parser p{(const unsigned char*)text.data(), text.size(), 0, {}};
    p.ws();
    Value v;
    if (!p.value(0, v)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i < p.n) {
        p.fail("unexpected data after the end of the JSON value");
        err = p.err;
        return false;
    }
    out = std::move(v);
    return true;
}

bool valid_utf8(const unsigned char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return false;  // overlong / range
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // surrogates
        i += len;
    }
    return true;
}

// ------------------------------------------------------------------------------ writing

std::string quote(const std::string& v) {
    std::string s;
    s.reserve(v.size() + 2);
    s += '"';
    for (unsigned char c : v) {
        if (c == '"') s += "\\\"";
        else if (c == '\\') s += "\\\\";
        else if (c == '\n') s += "\\n";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
            s += buf;
        } else s += (char)c;
    }
    s += '"';
    return s;
}

namespace {

/// Digits and decimal exponent of a `%.*e` rendering: value = 0.<digits> * 10^(exp + 1).
struct Sci {
    bool neg = false;
    std::string digits;
    int exp = 0;
};

Sci split_sci(const char* buf) {
    Sci r;
    const char* p = buf;
    if (*p == '-') {
        r.neg = true;
        ++p;
    }
    for (; *p && *p != 'e'; ++p)
        if (*p != '.') r.digits += *p;
    if (*p == 'e') r.exp = std::atoi(p + 1);
    return r;
}

std::string join_sci(const Sci& x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%c%s%se%d", x.neg ? "-" : "", x.digits[0], x.digits.size() > 1 ? "." : "",
                  x.digits.size() > 1 ? x.digits.c_str() + 1 : "", x.exp);
    return buf;
}

/// Add `delta` (+1 or -1) to the last digit, carrying as needed. Returns false if impossible.
bool bump(Sci& x, int delta) {
    std::string& d = x.digits;
    int k = (int)d.size() - 1;
    if (delta > 0) {
        while (k >= 0 && d[k] == '9') d[k--] = '0';
        if (k < 0) {
            d.insert(d.begin(), '1');
            d.pop_back();
            x.exp += 1;
        } else {
            d[k] += 1;
        }
    } else {
        while (k >= 0 && d[k] == '0') d[k--] = '9';
        if (k < 0) return false;
        d[k] -= 1;
        if (d[0] == '0') {
            if (d.size() == 1) return false;
            d.erase(d.begin());
            d.push_back('9');
            x.exp -= 1;
        }
    }
    return true;
}

/// Rust's shortest round-trip formatting (no exponent). `f32_mode` round-trips through float.
std::string shortest(double v, bool f32_mode) {
    if (v == 0.0) return std::signbit(v) ? "-0" : "0";
    auto roundtrips = [&](const std::string& s) {
        return f32_mode ? std::strtof(s.c_str(), nullptr) == (float)v : std::strtod(s.c_str(), nullptr) == v;
    };
    int maxp = f32_mode ? 9 : 17;
    Sci best;
    bool found = false;
    for (int p = 1; p <= maxp && !found; ++p) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        Sci c = split_sci(buf);
        if (roundtrips(buf)) {
            best = c;
            found = true;
            break;
        }
        // The correctly rounded p-digit value can miss the rounding interval at a power of two
        // (where the interval is lopsided) while its neighbour is inside: try both neighbours.
        for (int delta : {1, -1}) {
            Sci n = c;
            if (bump(n, delta) && n.digits.size() == (size_t)p && roundtrips(join_sci(n))) {
                best = n;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*e", maxp - 1, v);
        best = split_sci(buf);
    }
    while (best.digits.size() > 1 && best.digits.back() == '0') best.digits.pop_back();
    // Render 0.d1d2..dn x 10^(exp+1) as a plain decimal.
    std::string out = best.neg ? "-" : "";
    const std::string& d = best.digits;
    int point = best.exp + 1;  // digits before the decimal point
    if (point <= 0) {
        out += "0.";
        out.append((size_t)(-point), '0');
        out += d;
    } else if ((size_t)point >= d.size()) {
        out += d;
        out.append((size_t)point - d.size(), '0');
    } else {
        out += d.substr(0, (size_t)point);
        out += '.';
        out += d.substr((size_t)point);
    }
    return out;
}

bool scalar(const Value& v) { return v.type != Value::Arr && v.type != Value::Obj; }

void write_value(const Value& v, size_t indent, std::string& out) {
    switch (v.type) {
        case Value::Null: out += "null"; return;
        case Value::Bool: out += v.b ? "true" : "false"; return;
        case Value::Num: out += fmt_f64(v.n); return;
        case Value::Float: out += fmt_f32(v.f); return;
        case Value::Str: out += quote(v.s); return;
        case Value::Arr: {
            if (v.arr.empty()) {
                out += "[]";
                return;
            }
            bool all_scalar = true;
            for (auto& x : v.arr) all_scalar = all_scalar && scalar(x);
            if (all_scalar) {
                out += '[';
                for (size_t i = 0; i < v.arr.size(); ++i) {
                    if (i > 0) out += ", ";
                    write_value(v.arr[i], indent, out);
                }
                out += ']';
                return;
            }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                out.append(indent + 2, ' ');
                write_value(v.arr[i], indent + 2, out);
                out += i + 1 < v.arr.size() ? ",\n" : "\n";
            }
            out.append(indent, ' ');
            out += ']';
            return;
        }
        case Value::Obj: {
            if (v.obj.empty()) {
                out += "{}";
                return;
            }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                out.append(indent + 2, ' ');
                out += quote(v.obj[i].first);
                out += ": ";
                write_value(v.obj[i].second, indent + 2, out);
                out += i + 1 < v.obj.size() ? ",\n" : "\n";
            }
            out.append(indent, ' ');
            out += '}';
            return;
        }
    }
}

}  // namespace

std::string fmt_f32(float v) { return std::isfinite(v) ? shortest((double)v, true) : "0"; }

std::string fmt_f64(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    return shortest(v, false);
}

std::string to_pretty(const Value& v) {
    std::string s;
    write_value(v, 0, s);
    s += '\n';
    return s;
}

}  // namespace moba::json
