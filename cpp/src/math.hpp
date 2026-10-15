// Small 2D math helpers and a deterministic PRNG (C++ port of src/math.rs).
//
// Determinism note: the C++ and Rust engines must produce bit-identical simulations, so every
// float operation here is single precision (note the `f` suffixes) and the helpers below
// reproduce Rust semantics exactly where C++ differs: NaN-aware min/max, saturating casts.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace moba {

// ---- Rust-compatible scalar helpers --------------------------------------------------------

/// `f32::min` (returns the non-NaN operand if one is NaN).
inline float fmin_rs(float a, float b) {
    if (std::isnan(a)) return b;
    if (std::isnan(b)) return a;
    return b < a ? b : a;
}
/// `f32::max`.
inline float fmax_rs(float a, float b) {
    if (std::isnan(a)) return b;
    if (std::isnan(b)) return a;
    return a < b ? b : a;
}
/// `f32::clamp`.
inline float fclamp_rs(float v, float lo, float hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
template <typename T>
inline T clamp_int(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Rust `x as i32` for floats: saturating, NaN -> 0.
inline int32_t f2i32(float v) {
    if (std::isnan(v)) return 0;
    if (v <= -2147483648.0f) return INT32_MIN;
    if (v >= 2147483648.0f) return INT32_MAX;
    return (int32_t)v;
}
/// Rust `x as usize` / `as u32` for floats: saturating, NaN -> 0.
inline uint64_t f2u64(double v) {
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= 18446744073709551616.0) return UINT64_MAX;
    return (uint64_t)v;
}
inline uint32_t f2u32(double v) {
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= 4294967296.0) return UINT32_MAX;
    return (uint32_t)v;
}
inline uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// ---- Vec2 --------------------------------------------------------------------------------

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    float len_sq() const { return x * x + y * y; }
    float len() const { return std::sqrt(len_sq()); }
    float dist(Vec2 o) const { return (*this - o).len(); }
    float dist_sq(Vec2 o) const { return (*this - o).len_sq(); }
    float dot(Vec2 o) const { return x * o.x + y * o.y; }

    /// Unit vector in the same direction (zero vector stays zero).
    Vec2 norm() const {
        float l = len();
        if (l > 1e-6f) return *this * (1.0f / l);
        return Vec2();
    }
    /// Move towards `target` by at most `max_step`.
    Vec2 move_toward(Vec2 target, float max_step) const {
        Vec2 d = target - *this;
        float l = d.len();
        if (l <= max_step || l < 1e-6f) return target;
        return *this + d * (max_step / l);
    }

    Vec2 operator+(Vec2 o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(Vec2 o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    bool operator==(Vec2 o) const { return x == o.x && y == o.y; }
    bool operator!=(Vec2 o) const { return !(*this == o); }
};

/// Closest point to `p` on the segment `a`-`b`.
inline Vec2 closest_point_on_segment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    float l2 = ab.len_sq();
    if (l2 < 1e-6f) return a;
    float t = fclamp_rs((p - a).dot(ab) / l2, 0.0f, 1.0f);
    return a + ab * t;
}

// ---- PRNG ----------------------------------------------------------------------------------

/// xorshift64* generator: tiny, fast, and fully deterministic for a given seed.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s((seed * 0x9E3779B97F4A7C15ull ^ 0xD1B54A32D192ED03ull) | 1ull) {}
    uint32_t next_u32() {
        uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
    }
    /// Uniform in [0, 1).
    float f32() { return (float)(next_u32() >> 8) / 16777216.0f; }
    /// Uniform in [lo, hi).
    float range(float lo, float hi) { return lo + (hi - lo) * f32(); }
};

}  // namespace moba
