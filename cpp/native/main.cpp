// Lane Wars as a native desktop app: the C++ engine plus the C++ 3D renderer (cpp/render/) in a
// window, drawing through a native WebGPU library (Metal on macOS, Vulkan on Linux, D3D12 on Windows).
//
// You play one hero with the browser version's mouse and keys; bots play the others. There is no HUD
// yet: health bars and targeting are painted on the ground (marks.cpp), and the window title shows
// your hero's numbers and cooldowns. The browser build stays the full game (shop, scoreboard, mods).
//
//   cpp/build/lanewars [heroes_per_team] [seed] [hero]     # the keys are printed when it starts
//   cpp/build/lanewars 2 7 --watch                         # the bot plays your hero too
//   cpp/build/lanewars 2 7 --shot frame.bmp 90             # play 90 s of game time, save one frame, quit
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "game.hpp"
#include "gpu_native.hpp"
#include "marks.hpp"
#include "play.hpp"

#if defined(__APPLE__)
#include <SDL_metal.h>
#endif

using namespace moba;
using namespace app;

// ---- the renderer's entry points (cpp/render/render3d.cpp)
extern "C" {
unsigned char* r3d_terrain_buf();
float* r3d_frame_buf();
int r3d_max_units();
int r3d_init(float world_size, int grid_n, float cell_size, int heroes, float b0x, float b0y, float b1x, float b1y);
void r3d_camera(float x, float y, float zoom, float w, float h, float shake_x, float shake_z);
void r3d_draw(int n_units, int n_projs, float decal_x, float decal_z, float decal_side, float t, int fog_on);
}

constexpr int U_STRIDE = 16, P_STRIDE = 12;

// ------------------------------------------------------------------------------- canvases

struct Rgb {
    float r, g, b;
};

Rgb hex(uint32_t c) { return {(float)(c >> 16) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f}; }

/** Same sRGB -> linear curve as linear() in web/js/render3d.js. */
Rgb linear(Rgb c) {
    auto f = [](float v) { return v * v * (0.75f + 0.25f * v); };
    return {f(c.r), f(c.g), f(c.b)};
}

Rgb hsl(float h, float s, float l) {
    auto k = [&](float n) {
        float kk = std::fmod(n + h / 30.0f, 12.0f);
        float a = s * std::min(l, 1 - l);
        return l - a * std::max(-1.0f, std::min({kk - 3, 9 - kk, 1.0f}));
    };
    return {k(0), k(8), k(4)};
}

struct TeamColors {
    uint32_t main, light, dark;
};
const TeamColors TEAM[2] = {{0x4fd17a, 0xa8f5c2, 0x1b6e42}, {0xff5d52, 0xffb0a8, 0x842019}};

/** cellRand() from web/js/render.js: a stable hash noise per terrain cell. */
float cell_rand(int cx, int cy, int salt) {
    uint32_t h = (uint32_t)(cx + 101) * 73856093u ^ (uint32_t)(cy + 57) * 19349663u ^ (uint32_t)(salt + 13) * 83492791u;
    h = (h ^ (h >> 15)) * 2246822507u;
    h = (h ^ (h >> 13)) * 3266489909u;
    return (float)(h ^ (h >> 16)) / 4294967296.0f;
}

void put(std::vector<uint8_t>& px, size_t i, Rgb c, float a = 1.0f) {
    auto b = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    px[i * 4] = b(c.r);
    px[i * 4 + 1] = b(c.g);
    px[i * 4 + 2] = b(c.b);
    px[i * 4 + 3] = b(a);
}

/** The ground: buildTerrain() from web/js/render.js without the grass and pebble strokes. */
void paint_ground(const Map& map) {
    const float S = 0.75f, cp = CELL * S;
    const int size = (int)std::lround(WORLD * S);
    auto T = [&](int cx, int cy) { return cx < 0 || cy < 0 || cx >= GRID || cy >= GRID ? 0 : (int)map.terrain[(size_t)cy * GRID + cx]; };
    std::vector<uint8_t> px((size_t)size * size * 4);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int cx = (int)(x / cp), cy = (int)(y / cp), t = T(cx, cy);
            float n = cell_rand(cx, cy, 1);
            Rgb c = t == 0 ? hsl(150, 0.24f, (6 + n * 4) / 100)
                  : t == 1 ? hsl(96, 0.24f, (16 + n * 6) / 100)
                  : t == 2 ? hsl(36, 0.30f, (27 + n * 6) / 100)
                           : hsl(205, 0.55f, (23 + n * 8) / 100);
            if (t != 0) {  // soft shadow along every wall edge
                float fx = x / cp - cx, fy = y / cp - cy, keep = 1.0f;
                const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d : dirs) {
                    if (T(cx + d[0], cy + d[1]) != 0) continue;
                    float edge = d[0] > 0 ? 1 - fx : d[0] < 0 ? fx : d[1] > 0 ? 1 - fy : fy;  // 0 at the wall
                    keep *= 1 - 0.42f * std::max(0.0f, 1 - edge / 0.8f);
                }
                c = {c.r * keep, c.g * keep, c.b * keep};
            }
            put(px, (size_t)y * size + x, c);
        }
    gpu_native_set_canvas(0, px.data(), size, size);
}

/** Hero emblems: teams across, heroes down, 128 px tiles (drawHeroPortrait() without the glyphs). */
void paint_emblems(int heroes) {
    const int s = 128, w = 2 * s, h = s * std::max(1, heroes);
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (int hi = 0; hi < std::max(1, heroes); hi++)
        for (int t = 0; t < 2; t++)
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++) {
                    float dx = x + 0.5f - s / 2.0f, dy = y + 0.5f - s / 2.0f, d = std::sqrt(dx * dx + dy * dy) / s;
                    size_t i = (size_t)(hi * s + y) * w + t * s + x;
                    if (d > 0.485f) continue;
                    if (d > 0.435f) {
                        put(px, i, hex(0xe8c875));  // gold rim
                        continue;
                    }
                    // radial gradient from a highlight up and to the left
                    float gx = x + 0.5f - s * 0.35f, gy = y + 0.5f - s * 0.3f, g = std::sqrt(gx * gx + gy * gy) / (s * 0.5f);
                    Rgb a = hex(TEAM[t].light), m = hex(TEAM[t].main), k = hex(TEAM[t].dark);
                    auto mix = [](Rgb p, Rgb q, float f) { return Rgb{p.r + (q.r - p.r) * f, p.g + (q.g - p.g) * f, p.b + (q.b - p.b) * f}; };
                    put(px, i, g < 0.35f ? mix(a, m, g / 0.35f) : mix(m, k, std::min(1.0f, (g - 0.35f) / 0.65f)));
                }
    gpu_native_set_canvas(2, px.data(), w, h);
}

// ------------------------------------------------------------------------------- WebGPU setup

/** A WebGPU surface on the window (surface.cpp, which keeps the window-system headers to itself). */
WGPUSurface make_surface(WGPUInstance instance, SDL_Window* window);

int gpu_errors = 0;  // WebGPU validation errors so far; --shot fails on any

struct Request {
    bool done = false;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
};

/** `software`: ask for a CPU rasteriser (Mesa's lavapipe, WARP), for machines and CI runners without a GPU. */
WGPUAdapter request_adapter(WGPUInstance instance, WGPUSurface surface, bool software) {
    Request req;
    WGPURequestAdapterOptions o = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    o.powerPreference = WGPUPowerPreference_HighPerformance;
    o.forceFallbackAdapter = software;
    o.compatibleSurface = surface;
    WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView msg, void* ud, void*) {
        auto* r = (Request*)ud;
        if (status != WGPURequestAdapterStatus_Success) std::fprintf(stderr, "WebGPU adapter: %.*s\n", (int)msg.length, msg.data);
        r->adapter = status == WGPURequestAdapterStatus_Success ? a : nullptr;
        r->done = true;
    };
    cb.userdata1 = &req;
    wgpuInstanceRequestAdapter(instance, &o, cb);
    while (!req.done) wgpuInstanceProcessEvents(instance);
    return req.adapter;
}

WGPUDevice request_device(WGPUInstance instance, WGPUAdapter adapter) {
    Request req;
    WGPUDeviceDescriptor d = WGPU_DEVICE_DESCRIPTOR_INIT;
    d.label = WGPUStringView{"lanewars", WGPU_STRLEN};
    d.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType, WGPUStringView msg, void*, void*) {
        std::fprintf(stderr, "WebGPU: %.*s\n", (int)msg.length, msg.data);
        gpu_errors++;
    };
    d.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    d.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView msg, void*, void*) {
        if (reason != WGPUDeviceLostReason_Destroyed) std::fprintf(stderr, "WebGPU device lost: %.*s\n", (int)msg.length, msg.data);
    };
    WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice dev, WGPUStringView msg, void* ud, void*) {
        auto* r = (Request*)ud;
        if (status != WGPURequestDeviceStatus_Success) std::fprintf(stderr, "no WebGPU device: %.*s\n", (int)msg.length, msg.data);
        r->device = dev;
        r->done = true;
    };
    cb.userdata1 = &req;
    wgpuAdapterRequestDevice(adapter, &d, cb);
    while (!req.done) wgpuInstanceProcessEvents(instance);
    return req.device;
}

/** The browser's preferred canvas format is BGRA8Unorm (not sRGB): the shaders encode gamma themselves. */
WGPUTextureFormat pick_format(WGPUSurface surface, WGPUAdapter adapter) {
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(surface, adapter, &caps);
    WGPUTextureFormat f = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    for (size_t i = 0; i < caps.formatCount; i++)
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            f = caps.formats[i];
            break;
        }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return f;
}

// ------------------------------------------------------------------------------- the match

void start_match(Match& m, int per_team, uint32_t seed, int hero, bool watch) {
    Config cfg;
    cfg.seed = seed;
    cfg.heroes_per_team = (uint8_t)per_team;
    cfg.fog = true;
    cfg.player_hero = (uint8_t)hero;
    cfg.player_is_bot = watch;  // --watch: the bot plays your hero too
    m.g = std::make_unique<Game>(cfg);
    m.prev.clear();
    std::memcpy(r3d_terrain_buf(), m.g->map.terrain.data(), (size_t)GRID * GRID);
    if (!r3d_init(WORLD, GRID, CELL, (int)m.g->content.heroes.size(), BASE_RADIANT.x, BASE_RADIANT.y, BASE_DIRE.x, BASE_DIRE.y))
        std::fprintf(stderr, "3D renderer failed to start\n");
}

/** packFrame() from web/js/render3d.js: the visible units and projectiles into the renderer's frame buffer. */
std::pair<int, int> pack_frame(const Match& m, float alpha, bool fog) {
    const Game& g = *m.g;
    const int max = r3d_max_units();
    float* buf = r3d_frame_buf();
    const uint8_t pt = g.player_team;
    int nu = 0;
    for (size_t i = 0; i < g.units.size() && nu < max; i++) {
        const Unit& u = g.units[i];
        bool visible = !fog || u.team == pt || g.visible_to(pt, i);
        if (!visible && !u.is_building()) continue;
        float* b = buf + nu++ * U_STRIDE;
        Vec2 p = drawn_pos(m, u, alpha);
        float lx = 0, lz = 0;
        if (u.swing_t > 0 && u.swing_len > 0 && u.range < 150) {  // melee lunge while winding up an attack
            float l = std::sin((1 - u.swing_t / u.swing_len) * 3.14159265f) * 7;
            lx = std::cos(u.facing) * l;
            lz = std::sin(u.facing) * l;
        }
        b[0] = (float)u.id;
        b[1] = (float)(uint8_t)u.kind;  // the engine's Kind numbering is the renderer's
        b[2] = (float)u.team;
        b[3] = (float)u.def;
        b[4] = p.x;
        b[5] = p.y;
        b[6] = u.facing;
        b[7] = u.radius;
        b[8] = visible ? 1.0f : 0.0f;
        b[9] = u.alive ? 1.0f : 0.0f;
        b[10] = lx;
        b[11] = lz;
        b[12] = u.team == pt ? u.vision : 0;
        b[13] = u.is_hero() ? (float)g.content.heroes[u.def].primary : 0;
        b[14] = b[15] = 0;
    }
    // ability colours: a fixed palette here (the browser hashes the ability name into one)
    static const uint32_t PALETTE[] = {0x7cc7ff, 0xffb347, 0xc39bff, 0x7dffb0, 0xff7ab0, 0xfff07a};
    float* pb = buf + max * U_STRIDE;
    int np = 0;
    for (const Proj& p : g.projs) {
        if (np >= max) break;
        if (fog && !seen(g, p.pos)) continue;
        float* b = pb + np++ * P_STRIDE;
        bool attack = p.ability == 255;
        Rgb c = linear(hex(attack ? TEAM[p.team & 1].light : PALETTE[p.ability % 6]));
        b[0] = p.pos.x;
        b[1] = p.pos.y;
        b[2] = p.dir.x;
        b[3] = p.dir.y;
        b[4] = attack ? 4 : p.homing ? 7 : std::max(8.0f, p.radius * 0.5f);
        b[5] = attack ? 18 : p.homing ? 26 : 50;
        b[6] = attack ? 34 : 26;
        b[7] = c.r;
        b[8] = c.g;
        b[9] = c.b;
        b[10] = b[11] = 0;
    }
    return {nu, np};
}

// ------------------------------------------------------------------------------- main

int main(int argc, char** argv) {
    // lanewars [heroes_per_team] [seed] [hero] [--watch] [--shot file.bmp [game seconds]]
    int numbers[3] = {2, 7, 0}, n_numbers = 0;
    bool watch = false;
    const char* shot = nullptr;  // for checking the renderer without a person watching
    float shot_at = 60;
    // With --shot, input to play back two game seconds before the frame is saved, to test the controls:
    //   --mouse x y (window points)   --press keys (capitals = with Shift)   --click left|right
    float fake_mouse[2] = {-1, -1};
    const char* fake_keys = "";
    int fake_click = 0;
    for (int a = 1; a < argc; a++) {
        if (!std::strcmp(argv[a], "--watch")) {
            watch = true;
        } else if (!std::strcmp(argv[a], "--mouse") && a + 2 < argc) {
            fake_mouse[0] = std::strtof(argv[++a], nullptr);
            fake_mouse[1] = std::strtof(argv[++a], nullptr);
        } else if (!std::strcmp(argv[a], "--press") && a + 1 < argc) {
            fake_keys = argv[++a];
        } else if (!std::strcmp(argv[a], "--click") && a + 1 < argc) {
            fake_click = !std::strcmp(argv[++a], "right") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        } else if (!std::strcmp(argv[a], "--shot") && a + 1 < argc) {
            shot = argv[++a];
            if (a + 1 < argc && argv[a + 1][0] != '-') shot_at = std::strtof(argv[++a], nullptr);
        } else if (n_numbers < 3 && argv[a][0] >= '0' && argv[a][0] <= '9') {
            numbers[n_numbers++] = std::atoi(argv[a]);
        } else {
            std::fprintf(stderr, "usage: %s [heroes per team 1-5] [seed] [hero number] [--watch] [--shot file.bmp [game seconds]]\n", argv[0]);
            return 2;
        }
    }
    int per_team = std::clamp(numbers[0], 1, 5), hero = numbers[2];
    uint32_t seed = (uint32_t)numbers[1];
    const bool scripted = shot && (fake_mouse[0] >= 0 || *fake_keys || fake_click);
    if (shot && !scripted) watch = true;  // a saved frame should show a fight, not a hero waiting at the fountain

    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    SDL_Window* window = SDL_CreateWindow("Lane Wars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, flags);
    if (!window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }

    WGPUInstance instance = wgpuCreateInstance(nullptr);
    WGPUSurface surface = make_surface(instance, window);
    WGPUAdapter adapter = surface ? request_adapter(instance, surface, false) : nullptr;
    if (surface && !adapter) adapter = request_adapter(instance, surface, true);
    if (adapter) {
        WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
        if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
            const char* backend = info.backendType == WGPUBackendType_Metal ? "Metal"
                                : info.backendType == WGPUBackendType_Vulkan ? "Vulkan"
                                : info.backendType == WGPUBackendType_D3D12 ? "Direct3D 12"
                                : info.backendType == WGPUBackendType_OpenGL || info.backendType == WGPUBackendType_OpenGLES ? "OpenGL"
                                                                                                                           : "another backend";
            std::printf("WebGPU on %s: %.*s\n", backend, (int)info.device.length, info.device.data);
            wgpuAdapterInfoFreeMembers(info);
        }
    }
    WGPUDevice device = adapter ? request_device(instance, adapter) : nullptr;
    if (!device) {
        std::fprintf(stderr, "could not start WebGPU\n");
        return 1;
    }
    gpu_native_init(instance, device, surface, pick_format(surface, adapter));

    Match m;
    Marks marks;
    Ui ui;
    Screen sc;
    {
        Game probe(Config{});  // the map is the same for every match: paint it once
        paint_ground(probe.map);
        paint_emblems((int)probe.content.heroes.size());
        hero = std::clamp(hero, 0, (int)probe.content.heroes.size() - 1);
    }
    gpu_native_set_canvas(1, marks.pixels(), Marks::PX, Marks::PX);
    start_match(m, per_team, seed, hero, watch);
    if (!shot) print_help(*m.g);

    float cam_x = BASE_RADIANT.x, cam_y = BASE_RADIANT.y, zoom = 0.8f, alpha = 1;
    bool lock = true, paused = false, fog = true, running = true;
    int speed = 1, shot_tries = 0, shot_stage = 0;
    double acc = 0, clock = 0, title_t = -1;
    r3d_camera(cam_x, cam_y, zoom, sc.w, sc.h, 0, 0);
    Uint64 last = SDL_GetPerformanceCounter();

    auto on_click = [&](int button, float x, float y) {
        ui.mouse = screen_to_world(sc, x, y);
        ui.hover = pick_unit(m, alpha, fog, ui.mouse);
        if (button == SDL_BUTTON_RIGHT) right_click(*m.g, ui);
        if (button == SDL_BUTTON_LEFT) left_click(*m.g, ui);
    };
    auto on_key = [&](SDL_Keycode key, bool shift, bool ctrl) {
        Game& g = *m.g;
        auto ability = [&](uint8_t slot) { shift ? learn(g, ui, slot) : begin_cast(g, ui, slot); };
        if (ctrl) {
            if (key == SDLK_n) {
                start_match(m, per_team, ++seed, hero, watch);
                ui = Ui{};
                lock = true;
            }
            return;
        }
        switch (key) {
            case SDLK_q: ability(0); break;
            case SDLK_w: ability(1); break;
            case SDLK_e: ability(2); break;
            case SDLK_r: ability(3); break;
            case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5: case SDLK_6:
                begin_cast(g, ui, (uint8_t)(10 + key - SDLK_1));
                break;
            case SDLK_a: ui.aim = Aim{}, ui.aim.mode = Aim::AttackMove; break;
            case SDLK_s: ui.aim = Aim{}, send(g, order(Cmd::Stop)); break;
            case SDLK_h: ui.aim = Aim{}, send(g, order(Cmd::Hold)); break;
            case SDLK_b: buy_next(g, ui); break;
            case SDLK_c: buy_salve(g, ui); break;
            case SDLK_t: toggle_autopilot(g, ui); break;
            case SDLK_y: lock = !lock; break;
            case SDLK_SPACE:
                if (auto pi = g.player_idx()) cam_x = g.units[*pi].pos.x, cam_y = g.units[*pi].pos.y;
                break;
            case SDLK_ESCAPE:
                if (ui.aim.mode != Aim::Off) ui.aim = Aim{};
                else paused = !paused;
                break;
            case SDLK_p: paused = !paused; break;
            case SDLK_f: fog = !fog; break;
            case SDLK_LEFTBRACKET: speed = std::max(1, speed / 2); break;
            case SDLK_RIGHTBRACKET: speed = std::min(8, speed * 2); break;
            default: break;
        }
    };

    while (running) {
        Game& g = *m.g;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (shot) continue;
            if (e.type == SDL_MOUSEWHEEL) zoom = std::clamp(zoom * std::pow(1.1f, (float)e.wheel.y), 0.32f, 1.7f);
            if (e.type == SDL_MOUSEBUTTONDOWN) on_click(e.button.button, (float)e.button.x, (float)e.button.y);
            if (e.type == SDL_KEYDOWN && !e.key.repeat)
                on_key(e.key.keysym.sym, (e.key.keysym.mod & KMOD_SHIFT) != 0, (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0);
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = std::min(0.25, (double)(now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        clock += dt;
        ui.now = clock;

        // ---- simulation at its fixed 30 Hz
        if (shot) {  // fast-forward to the moment to capture, then draw that one frame
            if (shot_stage == 0) {  // to the moment of the scripted input; drawing this frame settles the camera
                while (g.time < (scripted ? shot_at - 2 : shot_at) && g.winner < 0) step(m);
            } else if (shot_stage == 1) {
                for (const char* c = fake_keys; *c; c++) {
                    on_key((SDL_Keycode)std::tolower(*c), std::isupper(*c) != 0, false);
                    step(m);  // one tick, so the next key sees this one's order applied
                }
                if (fake_click) on_click(fake_click, fake_mouse[0], fake_mouse[1]);
            } else {
                while (g.time < shot_at && g.winner < 0) step(m);
                if (gpu_native_captured()) break;
                if (++shot_tries > 300) {  // the surface never gave us a frame to draw into
                    std::fprintf(stderr, "--shot: no frame could be drawn\n");
                    return 1;
                }
                gpu_native_capture(shot);
            }
            shot_stage++;
            dt = 1.0;  // the camera jumps straight to the hero
        } else if (!paused && g.winner < 0) {
            acc += dt * speed;
            for (int n = 0; acc >= DT && n < 64; n++) {
                step(m);
                acc -= DT;
            }
            acc = std::min(acc, (double)DT);
        }
        alpha = paused || g.winner >= 0 ? 1.0f : (float)(acc / DT);

        // ---- window and mouse
        int w = 0, h = 0, pw = 0, ph = 0, mouse_x = 0, mouse_y = 0;
        SDL_GetWindowSize(window, &w, &h);
#if defined(__APPLE__)
        SDL_Metal_GetDrawableSize(window, &pw, &ph);
#elif SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
#else
        pw = w;
        ph = h;
#endif
        if (pw <= 0 || ph <= 0 || w <= 0 || h <= 0) {
            SDL_Delay(16);
            continue;
        }
        sc.w = (float)w;
        sc.h = (float)h;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        bool mouse_in = SDL_GetMouseFocus() == window;
        if (shot) {
            mouse_in = fake_mouse[0] >= 0;
            mouse_x = (int)fake_mouse[0];
            mouse_y = (int)fake_mouse[1];
        }

        // ---- camera: locked to your hero until you pan
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        float pan_x = (float)(k[SDL_SCANCODE_RIGHT] - k[SDL_SCANCODE_LEFT]), pan_y = (float)(k[SDL_SCANCODE_DOWN] - k[SDL_SCANCODE_UP]);
        if (pan_x != 0 || pan_y != 0) lock = false;
        if (!lock && mouse_in && !shot) {  // once unlocked, the window's edges pan too
            const int edge = 10;
            pan_x += (float)((mouse_x >= w - edge) - (mouse_x < edge));
            pan_y += (float)((mouse_y >= h - edge) - (mouse_y < edge));
        }
        float pan = (float)dt * 900.0f / zoom;
        cam_x = std::clamp(cam_x + std::clamp(pan_x, -1.0f, 1.0f) * pan, 0.0f, WORLD);
        cam_y = std::clamp(cam_y + std::clamp(pan_y, -1.0f, 1.0f) * pan, 0.0f, WORLD);
        if (lock)
            if (auto pi = g.player_idx()) {
                Vec2 p = drawn_pos(m, g.units[*pi], alpha);
                cam_x += (p.x - cam_x) * std::min(1.0f, (float)dt * 8);
                cam_y += (p.y - cam_y) * std::min(1.0f, (float)dt * 8);
            }
        r3d_camera(cam_x, cam_y, zoom, sc.w, sc.h, 0, 0);

        // ---- what the mouse is over, and the markings on the ground
        if (mouse_in) {
            ui.mouse = screen_to_world(sc, (float)mouse_x, (float)mouse_y);
            ui.hover = pick_unit(m, alpha, fog, ui.mouse);
        } else {
            ui.hover = -1;
        }
        for (Ping& p : ui.pings) p.life -= (float)dt;
        std::erase_if(ui.pings, [](const Ping& p) { return p.life <= 0; });
        float square[3];
        paint_marks(marks, m, ui, sc, alpha, fog, (float)clock, square);
        gpu_native_set_canvas(1, marks.pixels(), Marks::PX, Marks::PX);

        // ---- draw
        gpu_native_resize(pw, ph);
        auto [nu, np] = pack_frame(m, alpha, fog);
        r3d_draw(nu, np, square[0], square[1], square[2], (float)clock, fog ? 1 : 0);
        wgpuInstanceProcessEvents(instance);

        if (clock - title_t > 0.1) {
            title_t = clock;
            SDL_SetWindowTitle(window, title_text(g, ui, speed, paused, fog).c_str());
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (shot && gpu_errors) {
        std::fprintf(stderr, "--shot: %d WebGPU error(s)\n", gpu_errors);
        return 1;
    }
    if (shot) {
        ui.note_until = 1e30;  // show the last note whatever its age
        std::printf("saved %s at %d:%02d game time\ntitle: %s\n", shot, (int)m.g->time / 60, (int)m.g->time % 60,
                    title_text(*m.g, ui, speed, paused, fog).c_str());
    }
    return 0;
}
