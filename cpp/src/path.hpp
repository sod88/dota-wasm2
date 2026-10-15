// A* pathfinding on the walkability grid (8-way, no corner cutting) with line-of-sight
// "string pulling" so units walk smooth, direct-looking routes (port of src/path.rs).
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "map.hpp"

namespace moba {

struct PathNode {
    int32_t f, g;
    uint32_t idx;
};

/// A max-heap that pops in exactly the same order as Rust's `std::collections::BinaryHeap`
/// (same sift-up / sift-down-to-bottom algorithm), so equal-priority nodes are expanded in the
/// same order by both engines. `less(a, b)` is Rust's `a < b` for the element type.
class RustHeap {
   public:
    void clear() { data_.clear(); }
    bool empty() const { return data_.empty(); }
    void push(PathNode n);
    bool pop(PathNode& out);

   private:
    std::vector<PathNode> data_;
    static int cmp(const PathNode& a, const PathNode& b);  // Ord::cmp: -1, 0, 1
    size_t sift_up(size_t start, size_t pos);
    void sift_down_to_bottom(size_t pos);
};

/// Reusable A* scratch space. Generation stamps avoid clearing the arrays per query.
class PathFinder {
   public:
    PathFinder();
    /// Waypoints from `from` (exclusive) to `to` (inclusive, or the nearest walkable point if `to`
    /// itself is blocked). Empty optional if no route exists.
    std::optional<std::vector<Vec2>> find(const Map& map, Vec2 from, Vec2 to);

   private:
    std::vector<int32_t> g_, parent_;
    std::vector<uint32_t> open_stamp_, closed_stamp_;
    uint32_t stamp_ = 0;
    RustHeap heap_;
};

}  // namespace moba
