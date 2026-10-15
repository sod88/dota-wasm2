// The battlefield: a walkability grid, three lanes, a river and jungle camps (port of src/map.rs).
//
// Layout (y grows downwards, like the canvas): Radiant base bottom-left, Dire base top-right.
// Lanes are polylines running Radiant -> Dire. The map is point-symmetric about its centre.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "math.hpp"

namespace moba {

constexpr float CELL = 24.0f;
constexpr int GRID = 100;
constexpr float WORLD = CELL * (float)GRID;  // 2400 world units

constexpr Vec2 BASE_RADIANT{280.0f, 2120.0f};
constexpr Vec2 BASE_DIRE{2120.0f, 280.0f};

/// Terrain codes. Higher value wins when shapes overlap while carving.
constexpr uint8_t T_BLOCKED = 0;
constexpr uint8_t T_GROUND = 1;
constexpr uint8_t T_LANE = 2;
constexpr uint8_t T_RIVER = 3;

enum class Lane : uint8_t { Top = 0, Mid = 1, Bot = 2 };
constexpr std::array<Lane, 3> ALL_LANES{Lane::Top, Lane::Mid, Lane::Bot};
inline size_t lane_idx(Lane l) { return (size_t)l; }

struct CampSpawn {
    Vec2 pos;
    /// 0 = small, 1 = medium, 2 = large
    uint8_t tier;
};

using Cell = std::pair<int32_t, int32_t>;

struct Map {
    std::vector<uint8_t> terrain;
    /// Lane polylines, ordered Radiant base -> Dire base.
    std::array<std::vector<Vec2>, 3> lanes;
    std::vector<CampSpawn> camps;

    static Map generate();

    static Cell cell_of(Vec2 p) { return {f2i32(std::floor(p.x / CELL)), f2i32(std::floor(p.y / CELL))}; }
    static Vec2 cell_center(int32_t cx, int32_t cy) { return Vec2(((float)cx + 0.5f) * CELL, ((float)cy + 0.5f) * CELL); }
    bool cell_walkable(int32_t cx, int32_t cy) const {
        return cx >= 0 && cy >= 0 && cx < GRID && cy < GRID && terrain[(size_t)cy * GRID + (size_t)cx] != T_BLOCKED;
    }
    bool walkable(Vec2 p) const {
        Cell c = cell_of(p);
        return cell_walkable(c.first, c.second);
    }
    /// True if the straight segment a-b stays on walkable ground.
    bool los(Vec2 a, Vec2 b) const;
    /// Closest walkable cell to `p` (searching outwards in rings).
    std::optional<Cell> nearest_walkable_cell(Vec2 p) const;
    /// Closest walkable world position to `p`.
    Vec2 nearest_walkable(Vec2 p) const;
    float lane_length(Lane lane) const;
    /// Point at arc-length fraction `frac` (0 = Radiant base, 1 = Dire base) along a lane.
    Vec2 lane_point(Lane lane, float frac) const;

   private:
    void carve_capsule(Vec2 a, Vec2 b, float half_width, uint8_t value);
    void carve_circle(Vec2 c, float r, uint8_t value) { carve_capsule(c, c, r, value); }
    void block_circle(Vec2 c, float r);
};

}  // namespace moba
