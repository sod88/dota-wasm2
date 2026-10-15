// A tiny JSON parser and pretty printer used to load and export mod files (port of src/json.rs).
//
// Behaviour that matters for mods, reproduced exactly from the Rust engine:
//  * object keys keep their file order and duplicate keys are an error,
//  * errors carry `line L, column C` (1-based, counted in bytes),
//  * nesting is limited to MAX_DEPTH, numbers must be finite,
//  * numbers print like Rust's `{}`: the shortest decimal that round-trips, never exponent form.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace moba::json {

constexpr size_t MAX_DEPTH = 32;

struct Value {
    enum Type { Null, Bool, Num, Float, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double n = 0.0;  // Num
    float f = 0.0f;  // Float: only produced by exporters, prints as the shortest f32 decimal
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value null() { return Value(); }
    static Value boolean(bool v) { Value x; x.type = Bool; x.b = v; return x; }
    static Value num(double v) { Value x; x.type = Num; x.n = v; return x; }
    static Value flt(float v) { Value x; x.type = Float; x.f = v; return x; }
    static Value str(std::string v) { Value x; x.type = Str; x.s = std::move(v); return x; }
    static Value array(std::vector<Value> v = {}) { Value x; x.type = Arr; x.arr = std::move(v); return x; }
    static Value object(std::vector<std::pair<std::string, Value>> v = {}) { Value x; x.type = Obj; x.obj = std::move(v); return x; }

    const char* type_name() const;
    bool is(Type t) const { return type == t; }
    bool operator==(const Value& o) const;
};

/// Parse `text`. Returns false and sets `err` (e.g. "line 1, column 6: unexpected end of file") on failure.
bool parse(const std::string& text, Value& out, std::string& err);

/// JSON string literal. Only `"`, `\\`, `\n` and control characters are escaped.
std::string quote(const std::string& v);
/// Rust `format!("{}", v)` for f32: shortest round-trip decimal, no exponent; non-finite prints "0".
std::string fmt_f32(float v);
/// Rust `format!("{}", v)` for f64.
std::string fmt_f64(double v);
/// Human-readable JSON: objects one key per line, arrays of scalars on one line.
std::string to_pretty(const Value& v);

/// True if `bytes` is valid UTF-8 (same rules as Rust's `str::from_utf8`).
bool valid_utf8(const unsigned char* bytes, size_t len);

}  // namespace moba::json
