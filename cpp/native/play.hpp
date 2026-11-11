// Playing a match in the native app: your hero's orders from the mouse and keyboard, the markings
// painted on the ground, and the title bar that stands in for a HUD. main.cpp owns the window and the
// frame loop and calls into this; nothing here touches SDL or WebGPU.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game.hpp"
#include "marks.hpp"

namespace app {

using namespace moba;

struct Match {
    std::unique_ptr<Game> g;
    std::vector<std::pair<uint32_t, Vec2>> prev;  // unit positions one tick ago, sorted by id
};

/** Advance the match one tick, remembering where every unit was for drawing between ticks. */
void step(Match& m);
/** Where to draw a unit between the last tick and this one (alpha 0..1). */
Vec2 drawn_pos(const Match& m, const Unit& u, float alpha);
/** Is this point inside the player team's vision? */
bool seen(const Game& g, Vec2 p);

/** The window in points: what the camera and the mouse are measured in. */
struct Screen {
    float w = 1280, h = 800;
};

/** Where a point of the window lies on the ground, from the camera the renderer was last given. */
Vec2 screen_to_world(const Screen& sc, float sx, float sy);

/** What the next left click will do. */
struct Aim {
    enum Mode { Off, AttackMove, Spell } mode = Off;
    enum Shape { OnUnit, Line, Point, Blink } shape = Point;
    uint8_t slot = 0;  // Spell: 0-3 ability, 10-15 inventory item
    float range = 0, radius = 0;
};

struct Ping {
    Vec2 at;
    Rgba color;
    float life;
};

struct Ui {
    Aim aim;
    std::vector<Ping> pings;  // rings where you clicked
    std::string note;         // why the last order was refused; shown in the title for a moment
    double note_until = 0, now = 0;
    Vec2 mouse;      // where the mouse points on the ground
    int hover = -1;  // index in Game::units of the unit under the mouse

    void say(std::string s) {
        note = std::move(s);
        note_until = now + 2.5;
    }
    void ping(Vec2 at, Rgba c) { pings.push_back({at, c, 0.5f}); }
};

/** The unit under a point on the ground, or -1. */
int pick_unit(const Match& m, float alpha, bool fog, Vec2 w);

Cmd order(Cmd::K k);
/** Queue an order for your hero; the engine applies it on the next tick, as it does for the browser. */
void send(Game& g, Cmd c);

void right_click(Game& g, Ui& ui);
void left_click(Game& g, Ui& ui);
/** Q W E R (slot 0-3) or an item key (slot 10-15): cast at once, or start aiming. */
void begin_cast(Game& g, Ui& ui, uint8_t slot);
void learn(Game& g, Ui& ui, uint8_t slot);
void buy_next(Game& g, Ui& ui);
void buy_salve(Game& g, Ui& ui);
void toggle_autopilot(Game& g, Ui& ui);

/** Paint this frame's markings and say which square of ground they cover (centre x, z and side). */
void paint_marks(Marks& mk, const Match& m, const Ui& ui, const Screen& sc, float alpha, bool fog, float t, float square[3]);

std::string title_text(const Game& g, const Ui& ui, int speed, bool paused, bool fog);
void print_help(const Game& g);

}  // namespace app
