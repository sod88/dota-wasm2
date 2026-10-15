#include "map.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdlib>

namespace moba {

namespace {
constexpr float LANE_HALF_WIDTH = 72.0f;
constexpr float RIVER_HALF_WIDTH = 50.0f;
constexpr float BASE_RADIUS = 250.0f;
}  // namespace

Map Map::generate() {
    Map m;
    m.terrain.assign((size_t)GRID * GRID, T_BLOCKED);
    const Vec2 corner_tl(280.0f, 280.0f);
    const Vec2 corner_br(2120.0f, 2120.0f);
    m.lanes[lane_idx(Lane::Top)] = {BASE_RADIANT, corner_tl, BASE_DIRE};
    m.lanes[lane_idx(Lane::Mid)] = {BASE_RADIANT, BASE_DIRE};
    m.lanes[lane_idx(Lane::Bot)] = {BASE_RADIANT, corner_br, BASE_DIRE};

    // Segments jungle corridors may connect to: every lane segment plus the river.
    std::vector<std::pair<Vec2, Vec2>> connectors{{corner_tl, corner_br}};
    for (auto& lane : m.lanes)
        for (size_t i = 0; i + 1 < lane.size(); ++i) connectors.emplace_back(lane[i], lane[i + 1]);

    m.carve_circle(BASE_RADIANT, BASE_RADIUS, T_GROUND);
    m.carve_circle(BASE_DIRE, BASE_RADIUS, T_GROUND);

    // Radiant-half camps; the Dire half is the point-mirror image.
    struct C {
        float x, y;
        uint8_t tier;
    };
    const C radiant_camps[6] = {{600.0f, 1000.0f, 0}, {520.0f, 1450.0f, 1}, {900.0f, 1250.0f, 2},
                                {1250.0f, 1750.0f, 1}, {1650.0f, 1900.0f, 0}, {1350.0f, 1550.0f, 2}};
    for (auto& rc : radiant_camps) {
        for (Vec2 pos : {Vec2(rc.x, rc.y), Vec2(WORLD - rc.x, WORLD - rc.y)}) {
            m.carve_circle(pos, 70.0f, T_GROUND);
            float best_d = FLT_MAX;
            Vec2 best_q = pos;
            for (auto& ab : connectors) {
                Vec2 q = closest_point_on_segment(pos, ab.first, ab.second);
                float d = pos.dist(q);
                if (d < best_d) {
                    best_d = d;
                    best_q = q;
                }
            }
            m.carve_capsule(pos, best_q, 40.0f, T_GROUND);
            m.camps.push_back(CampSpawn{pos, rc.tier});
        }
    }

    auto lanes = m.lanes;
    for (auto& lane : lanes)
        for (size_t i = 0; i + 1 < lane.size(); ++i) m.carve_capsule(lane[i], lane[i + 1], LANE_HALF_WIDTH, T_LANE);
    m.carve_capsule(corner_tl, corner_br, RIVER_HALF_WIDTH, T_RIVER);

    // Rocks: a pair in the river (forces detours) and one in each base.
    const float rocks[4][3] = {{800.0f, 800.0f, 34.0f}, {1600.0f, 1600.0f, 34.0f}, {150.0f, 1950.0f, 34.0f}, {2250.0f, 450.0f, 34.0f}};
    for (auto& r : rocks) m.block_circle(Vec2(r[0], r[1]), r[2]);
    return m;
}

void Map::carve_capsule(Vec2 a, Vec2 b, float half_width, uint8_t value) {
    float r2 = half_width * half_width;
    for (int cy = 0; cy < GRID; ++cy) {
        for (int cx = 0; cx < GRID; ++cx) {
            Vec2 c = cell_center(cx, cy);
            Vec2 q = closest_point_on_segment(c, a, b);
            if (c.dist_sq(q) <= r2) {
                uint8_t& t = terrain[(size_t)cy * GRID + cx];
                if (value > t) t = value;
            }
        }
    }
}

void Map::block_circle(Vec2 c, float r) {
    float r2 = r * r;
    for (int cy = 0; cy < GRID; ++cy)
        for (int cx = 0; cx < GRID; ++cx)
            if (cell_center(cx, cy).dist_sq(c) <= r2) terrain[(size_t)cy * GRID + cx] = T_BLOCKED;
}

bool Map::los(Vec2 a, Vec2 b) const {
    float d = a.dist(b);
    int32_t steps = f2i32(fmax_rs(std::ceil(d / (CELL * 0.5f)), 1.0f));
    for (int32_t i = 0; i <= steps; ++i) {
        float t = (float)i / (float)steps;
        if (!walkable(a + (b - a) * t)) return false;
    }
    return true;
}

std::optional<Cell> Map::nearest_walkable_cell(Vec2 p) const {
    Cell c0 = cell_of(p);
    int32_t cx = clamp_int<int32_t>(c0.first, 0, GRID - 1), cy = clamp_int<int32_t>(c0.second, 0, GRID - 1);
    if (cell_walkable(cx, cy)) return Cell{cx, cy};
    for (int32_t r = 1; r <= 20; ++r) {
        bool have = false;
        float bd = 0.0f;
        Cell best{0, 0};
        for (int32_t dy = -r; dy <= r; ++dy) {
            for (int32_t dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r || !cell_walkable(cx + dx, cy + dy)) continue;
                float d = cell_center(cx + dx, cy + dy).dist_sq(p);
                if (!have || d < bd) {
                    have = true;
                    bd = d;
                    best = {cx + dx, cy + dy};
                }
            }
        }
        if (have) return best;
    }
    return std::nullopt;
}

Vec2 Map::nearest_walkable(Vec2 p) const {
    if (walkable(p)) return p;
    auto c = nearest_walkable_cell(p);
    return c ? cell_center(c->first, c->second) : p;
}

float Map::lane_length(Lane lane) const {
    const auto& poly = lanes[lane_idx(lane)];
    float sum = -0.0f;  // Rust's `Sum for f32` starts from -0.0
    for (size_t i = 0; i + 1 < poly.size(); ++i) sum += poly[i].dist(poly[i + 1]);
    return sum;
}

Vec2 Map::lane_point(Lane lane, float frac) const {
    const auto& poly = lanes[lane_idx(lane)];
    float left = lane_length(lane) * fclamp_rs(frac, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < poly.size(); ++i) {
        float seg = poly[i].dist(poly[i + 1]);
        if (left <= seg) return poly[i].move_toward(poly[i + 1], left);
        left -= seg;
    }
    return poly.back();
}

}  // namespace moba
