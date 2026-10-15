#include "path.hpp"

#include <algorithm>
#include <cstdlib>

namespace moba {

// ---- RustHeap ---------------------------------------------------------------------------------

// Rust: `o.f.cmp(&self.f).then_with(|| self.g.cmp(&o.g))` — lowest f first, ties prefer larger g.
int RustHeap::cmp(const PathNode& a, const PathNode& b) {
    if (b.f != a.f) return b.f < a.f ? -1 : 1;
    if (a.g != b.g) return a.g < b.g ? -1 : 1;
    return 0;
}

size_t RustHeap::sift_up(size_t start, size_t pos) {
    PathNode elem = data_[pos];
    while (pos > start) {
        size_t parent = (pos - 1) / 2;
        if (cmp(elem, data_[parent]) <= 0) break;  // elem <= parent
        data_[pos] = data_[parent];
        pos = parent;
    }
    data_[pos] = elem;
    return pos;
}

void RustHeap::sift_down_to_bottom(size_t pos) {
    size_t end = data_.size();
    size_t start = pos;
    PathNode elem = data_[pos];
    size_t child = 2 * pos + 1;
    while (child <= (end >= 2 ? end - 2 : 0) && end >= 2) {
        child += cmp(data_[child], data_[child + 1]) <= 0 ? 1 : 0;  // pick the greater child (right on ties)
        data_[pos] = data_[child];
        pos = child;
        child = 2 * pos + 1;
    }
    if (child == end - 1) {
        data_[pos] = data_[child];
        pos = child;
    }
    data_[pos] = elem;
    sift_up(start, pos);
}

void RustHeap::push(PathNode n) {
    size_t old_len = data_.size();
    data_.push_back(n);
    sift_up(0, old_len);
}

bool RustHeap::pop(PathNode& out) {
    if (data_.empty()) return false;
    PathNode item = data_.back();
    data_.pop_back();
    if (!data_.empty()) {
        std::swap(item, data_[0]);
        sift_down_to_bottom(0);
    }
    out = item;
    return true;
}

// ---- PathFinder -------------------------------------------------------------------------------

namespace {

struct Dir {
    int32_t dx, dy, cost;
};
const Dir DIRS[8] = {{1, 0, 10}, {-1, 0, 10}, {0, 1, 10}, {0, -1, 10}, {1, 1, 14}, {1, -1, 14}, {-1, 1, 14}, {-1, -1, 14}};

int32_t heuristic(int32_t x, int32_t y, Cell goal) {
    int32_t dx = std::abs(x - goal.first);
    int32_t dy = std::abs(y - goal.second);
    return 10 * (dx + dy) - 6 * std::min(dx, dy);  // octile distance
}

/// Greedy string pulling: keep only the waypoints needed to avoid obstacles.
std::vector<Vec2> smooth(const Map& map, Vec2 from, const std::vector<Vec2>& pts) {
    std::vector<Vec2> out;
    Vec2 anchor = from;
    size_t i = 0;
    while (i < pts.size()) {
        size_t best = i;
        for (size_t k = pts.size(); k-- > i;) {
            if (map.los(anchor, pts[k])) {
                best = k;
                break;
            }
        }
        out.push_back(pts[best]);
        anchor = pts[best];
        i = best + 1;
    }
    // Drop a leading waypoint we are already standing on.
    if (out.size() > 1 && out[0].dist(from) < CELL * 0.25f) out.erase(out.begin());
    return out;
}

}  // namespace

PathFinder::PathFinder() {
    size_t n = (size_t)GRID * GRID;
    g_.assign(n, 0);
    parent_.assign(n, -1);
    open_stamp_.assign(n, 0);
    closed_stamp_.assign(n, 0);
}

std::optional<std::vector<Vec2>> PathFinder::find(const Map& map, Vec2 from, Vec2 to) {
    auto start_c = map.nearest_walkable_cell(from);
    if (!start_c) return std::nullopt;
    auto goal_c = map.nearest_walkable_cell(to);
    if (!goal_c) return std::nullopt;
    Cell start = *start_c, goal = *goal_c;

    stamp_ += 1;
    if (stamp_ == 0) {
        std::fill(open_stamp_.begin(), open_stamp_.end(), 0u);
        std::fill(closed_stamp_.begin(), closed_stamp_.end(), 0u);
        stamp_ = 1;
    }
    auto cell_idx = [](Cell c) { return (size_t)c.second * GRID + (size_t)c.first; };
    size_t sidx = cell_idx(start), gidx = cell_idx(goal);

    heap_.clear();
    g_[sidx] = 0;
    parent_[sidx] = -1;
    open_stamp_[sidx] = stamp_;
    heap_.push(PathNode{heuristic(start.first, start.second, goal), 0, (uint32_t)sidx});

    bool found = false;
    PathNode node;
    while (heap_.pop(node)) {
        size_t i = node.idx;
        int32_t g = node.g;
        if (closed_stamp_[i] == stamp_) continue;
        closed_stamp_[i] = stamp_;
        if (i == gidx) {
            found = true;
            break;
        }
        int32_t cx = (int32_t)(i % GRID), cy = (int32_t)(i / GRID);
        for (const Dir& d : DIRS) {
            int32_t nx = cx + d.dx, ny = cy + d.dy;
            if (!map.cell_walkable(nx, ny)) continue;
            // No squeezing diagonally between two blocked cells.
            if (d.dx != 0 && d.dy != 0 && !(map.cell_walkable(cx + d.dx, cy) && map.cell_walkable(cx, cy + d.dy))) continue;
            size_t ni = (size_t)ny * GRID + (size_t)nx;
            if (closed_stamp_[ni] == stamp_) continue;
            int32_t ng = g + d.cost;
            if (open_stamp_[ni] != stamp_ || ng < g_[ni]) {
                open_stamp_[ni] = stamp_;
                g_[ni] = ng;
                parent_[ni] = (int32_t)i;
                heap_.push(PathNode{ng + heuristic(nx, ny, goal), ng, (uint32_t)ni});
            }
        }
    }
    if (!found) return std::nullopt;

    // Walk the parent chain back from the goal.
    std::vector<Vec2> pts;
    int32_t cur = (int32_t)gidx;
    while (cur >= 0 && (size_t)cur != sidx) {
        size_t c = (size_t)cur;
        pts.push_back(Map::cell_center((int32_t)(c % GRID), (int32_t)(c / GRID)));
        cur = parent_[c];
    }
    std::reverse(pts.begin(), pts.end());
    if (!pts.empty()) {
        if (map.walkable(to)) pts.back() = to;
    } else {
        pts.push_back(map.walkable(to) ? to : Map::cell_center(goal.first, goal.second));
    }
    return smooth(map, from, pts);
}

}  // namespace moba
