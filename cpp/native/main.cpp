// Lane Wars as a native desktop app: the C++ engine plus the C++ 3D renderer (cpp/render/) in a
// window, drawing through a native WebGPU library (Metal on macOS, Vulkan on Linux, D3D12 on Windows).
//
// Prototype: bots play both teams and you watch. There is no HUD yet; the window title shows the
// clock and the score. The browser build stays the full game.
//
//   cpp/build/lanewars [heroes_per_team] [seed]
//   cpp/build/lanewars 2 7 --shot frame.bmp 90   # play 90 s of game time, save one frame, quit
//
// Keys: WASD / arrows pan, mouse wheel zooms, Space follows your hero again, 1-4 game speed,
// P pause, F fog of war, N new match, Esc quit.
#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "game.hpp"
#include "gpu_native.hpp"

#if defined(__APPLE__)
#include <SDL_metal.h>
#endif

using namespace moba;

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
constexpr int DECAL_PX = 64;  // the ground markings canvas; empty in this prototype

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

WGPUSurface make_surface(WGPUInstance instance, SDL_Window* window) {
    WGPUSurfaceDescriptor d = WGPU_SURFACE_DESCRIPTOR_INIT;
#if defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    WGPUSurfaceSourceMetalLayer src = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    src.layer = SDL_Metal_GetLayer(view);
    d.nextInChain = &src.chain;
    return wgpuInstanceCreateSurface(instance, &d);
#else
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) return nullptr;
#if defined(_WIN32)
    WGPUSurfaceSourceWindowsHWND src = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    src.hinstance = wm.info.win.hinstance;
    src.hwnd = wm.info.win.window;
    d.nextInChain = &src.chain;
    return wgpuInstanceCreateSurface(instance, &d);
#else
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    if (wm.subsystem == SDL_SYSWM_WAYLAND) {
        WGPUSurfaceSourceWaylandSurface src = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
        src.display = wm.info.wl.display;
        src.surface = wm.info.wl.surface;
        d.nextInChain = &src.chain;
        return wgpuInstanceCreateSurface(instance, &d);
    }
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    if (wm.subsystem == SDL_SYSWM_X11) {
        WGPUSurfaceSourceXlibWindow src = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
        src.display = wm.info.x11.display;
        src.window = (uint64_t)wm.info.x11.window;
        d.nextInChain = &src.chain;
        return wgpuInstanceCreateSurface(instance, &d);
    }
#endif
    std::fprintf(stderr, "this SDL video driver (%s) is not supported; try SDL_VIDEODRIVER=x11\n", SDL_GetCurrentVideoDriver());
    return nullptr;
#endif
#endif
}

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

struct Match {
    std::unique_ptr<Game> g;
    std::vector<std::pair<uint32_t, Vec2>> prev;  // unit positions one tick ago, sorted by id
};

void start_match(Match& m, int per_team, uint32_t seed) {
    Config cfg;
    cfg.seed = seed;
    cfg.heroes_per_team = (uint8_t)per_team;
    cfg.fog = true;
    cfg.player_is_bot = true;  // watch mode: the bot plays your hero too
    m.g = std::make_unique<Game>(cfg);
    m.prev.clear();
    std::memcpy(r3d_terrain_buf(), m.g->map.terrain.data(), (size_t)GRID * GRID);
    if (!r3d_init(WORLD, GRID, CELL, (int)m.g->content.heroes.size(), BASE_RADIANT.x, BASE_RADIANT.y, BASE_DIRE.x, BASE_DIRE.y))
        std::fprintf(stderr, "3D renderer failed to start\n");
}

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

void update_title(SDL_Window* w, const Game& g, int speed, bool paused, bool fog) {
    uint32_t kills[2] = {0, 0};
    for (const HeroData& h : g.heroes) kills[h.team & 1] += h.kills;
    int t = (int)g.time;
    char s[200];
    std::snprintf(s, sizeof s, "Lane Wars   %d:%02d   Radiant %u : %u Dire%s   %dx%s%s", t / 60, t % 60, kills[0], kills[1],
                  g.winner < 0 ? "" : g.winner == 0 ? "   RADIANT WINS" : "   DIRE WINS", speed, paused ? "   paused" : "",
                  fog ? "" : "   no fog");
    SDL_SetWindowTitle(w, s);
}

// ------------------------------------------------------------------------------- main

int main(int argc, char** argv) {
    int per_team = argc > 1 ? std::clamp(std::atoi(argv[1]), 1, 5) : 2;
    uint32_t seed = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 7;
    const char* shot = nullptr;  // --shot file.bmp [game seconds]: for checking the renderer without a person watching
    float shot_at = 60;
    for (int a = 3; a < argc; a++)
        if (!std::strcmp(argv[a], "--shot") && a + 1 < argc) {
            shot = argv[++a];
            if (a + 1 < argc) shot_at = std::strtof(argv[++a], nullptr);
        }

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
    {
        Game probe(Config{});  // the map is the same for every match: paint it once
        paint_ground(probe.map);
        paint_emblems((int)probe.content.heroes.size());
    }
    std::vector<uint8_t> empty((size_t)DECAL_PX * DECAL_PX * 4, 0);
    gpu_native_set_canvas(1, empty.data(), DECAL_PX, DECAL_PX);
    start_match(m, per_team, seed);

    float cam_x = BASE_RADIANT.x, cam_y = BASE_RADIANT.y, zoom = 0.8f;
    bool follow = true, paused = false, fog = true, running = true;
    int speed = 1;
    double acc = 0, clock = 0, title_t = 0;
    int shot_tries = 0;
    Uint64 last = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_MOUSEWHEEL) zoom = std::clamp(zoom * std::pow(1.1f, (float)e.wheel.y), 0.32f, 1.7f);
            if (e.type == SDL_KEYDOWN && !e.key.repeat) switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_SPACE: follow = true; break;
                    case SDLK_p: paused = !paused; break;
                    case SDLK_f: fog = !fog; break;
                    case SDLK_1: speed = 1; break;
                    case SDLK_2: speed = 2; break;
                    case SDLK_3: speed = 4; break;
                    case SDLK_4: speed = 8; break;
                    case SDLK_n:
                        start_match(m, per_team, ++seed);
                        follow = true;
                        break;
                    default: break;
                }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = std::min(0.25, (double)(now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        clock += dt;

        // ---- simulation at its fixed 30 Hz
        if (shot) {  // fast-forward to the moment to capture, then draw that one frame
            while (m.g->time < shot_at && m.g->winner < 0) step(m);
            if (gpu_native_captured()) break;
            if (++shot_tries > 300) {  // the surface never gave us a frame to draw into
                std::fprintf(stderr, "--shot: no frame could be drawn\n");
                return 1;
            }
            gpu_native_capture(shot);
            dt = 1.0;  // let the camera settle on the hero
        } else if (!paused && m.g->winner < 0) {
            acc += dt * speed;
            for (int n = 0; acc >= DT && n < 64; n++) {
                step(m);
                acc -= DT;
            }
            acc = std::min(acc, (double)DT);
        }
        float alpha = paused || m.g->winner >= 0 ? 1.0f : (float)(acc / DT);

        // ---- camera
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        float pan = (float)dt * 900.0f / zoom;
        float mx = (float)((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]));
        float my = (float)((k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) - (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]));
        if (mx != 0 || my != 0) follow = false;
        cam_x = std::clamp(cam_x + mx * pan, 0.0f, WORLD);
        cam_y = std::clamp(cam_y + my * pan, 0.0f, WORLD);
        if (follow)
            if (auto pi = m.g->player_idx()) {
                Vec2 p = drawn_pos(m, m.g->units[*pi], alpha);
                cam_x += (p.x - cam_x) * std::min(1.0f, (float)dt * 8);
                cam_y += (p.y - cam_y) * std::min(1.0f, (float)dt * 8);
            }

        // ---- draw
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window, &w, &h);
#if defined(__APPLE__)
        SDL_Metal_GetDrawableSize(window, &pw, &ph);
#elif SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
#else
        pw = w;
        ph = h;
#endif
        if (pw <= 0 || ph <= 0) {
            SDL_Delay(16);
            continue;
        }
        gpu_native_resize(pw, ph);
        r3d_camera(cam_x, cam_y, zoom, (float)w, (float)h, 0, 0);
        auto [nu, np] = pack_frame(m, alpha, fog);
        r3d_draw(nu, np, cam_x, cam_y, 200, (float)clock, fog ? 1 : 0);
        wgpuInstanceProcessEvents(instance);

        if (clock - title_t > 0.25) {
            title_t = clock;
            update_title(window, *m.g, speed, paused, fog);
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (shot && gpu_errors) {
        std::fprintf(stderr, "--shot: %d WebGPU error(s)\n", gpu_errors);
        return 1;
    }
    if (shot) std::printf("saved %s at %d:%02d game time\n", shot, (int)m.g->time / 60, (int)m.g->time % 60);
    return 0;
}
