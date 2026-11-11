#include "play.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

extern "C" float* r3d_cam_ptr();  // cpp/render/render3d.cpp: view-projection, its inverse, the eye

namespace app {

void step(Match& m) {
    m.prev.clear();
    for (const Unit& u : m.g->units) m.prev.emplace_back(u.id, u.pos);
    m.g->step();
}

/** Where to draw a unit between the last tick and this one (alpha 0..1). */
Vec2 drawn_pos(const Match& m, const Unit& u, float alpha) {
    auto it = std::lower_bound(m.prev.begin(), m.prev.end(), u.id, [](const auto& p, uint32_t id) { return p.first < id; });
    if (it == m.prev.end() || it->first != u.id) return u.pos;
    Vec2 a = it->second;
    if (a.dist_sq(u.pos) > 200.0f * 200.0f) return u.pos;  // blinked or respawned
    return Vec2(a.x + (u.pos.x - a.x) * alpha, a.y + (u.pos.y - a.y) * alpha);
}

/** Is this point inside the player team's vision? (seen() in cpp/src/ffi.cpp) */
bool seen(const Game& g, Vec2 p) {
    for (const Unit& u : g.units)
        if (u.alive && u.team == g.player_team && u.pos.dist_sq(p) <= u.vision * u.vision) return true;
    return false;
}

// ------------------------------------------------------------------------------- camera maths

/** Where a point of the window lies on the ground (s2w() in web/js/render3d.js), from the camera last set. */
Vec2 screen_to_world(const Screen& sc, float sx, float sy) {
    const float* m = r3d_cam_ptr() + 16;  // the inverse view-projection matrix
    float nx = sx / sc.w * 2 - 1, ny = 1 - sy / sc.h * 2;
    auto unproject = [&](float z, float* o) {
        float w = m[3] * nx + m[7] * ny + m[11] * z + m[15];
        o[0] = (m[0] * nx + m[4] * ny + m[8] * z + m[12]) / w;
        o[1] = (m[1] * nx + m[5] * ny + m[9] * z + m[13]) / w;
        o[2] = (m[2] * nx + m[6] * ny + m[10] * z + m[14]) / w;
    };
    float a[3], b[3];  // the ray through that pixel, from the near plane to the far plane
    unproject(0, a);
    unproject(1, b);
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float t = dy < -1e-6f ? -a[1] / dy : 20.0f;
    return Vec2(a[0] + dx * t, a[2] + dz * t);
}

// ------------------------------------------------------------------------------- playing your hero

const Rgba ALLY{0.31f, 0.82f, 0.48f, 1}, ENEMY{1.0f, 0.36f, 0.32f, 1}, NEUTRAL{0.88f, 0.75f, 0.38f, 1};
const Rgba GOLD{1.0f, 0.87f, 0.47f, 1}, MANA{0.29f, 0.64f, 1.0f, 1}, AIM{0.55f, 0.82f, 1.0f, 1}, BLACK{0, 0, 0, 1};

bool shown(const Game& g, size_t i, bool fog) {
    return !fog || g.units[i].team == g.player_team || g.visible_to(g.player_team, i);
}

bool is_enemy(const Game& g, int i) { return g.units[(size_t)i].team != g.player_team; }

/** The unit under a point on the ground (pickUnit() in web/js/main.js). */
int pick_unit(const Match& m, float alpha, bool fog, Vec2 w) {
    const Game& g = *m.g;
    int best = -1;
    float best_d = 0;
    for (size_t i = 0; i < g.units.size(); i++) {
        const Unit& u = g.units[i];
        if (!u.alive || !shown(g, i, fog)) continue;
        float d = drawn_pos(m, u, alpha).dist(w) - (u.radius * 1.3f + 6);
        if (d < best_d) {
            best_d = d;
            best = (int)i;
        }
    }
    return best;
}

Cmd order(Cmd::K k) {
    Cmd c{};
    c.k = k;
    return c;
}

/** Queue an order for your hero; the engine applies it on the next tick, as it does for the browser. */
void send(Game& g, Cmd c) {
    c.unit = g.player;
    g.push_cmd(c);
}

/** Your hero's unit and hero record, or false before it exists. */
bool me(const Game& g, size_t& ui, size_t& hi) {
    auto pi = g.player_idx();
    if (!pi || g.units[*pi].hero < 0) return false;
    ui = *pi;
    hi = (size_t)g.units[*pi].hero;
    return true;
}

void right_click(Game& g, Ui& ui) {
    if (ui.aim.mode != Aim::Off) {  // a right click backs out of targeting
        ui.aim = Aim{};
        return;
    }
    if (ui.hover >= 0 && g.units[(size_t)ui.hover].id != g.player) {
        const Unit& t = g.units[(size_t)ui.hover];
        // deny: finish off a hurt allied creep so the enemy cannot farm it
        bool deny = is_lane_creep(t.kind) && t.hp < t.max_hp * 0.5f;
        if (is_enemy(g, ui.hover) || deny) {
            Cmd c = order(Cmd::Attack);
            c.target = t.id;
            send(g, c);
            ui.ping(t.pos, ENEMY);
            return;
        }
    }
    Cmd c = order(Cmd::Move);
    c.to = ui.mouse;
    send(g, c);
    ui.ping(ui.mouse, ALLY);
}

void left_click(Game& g, Ui& ui) {
    if (ui.aim.mode == Aim::AttackMove) {
        Cmd c = order(Cmd::AttackMove);
        c.to = ui.mouse;
        if (ui.hover >= 0 && is_enemy(g, ui.hover)) {
            c = order(Cmd::Attack);
            c.target = g.units[(size_t)ui.hover].id;
        }
        send(g, c);
        ui.ping(ui.mouse, ENEMY);
        ui.aim = Aim{};
    } else if (ui.aim.mode == Aim::Spell) {
        Cmd c = order(Cmd::Cast);
        c.slot = ui.aim.slot;
        c.to = ui.mouse;
        if (ui.aim.shape == Aim::OnUnit) {
            if (ui.hover < 0 || !is_enemy(g, ui.hover)) return ui.say("Click an enemy unit");
            c.target = g.units[(size_t)ui.hover].id;
            c.to = g.units[(size_t)ui.hover].pos;
        }
        send(g, c);
        ui.ping(c.to, AIM);
        ui.aim = Aim{};
    }
}

/** Q W E R or an item key: cast at once, or start aiming (beginCast() in web/js/main.js). */
void begin_cast(Game& g, Ui& ui, uint8_t slot) {
    size_t i = 0, hi = 0;
    if (!me(g, i, hi)) return;
    const Unit& u = g.units[i];
    const HeroData& h = g.heroes[hi];
    if (!u.alive) return ui.say("You are dead");
    if (u.stunned()) return ui.say("Stunned");

    Aim a;
    a.mode = Aim::Spell;
    a.slot = slot;
    bool on_self = false;
    if (slot < 4) {
        size_t l = 0;
        const AbilityDef* ab = g.ability_def(i, slot, l);
        if (!ab) return ui.say(h.skill_pts > 0 ? std::string("Not learned yet: Shift+") + "QWER"[slot] + " learns it" : "Ability not learned yet");
        if (h.ab_cd[slot] > 0.05f) return ui.say(ab->name + " is on cooldown");
        if (u.mana < ab->mana[l]) return ui.say("Not enough mana for " + ab->name);
        a.range = ab->range[l];
        a.radius = ab->radius[l];
        switch (ab->cast) {
            case Cast::Target: a.shape = Aim::OnUnit; break;
            case Cast::Line: a.shape = Aim::Line; break;
            case Cast::PointAoE: a.shape = Aim::Point; break;
            case Cast::Blink: a.shape = Aim::Blink; break;
            case Cast::SelfAoE:
            case Cast::SelfBuff: on_self = true; break;
            case Cast::None: return ui.say(ab->name + " is passive");
        }
    } else {
        const ItemSlot& s = h.items[(size_t)slot - 10];
        if (s.id == 0) return;
        const ItemDef& it = g.content.items[s.id];
        if (it.active == ACT_NONE) return ui.say(it.name + " is a passive item");
        if (s.cd > 0.05f) return ui.say(it.name + " is on cooldown");
        if (it.active == ACT_BLINK) {
            a.shape = Aim::Blink;
            a.range = g.content.rules.blink_range;
        } else {
            on_self = true;
        }
    }
    if (on_self) {
        Cmd c = order(Cmd::Cast);
        c.slot = slot;
        c.to = u.pos;
        send(g, c);
        ui.aim = Aim{};
    } else {
        ui.aim = a;
    }
}

void learn(Game& g, Ui& ui, uint8_t slot) {
    size_t i = 0, hi = 0;
    if (!me(g, i, hi)) return;
    const HeroData& h = g.heroes[hi];
    const AbilityDef& ab = g.content.abilities[(size_t)h.def * 4 + slot];
    uint8_t cur = h.ab_level[slot];
    if (h.skill_pts == 0) return ui.say("No skill points to spend");
    if (cur >= ab.max_level) return ui.say(ab.name + " is at its top level");
    if (h.level < ab.req_level[cur]) return ui.say(ab.name + " needs hero level " + std::to_string(ab.req_level[cur]));
    Cmd c = order(Cmd::LevelUp);
    c.slot = slot;
    send(g, c);
    ui.say("Learned " + ab.name + " level " + std::to_string(cur + 1));
}

/** The first item of the hero's recommended build (the one the bots follow) that you do not own yet. */
uint8_t next_build_item(const Game& g, const HeroData& h) {
    std::array<int, 256> owned{};
    for (const ItemSlot& s : h.items) owned[s.id]++;
    for (uint8_t item : g.content.heroes[h.def].build) {
        if (owned[item] > 0) owned[item]--;
        else return item;
    }
    return 0;
}

void buy(Game& g, Ui& ui, uint8_t item) {
    size_t i = 0, hi = 0;
    if (!me(g, i, hi) || item == 0) return;
    const HeroData& h = g.heroes[hi];
    const ItemDef& it = g.content.items[item];
    if (!g.units[i].alive) return ui.say("You are dead");
    if (!g.near_shop(i)) return ui.say("Return to your fountain to buy items");
    if (h.gold < it.cost) return ui.say(it.name + " costs " + std::to_string((int)it.cost) + " gold");
    bool room = h.free_slot() >= 0;
    for (const ItemSlot& s : h.items)
        if (it.stack && s.id == item && s.charges < 5) room = true;
    if (!room) return ui.say("Inventory is full");
    Cmd c = order(Cmd::Buy);
    c.item = item;
    send(g, c);
    ui.say("Bought " + it.name);
}

void buy_next(Game& g, Ui& ui) {
    size_t i = 0, hi = 0;
    if (!me(g, i, hi)) return;
    uint8_t item = next_build_item(g, g.heroes[hi]);
    if (item == 0) return ui.say("Your build is complete");
    buy(g, ui, item);
}

void buy_salve(Game& g, Ui& ui) {
    for (size_t k = 1; k < g.content.items.size(); k++)
        if (g.content.items[k].active == ACT_SALVE) return buy(g, ui, (uint8_t)k);
}

/** T: hand your hero to the bot, or take it back. */
void toggle_autopilot(Game& g, Ui& ui) {
    size_t i = 0, hi = 0;
    if (!me(g, i, hi)) return;
    g.heroes[hi].bot = !g.heroes[hi].bot;
    ui.aim = Aim{};
    ui.say(g.heroes[hi].bot ? "Autopilot on: T takes control back" : "You are in control");
}

// ------------------------------------------------------------------------------- ground markings

/**
 * Paint this frame's markings and say which square of ground they cover (centre x, z and side).
 * The browser paints the same things in drawDecal() (web/js/render3d.js), plus the health bars it
 * draws on its 2D overlay; here the bars lie on the ground in front of each unit.
 */
void paint_marks(Marks& mk, const Match& m, const Ui& ui, const Screen& sc, float alpha, bool fog, float t, float square[3]) {
    const Game& g = *m.g;
    const uint8_t pt = g.player_team;

    // the square of ground the camera sees
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
    const float corners[4][2] = {{0, 0}, {sc.w, 0}, {sc.w, sc.h}, {0, sc.h}};
    for (auto& c : corners) {
        Vec2 w = screen_to_world(sc, c[0], c[1]);
        x0 = std::min(x0, w.x), x1 = std::max(x1, w.x), z0 = std::min(z0, w.y), z1 = std::max(z1, w.y);
    }
    x0 = std::clamp(x0, -100.0f, WORLD + 100), x1 = std::clamp(x1, -100.0f, WORLD + 100);
    z0 = std::clamp(z0, -100.0f, WORLD + 100), z1 = std::clamp(z1, -100.0f, WORLD + 100);
    square[0] = (x0 + x1) / 2;
    square[1] = (z0 + z1) / 2;
    square[2] = std::max({200.0f, x1 - x0, z1 - z0});
    mk.begin(square[0], square[1], square[2]);

    auto team_color = [&](uint8_t team) { return team == pt ? ALLY : team == TEAM_NEUTRAL ? NEUTRAL : ENEMY; };
    size_t my_i = 0, my_hi = 0;
    bool have_me = me(g, my_i, my_hi);
    Vec2 my_pos = have_me ? drawn_pos(m, g.units[my_i], alpha) : Vec2();

    // spell areas on the ground
    for (const Zone& z : g.zones) {
        if (fog && z.team != pt && !seen(g, z.pos)) continue;
        Rgba c = team_color(z.team);
        mk.disc(z.pos.x, z.pos.y, z.radius, c.with_alpha(0.16f));
        mk.ring(z.pos.x, z.pos.y, z.radius, 3, c.with_alpha(0.75f));
    }

    // how far the enemy towers near you reach
    if (have_me && g.units[my_i].alive)
        for (const Unit& u : g.units)
            if (u.alive && u.kind == Kind::Tower && u.team != pt && u.pos.dist(my_pos) < u.range + 450)
                mk.ring(u.pos.x, u.pos.y, u.range, 3, ENEMY.with_alpha(0.3f));

    for (size_t i = 0; i < g.units.size(); i++) {
        const Unit& u = g.units[i];
        if (!u.alive || !shown(g, i, fog)) continue;
        Vec2 p = drawn_pos(m, u, alpha);
        if ((int)i == ui.hover && u.id != g.player) mk.ring(p.x, p.y, u.radius * 1.45f, 3.5f, team_color(u.team).with_alpha(0.9f));
        if (u.id == g.player) mk.ring(p.x, p.y, u.radius * 1.7f, 3.5f, GOLD.with_alpha(0.65f + 0.3f * std::sin(t * 5)));

        // health bar, on the ground in front of the unit; creeps only show one once they are hurt
        bool big = u.is_hero() || u.is_building();
        if (!big && u.hp >= u.max_hp) continue;
        float bw = std::max(34.0f, u.radius * 2.4f), bh = big ? 9.0f : 7.0f;
        float bx = p.x - bw / 2, by = p.y + u.radius + 9;
        mk.rect(bx - 2, by - 2, bw + 4, bh + 4, BLACK.with_alpha(0.7f));
        mk.rect(bx, by, bw * std::clamp(u.hp / u.max_hp, 0.0f, 1.0f), bh, team_color(u.team));
        if (u.id == g.player && u.max_mana > 0) {
            mk.rect(bx - 2, by + bh + 2, bw + 4, 8, BLACK.with_alpha(0.7f));
            mk.rect(bx, by + bh + 3, bw * std::clamp(u.mana / u.max_mana, 0.0f, 1.0f), 5, MANA);
        }
    }

    for (const Ping& p : ui.pings) {
        float k = 1 - p.life / 0.5f;
        mk.ring(p.at.x, p.at.y, 26 * (1 - k * 0.7f), 4, p.color.with_alpha(1 - k));
    }

    // what the next click will do
    if (!have_me || ui.aim.mode == Aim::Off) return;
    const Unit& u = g.units[my_i];
    Vec2 to = ui.mouse - my_pos;
    if (ui.aim.mode == Aim::AttackMove) {
        mk.ring(my_pos.x, my_pos.y, u.range + u.radius, 3, ENEMY.with_alpha(0.4f));
        mk.ring(ui.mouse.x, ui.mouse.y, 14, 4, ENEMY.with_alpha(0.95f));
        return;
    }
    const Aim& a = ui.aim;
    if (a.range > 0) mk.ring(my_pos.x, my_pos.y, a.range, 3, AIM.with_alpha(0.45f));
    switch (a.shape) {
        case Aim::OnUnit:
            if (ui.hover >= 0 && is_enemy(g, ui.hover)) {
                const Unit& v = g.units[(size_t)ui.hover];
                Vec2 vp = drawn_pos(m, v, alpha);
                mk.ring(vp.x, vp.y, v.radius * 1.7f, 5, AIM.with_alpha(0.95f));
            } else {
                mk.ring(ui.mouse.x, ui.mouse.y, 12, 3.5f, AIM.with_alpha(0.6f));
            }
            break;
        case Aim::Line: {
            Vec2 end = my_pos + to.norm() * std::max(a.range, 60.0f);
            mk.line(my_pos.x, my_pos.y, end.x, end.y, std::max(a.radius * 2, 12.0f), AIM.with_alpha(0.3f));
            break;
        }
        case Aim::Point:
            mk.disc(ui.mouse.x, ui.mouse.y, std::max(a.radius, 10.0f), AIM.with_alpha(0.2f));
            mk.ring(ui.mouse.x, ui.mouse.y, std::max(a.radius, 10.0f), 3.5f, AIM.with_alpha(0.9f));
            break;
        case Aim::Blink: {
            Vec2 dest = a.range > 0 && to.len() > a.range ? my_pos + to.norm() * a.range : ui.mouse;
            mk.line(my_pos.x, my_pos.y, dest.x, dest.y, 3, AIM.with_alpha(0.5f));
            mk.ring(dest.x, dest.y, std::max(a.radius, 18.0f), 3.5f, AIM.with_alpha(0.9f));
            break;
        }
    }
}

// ------------------------------------------------------------------------------- title bar

/** There is no HUD yet, so the window title carries it: your hero first, then the match. */
std::string title_text(const Game& g, const Ui& ui, int speed, bool paused, bool fog) {
    std::string s;
    char b[200];
    if (ui.now < ui.note_until && !ui.note.empty()) s += "[" + ui.note + "]   ";
    size_t i = 0, hi = 0;
    if (me(g, i, hi)) {
        const Unit& u = g.units[i];
        const HeroData& h = g.heroes[hi];
        const char* name = g.content.heroes[h.def].name.c_str();
        if (u.alive) std::snprintf(b, sizeof b, "%s L%u   HP %.0f/%.0f   MP %.0f/%.0f   %.0fg  ", name, h.level, std::max(u.hp, 0.0f), u.max_hp, u.mana, u.max_mana, std::floor(h.gold));
        else std::snprintf(b, sizeof b, "%s L%u   DEAD, back in %.0fs   %.0fg  ", name, h.level, std::ceil(h.respawn_t), std::floor(h.gold));
        s += b;
        for (size_t slot = 0; slot < 4; slot++) {
            const AbilityDef& ab = g.content.abilities[(size_t)h.def * 4 + slot];
            uint8_t lvl = h.ab_level[slot];
            if (lvl == 0) std::snprintf(b, sizeof b, " %c --", "QWER"[slot]);
            else if (h.ab_cd[slot] > 0.05f) std::snprintf(b, sizeof b, " %c %.0fs", "QWER"[slot], std::ceil(h.ab_cd[slot]));
            else if (u.mana < ab.mana[lvl - 1]) std::snprintf(b, sizeof b, " %c mana", "QWER"[slot]);
            else std::snprintf(b, sizeof b, " %c ok", "QWER"[slot]);
            s += b;
        }
        if (h.skill_pts > 0) {
            std::snprintf(b, sizeof b, "   +%u skill (Shift+key)", h.skill_pts);
            s += b;
        }
        if (uint8_t item = next_build_item(g, h); item != 0 && g.near_shop(i)) {
            std::snprintf(b, sizeof b, "   B: %s %.0fg", g.content.items[item].name.c_str(), g.content.items[item].cost);
            s += b;
        }
        if (h.bot) s += "   AUTOPILOT";
    }
    uint32_t kills[2] = {0, 0};
    for (const HeroData& h : g.heroes) kills[h.team & 1] += h.kills;
    int t = (int)g.time;
    std::snprintf(b, sizeof b, "   |   %d:%02d   %u : %u%s", t / 60, t % 60, kills[0], kills[1],
                  g.winner < 0 ? "" : g.winner == (int)g.player_team ? "   VICTORY" : "   DEFEAT");
    s += b;
    if (speed != 1) s += "   " + std::to_string(speed) + "x";
    if (paused) s += "   paused";
    if (!fog) s += "   no fog";
    return s;
}

void print_help(const Game& g) {
    size_t i = 0, hi = 0;
    if (me(g, i, hi)) {
        const HeroDef& d = g.content.heroes[g.heroes[hi].def];
        std::printf("You play %s, %s.", d.name.c_str(), d.title.c_str());
        for (size_t slot = 0; slot < 4; slot++) std::printf("  %c %s", "QWER"[slot], g.content.abilities[(size_t)g.heroes[hi].def * 4 + slot].name.c_str());
        std::printf("\n");
    }
    std::printf(
        "  Right-click     move / attack (also denies hurt allied creeps)\n"
        "  A, then click   attack-move\n"
        "  Q W E R         cast: click to aim, right-click or Esc backs out.  Shift+key learns it\n"
        "  1-6             use an inventory item\n"
        "  S / H           stop / hold position\n"
        "  B / C           at your fountain: buy the next item of your hero's build / a healing salve\n"
        "  T               autopilot: the bot plays your hero\n"
        "  Space / Y       centre on your hero / lock the camera to it\n"
        "  Arrows, wheel   pan (unlocks the camera) and zoom\n"
        "  P or Esc        pause.   [ and ]  game speed.   F  fog of war.   Ctrl/Cmd+N  new match\n");
    std::fflush(stdout);
}

}  // namespace app
