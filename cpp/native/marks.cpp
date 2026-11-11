#include "marks.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/** How much of a pixel a shape covers, from the pixel centre's signed distance to its edge (in pixels). */
float coverage(float dist) { return std::clamp(0.5f - dist, 0.0f, 1.0f); }

uint8_t byte(float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

}  // namespace

void Marks::begin(float cx, float cz, float side) {
    std::memset(px.data(), 0, px.size());
    ox = cx - side / 2;
    oz = cz - side / 2;
    s = (float)PX / side;
}

void Marks::blend(int x, int y, Rgba c, float cov) {
    float sa = c.a * cov;
    if (sa <= 0.003f) return;
    uint8_t* p = &px[((size_t)y * PX + (size_t)x) * 4];
    float da = (float)p[3] / 255.0f, keep = da * (1 - sa), oa = sa + keep;
    p[0] = byte((c.r * sa + (float)p[0] / 255.0f * keep) / oa);
    p[1] = byte((c.g * sa + (float)p[1] / 255.0f * keep) / oa);
    p[2] = byte((c.b * sa + (float)p[2] / 255.0f * keep) / oa);
    p[3] = byte(oa);
}

void Marks::rect(float x, float y, float w, float h, Rgba c) {
    float x0 = (x - ox) * s, y0 = (y - oz) * s, x1 = x0 + w * s, y1 = y0 + h * s;
    int ix0 = std::max(0, (int)std::floor(x0)), ix1 = std::min(PX - 1, (int)std::floor(x1));
    int iy0 = std::max(0, (int)std::floor(y0)), iy1 = std::min(PX - 1, (int)std::floor(y1));
    for (int py = iy0; py <= iy1; py++) {
        float cy = std::min((float)py + 1, y1) - std::max((float)py, y0);
        for (int pxl = ix0; pxl <= ix1; pxl++) {
            float cx = std::min((float)pxl + 1, x1) - std::max((float)pxl, x0);
            if (cx > 0 && cy > 0) blend(pxl, py, c, cx * cy);
        }
    }
}

void Marks::disc(float x, float y, float radius, Rgba c) {
    float cx = (x - ox) * s, cy = (y - oz) * s, r = radius * s;
    int ix0 = std::max(0, (int)std::floor(cx - r - 1)), ix1 = std::min(PX - 1, (int)std::ceil(cx + r + 1));
    int iy0 = std::max(0, (int)std::floor(cy - r - 1)), iy1 = std::min(PX - 1, (int)std::ceil(cy + r + 1));
    for (int py = iy0; py <= iy1; py++)
        for (int pxl = ix0; pxl <= ix1; pxl++) {
            float dx = (float)pxl + 0.5f - cx, dy = (float)py + 0.5f - cy;
            blend(pxl, py, c, coverage(std::sqrt(dx * dx + dy * dy) - r));
        }
}

void Marks::ring(float x, float y, float radius, float width, Rgba c) {
    float cx = (x - ox) * s, cy = (y - oz) * s, r = radius * s;
    float half = std::max(width * s, 1.6f) / 2;  // never thinner than a pixel and a half
    float outer = r + half + 1, inner = std::max(0.0f, r - half - 1);
    int ix0 = std::max(0, (int)std::floor(cx - outer)), ix1 = std::min(PX - 1, (int)std::ceil(cx + outer));
    int iy0 = std::max(0, (int)std::floor(cy - outer)), iy1 = std::min(PX - 1, (int)std::ceil(cy + outer));
    for (int py = iy0; py <= iy1; py++) {
        float dy = (float)py + 0.5f - cy;
        for (int pxl = ix0; pxl <= ix1; pxl++) {
            float dx = (float)pxl + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 > outer * outer || d2 < inner * inner) continue;
            blend(pxl, py, c, coverage(std::fabs(std::sqrt(d2) - r) - half));
        }
    }
}

void Marks::line(float x0, float y0, float x1, float y1, float width, Rgba c) {
    float ax = (x0 - ox) * s, ay = (y0 - oz) * s, bx = (x1 - ox) * s, by = (y1 - oz) * s;
    float half = std::max(width * s, 1.6f) / 2;
    int ix0 = std::max(0, (int)std::floor(std::min(ax, bx) - half - 1)), ix1 = std::min(PX - 1, (int)std::ceil(std::max(ax, bx) + half + 1));
    int iy0 = std::max(0, (int)std::floor(std::min(ay, by) - half - 1)), iy1 = std::min(PX - 1, (int)std::ceil(std::max(ay, by) + half + 1));
    float vx = bx - ax, vy = by - ay, len2 = std::max(vx * vx + vy * vy, 1e-6f);
    for (int py = iy0; py <= iy1; py++)
        for (int pxl = ix0; pxl <= ix1; pxl++) {
            float qx = (float)pxl + 0.5f - ax, qy = (float)py + 0.5f - ay;
            float t = std::clamp((qx * vx + qy * vy) / len2, 0.0f, 1.0f);
            float dx = qx - vx * t, dy = qy - vy * t;
            blend(pxl, py, c, coverage(std::sqrt(dx * dx + dy * dy) - half));
        }
}
