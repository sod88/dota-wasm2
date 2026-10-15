// Play a full bot-vs-bot match without any graphics and print a timeline (C++ twin of
// examples/headless.rs — same arguments, and the same final state hash for the same match).
//
//   cpp/build/headless [heroes_per_team] [seed] [max_minutes] [mod.json ...]
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "content.hpp"
#include "game.hpp"

using namespace moba;

static void report(const Game& g) {
    std::printf("--- t = %5.1f min   wave %2u   units alive %3zu\n", g.time / 60.0f, g.wave_no,
                (size_t)std::count_if(g.units.begin(), g.units.end(), [](const Unit& u) { return u.alive; }));
    for (uint8_t team = 0; team < 2; ++team) {
        float ahp = 0.0f;
        for (const Unit& u : g.units)
            if (u.kind == Kind::Ancient && u.team == team) ahp = u.hp;
        std::printf("  %s towers %2zu/9   ancient hp %5.0f\n", team == 0 ? "Radiant" : "Dire   ", g.count_kind(team, Kind::Tower), ahp);
    }
    for (const HeroData& h : g.heroes) {
        const Unit* u = nullptr;
        for (const Unit& x : g.units)
            if (x.id == h.unit) u = &x;
        std::string items;
        for (const ItemSlot& s : h.items) {
            if (s.id == 0) continue;
            if (!items.empty()) items += ", ";
            items += g.content.items[s.id].name;
        }
        std::printf("  %c %-9s L%-2u %4.0fg  K/D/A %u/%u/%u  LH %-3u DN %-2u hp %4.0f/%-4.0f %s  [%s]\n", h.team == 0 ? 'R' : 'D',
                    g.content.heroes[h.def].name.c_str(), h.level, h.gold, h.kills, h.deaths, h.assists, h.last_hits, h.denies,
                    u ? std::max(u->hp, 0.0f) : 0.0f, u ? u->max_hp : 0.0f, u && u->alive ? "alive" : "dead ", items.c_str());
    }
}

int main(int argc, char** argv) {
    int per = argc > 1 ? std::atoi(argv[1]) : 3;
    uint32_t seed = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 7;
    float max_min = argc > 3 ? std::strtof(argv[3], nullptr) : 45.0f;

    // Optional mods, applied in order.
    Content content = *builtin();
    for (int a = 4; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", argv[a]);
            return 2;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        Content next;
        std::vector<std::string> errors;
        if (!apply_mod(content, ss.str(), next, errors)) {
            std::fprintf(stderr, "%s:\n", argv[a]);
            for (auto& e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
            return 1;
        }
        content = std::move(next);
        std::printf("mod loaded: %s\n", content.mods.back().name.c_str());
    }

    Config cfg;
    cfg.seed = seed;
    cfg.player_hero = 0;
    cfg.heroes_per_team = (uint8_t)per;
    cfg.fog = true;
    cfg.player_is_bot = true;
    Game g(cfg, std::make_shared<const Content>(content));
    auto started = std::chrono::steady_clock::now();
    float next_report = 0.0f;
    while (g.winner < 0 && g.time < max_min * 60.0f) {
        g.step();
        if (g.time >= next_report) {
            next_report += 120.0f;
            report(g);
        }
    }
    report(g);
    if (g.winner == 0) std::printf("\nWINNER: Radiant at %.1f min\n", g.time / 60.0f);
    else if (g.winner == 1) std::printf("\nWINNER: Dire at %.1f min\n", g.time / 60.0f);
    else std::printf("\nNo winner after %.1f min\n", g.time / 60.0f);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("simulated %u ticks in %.0f ms (%.2f ms/tick, state hash %016" PRIx64 ")\n", g.tick, ms, ms / g.tick, g.state_hash());
    return 0;
}
