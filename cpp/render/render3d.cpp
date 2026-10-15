// Lane Wars 3D renderer: draws a match in perspective with WebGPU, compiled to WebAssembly.
//
// It works with either engine. The page (web/js/render3d.js) takes each snapshot from whichever
// engine is running, packs the units and projectiles into this module's frame buffer in the layout
// below, and calls r3d_draw(). Nothing here knows about game rules; the simulation stays 2D (x, y on
// the ground plane), and this file lifts it into a scene with trees, unit models, shadows and fog.
//
// Freestanding C++: no standard library, no heap, no exceptions. All storage is static, GL calls are
// imported from the page (gpu.h), and the few math functions needed are defined below. That keeps the
// build to a plain `clang++ --target=wasm32` (or em++, wasi-sdk, zig) with no runtime to link.
//
// Coordinates: world x -> GL x, world y -> GL z, height -> GL y. Matrices are column-major.

#include "gpu.h"

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

typedef unsigned int u32;
typedef unsigned char u8;

// ------------------------------------------------------------------------------------------- libc bits

extern "C" __attribute__((weak)) void* memset(void* d, int c, unsigned long n) {
  u8* p = (u8*)d;
  while (n--) *p++ = (u8)c;
  return d;
}
extern "C" __attribute__((weak)) void* memcpy(void* d, const void* s, unsigned long n) {
  u8* p = (u8*)d;
  const u8* q = (const u8*)s;
  while (n--) *p++ = *q++;
  return d;
}

// ------------------------------------------------------------------------------------------- math

static const float PI = 3.14159265f;
static inline float fsqrt(float x) { return __builtin_sqrtf(x); }
static inline float ffloor(float x) { return __builtin_floorf(x); }
static inline float fabsf_(float x) { return x < 0 ? -x : x; }
static inline float fmin_(float a, float b) { return a < b ? a : b; }
static inline float fmax_(float a, float b) { return a > b ? a : b; }
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float wrap_pi(float x) { return x - 2 * PI * ffloor((x + PI) / (2 * PI)); }
static float fsin(float x) {
  x = wrap_pi(x);
  if (x > PI / 2) x = PI - x;
  else if (x < -PI / 2) x = -PI - x;
  float x2 = x * x;
  return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float fcos(float x) { return fsin(x + PI / 2); }

struct V3 { float x, y, z; };
static V3 v3(float x, float y, float z) { return V3{x, y, z}; }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) {
  float l = fsqrt(dot(a, a));
  return l > 0 ? v3(a.x / l, a.y / l, a.z / l) : a;
}

struct M4 { float m[16]; };
static M4 ident() {
  M4 r = {};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
  return r;
}
static M4 mul(const M4& a, const M4& b) {
  M4 r;
  for (int c = 0; c < 4; c++)
    for (int i = 0; i < 4; i++)
      r.m[c * 4 + i] = a.m[i] * b.m[c * 4] + a.m[4 + i] * b.m[c * 4 + 1] + a.m[8 + i] * b.m[c * 4 + 2] + a.m[12 + i] * b.m[c * 4 + 3];
  return r;
}
static M4 translate(float x, float y, float z) {
  M4 r = ident();
  r.m[12] = x; r.m[13] = y; r.m[14] = z;
  return r;
}
static M4 rot_x(float a) {
  M4 r = ident();
  float c = fcos(a), s = fsin(a);
  r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
  return r;
}
static M4 rot_y(float a) {
  M4 r = ident();
  float c = fcos(a), s = fsin(a);
  r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
  return r;
}
static M4 rot_z(float a) {
  M4 r = ident();
  float c = fcos(a), s = fsin(a);
  r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
  return r;
}
/** T * Rx * Ry * Rz * S, the order Three.js uses for a position / Euler XYZ / scale triple. */
static M4 compose(float x, float y, float z, float rx, float ry, float rz, float sx, float sy, float sz) {
  M4 r = translate(x, y, z);
  if (rx) r = mul(r, rot_x(rx));
  if (ry) r = mul(r, rot_y(ry));
  if (rz) r = mul(r, rot_z(rz));
  for (int i = 0; i < 4; i++) { r.m[i] *= sx; r.m[4 + i] *= sy; r.m[8 + i] *= sz; }
  return r;
}
static M4 perspective(float fovy, float aspect, float n, float f) {
  float t = fcos(fovy / 2) / fsin(fovy / 2);
  M4 r = {};
  r.m[0] = t / aspect; r.m[5] = t; r.m[10] = f / (n - f); r.m[11] = -1; r.m[14] = f * n / (n - f); // depth 0..1
  return r;
}
static M4 ortho(float l, float rr, float b, float t, float n, float f) {
  M4 r = ident();
  r.m[0] = 2 / (rr - l); r.m[5] = 2 / (t - b); r.m[10] = -1 / (f - n); // depth 0..1
  r.m[12] = -(rr + l) / (rr - l); r.m[13] = -(t + b) / (t - b); r.m[14] = -n / (f - n);
  return r;
}
static M4 look_at(V3 eye, V3 at, V3 up) {
  V3 f = norm(sub(at, eye)), s = norm(cross(f, up)), u = cross(s, f);
  M4 r = ident();
  r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
  r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
  r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye);
  return r;
}
static M4 inverse(const M4& a) {
  const float* m = a.m;
  M4 o;
  float* inv = o.m;
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det != 0) for (int i = 0; i < 16; i++) inv[i] /= det;
  return o;
}

/** Same hash as cellRand() in render.js, so trees stand where the 2D view paints them. */
static float cell_rand(int cx, int cy, int salt) {
  u32 h = (u32)(cx + 101) * 73856093u ^ (u32)(cy + 57) * 19349663u ^ (u32)(salt + 13) * 83492791u;
  h = (h ^ (h >> 15)) * 2246822507u;
  h = (h ^ (h >> 13)) * 3266489909u;
  return (float)(h ^ (h >> 16)) / 4294967296.0f;
}

// ------------------------------------------------------------------------------------------- colours

struct Col { float r, g, b; };
/** sRGB hex -> linear (close enough to the 2.2 curve for shading). */
static Col hex(u32 h) {
  float c[3] = {(float)((h >> 16) & 255) / 255, (float)((h >> 8) & 255) / 255, (float)(h & 255) / 255};
  for (float& v : c) v = v * v * (0.75f + 0.25f * v);
  return Col{c[0], c[1], c[2]};
}
static Col hsl(float h, float s, float l) {
  auto f = [&](float n) {
    float k = n + h * 12;
    k -= 12 * ffloor(k / 12);
    float a = s * fmin_(l, 1 - l);
    return l - a * fmax_(-1.f, fmin_(fmin_(k - 3, 9 - k), 1.f));
  };
  return hex(((u32)(f(0) * 255) << 16) | ((u32)(f(8) * 255) << 8) | (u32)(f(4) * 255));
}

struct TeamCol { u32 main, light, dark; };
static const TeamCol TEAMS[2] = {{0x4fd17a, 0xa8f5c2, 0x1b6e42}, {0xff5d52, 0xffb0a8, 0x842019}};
static const u32 NEUTRALS[4] = {0xc9a45f, 0xa8794a, 0x8fae5a, 0x8a7ab0};

// ------------------------------------------------------------------------------------------- meshes

// One static vertex buffer holds every mesh: position, normal, uv (8 floats per vertex).
enum Mesh { CYL, TAPER, CONE, BALL, BOX, HEX, OCT, GEM, ROCK, BOW, DISC, QUAD, TRUNK, TREE, BASIN, CYL32, SPIRE, N_MESH };
struct Range { int first, count; };
static Range meshes[N_MESH];
static const int MAX_VERTS = 24000;
static float verts[MAX_VERTS * 8];
static int n_verts;

static void vert(V3 p, V3 n, float u, float v) {
  if (n_verts >= MAX_VERTS) return;
  float* o = verts + n_verts++ * 8;
  o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = n.x; o[4] = n.y; o[5] = n.z; o[6] = u; o[7] = v;
}
static void flat_tri(V3 a, V3 b, V3 c) {
  V3 n = norm(cross(sub(b, a), sub(c, a)));
  vert(a, n, 0, 0); vert(b, n, 0, 0); vert(c, n, 0, 0);
}
static int begin_mesh() { return n_verts; }
static void end_mesh(Mesh m, int first) { meshes[m] = Range{first, n_verts - first}; }

/** Cylinder or cone along y in [-0.5, 0.5]. Smooth sides unless `flat`, capped both ends. */
static void build_cylinder(Mesh m, int seg, float r_top, float r_bot, bool flat) {
  int first = begin_mesh();
  float slope = r_bot - r_top; // outward lean of the side normal
  for (int i = 0; i < seg; i++) {
    float a0 = 2 * PI * i / seg, a1 = 2 * PI * (i + 1) / seg;
    float c0 = fcos(a0), s0 = fsin(a0), c1 = fcos(a1), s1 = fsin(a1);
    V3 t0 = v3(c0 * r_top, 0.5f, s0 * r_top), t1 = v3(c1 * r_top, 0.5f, s1 * r_top);
    V3 b0 = v3(c0 * r_bot, -0.5f, s0 * r_bot), b1 = v3(c1 * r_bot, -0.5f, s1 * r_bot);
    if (flat) {
      flat_tri(b0, t1, t0); flat_tri(b0, b1, t1);
    } else {
      V3 n0 = norm(v3(c0, slope, s0)), n1 = norm(v3(c1, slope, s1));
      vert(b0, n0, 0, 0); vert(t1, n1, 0, 0); vert(t0, n0, 0, 0);
      vert(b0, n0, 0, 0); vert(b1, n1, 0, 0); vert(t1, n1, 0, 0);
    }
    if (r_top > 0) { V3 n = v3(0, 1, 0); vert(v3(0, 0.5f, 0), n, 0, 0); vert(t0, n, 0, 0); vert(t1, n, 0, 0); }
    if (r_bot > 0) { V3 n = v3(0, -1, 0); vert(v3(0, -0.5f, 0), n, 0, 0); vert(b1, n, 0, 0); vert(b0, n, 0, 0); }
  }
  end_mesh(m, first);
}

static void build_sphere(Mesh m, int lon, int lat) {
  int first = begin_mesh();
  auto p = [&](int i, int j) {
    float th = PI * j / lat, ph = 2 * PI * i / lon;
    return v3(fsin(th) * fcos(ph), fcos(th), fsin(th) * fsin(ph));
  };
  for (int j = 0; j < lat; j++)
    for (int i = 0; i < lon; i++) {
      V3 a = p(i, j), b = p(i + 1, j), c = p(i + 1, j + 1), d = p(i, j + 1);
      vert(a, a, 0, 0); vert(c, c, 0, 0); vert(b, b, 0, 0);
      vert(a, a, 0, 0); vert(d, d, 0, 0); vert(c, c, 0, 0);
    }
  end_mesh(m, first);
}

static void build_box() {
  int first = begin_mesh();
  for (int axis = 0; axis < 3; axis++)
    for (int sgn = -1; sgn <= 1; sgn += 2) {
      auto P = [&](float u, float v) {
        float c[3];
        c[axis] = 0.5f * sgn; c[(axis + 1) % 3] = u; c[(axis + 2) % 3] = v;
        return v3(c[0], c[1], c[2]);
      };
      V3 a = P(-0.5f, -0.5f), b = P(0.5f, -0.5f), c = P(0.5f, 0.5f), d = P(-0.5f, 0.5f);
      if (sgn > 0) { flat_tri(a, b, c); flat_tri(a, c, d); } else { flat_tri(a, c, b); flat_tri(a, d, c); }
    }
  end_mesh(BOX, first);
}

static void build_gem() {
  int first = begin_mesh();
  V3 top = v3(0, 1, 0), bot = v3(0, -1, 0);
  V3 ring[4] = {v3(1, 0, 0), v3(0, 0, 1), v3(-1, 0, 0), v3(0, 0, -1)};
  for (int i = 0; i < 4; i++) {
    V3 a = ring[i], b = ring[(i + 1) % 4];
    flat_tri(top, b, a);
    flat_tri(bot, a, b);
  }
  end_mesh(GEM, first);
}

/** A lumpy low-poly ball for jungle creeps (icosahedron). */
static void build_rock() {
  int first = begin_mesh();
  const float t = 1.618034f;
  V3 v[12] = {v3(-1, t, 0), v3(1, t, 0), v3(-1, -t, 0), v3(1, -t, 0), v3(0, -1, t), v3(0, 1, t),
              v3(0, -1, -t), v3(0, 1, -t), v3(t, 0, -1), v3(t, 0, 1), v3(-t, 0, -1), v3(-t, 0, 1)};
  const int f[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                        {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                        {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
  for (auto& p : v) p = norm(p);
  for (auto& tri : f) flat_tri(v[tri[0]], v[tri[2]], v[tri[1]]);
  end_mesh(ROCK, first);
}

/** Half a torus in the xy plane bulging toward +x: a bow held upright. */
static void build_bow() {
  int first = begin_mesh();
  const int seg = 12, tube = 6;
  const float tr = 0.08f;
  auto P = [&](int i, int j, V3* n) {
    float a = -PI / 2 + PI * i / seg, b = 2 * PI * j / tube;
    V3 c = v3(fcos(a), fsin(a), 0);
    *n = v3(c.x * fcos(b), c.y * fcos(b), fsin(b));
    return v3(c.x + n->x * tr, c.y + n->y * tr, n->z * tr);
  };
  for (int i = 0; i < seg; i++)
    for (int j = 0; j < tube; j++) {
      V3 na, nb, nc, nd;
      V3 a = P(i, j, &na), b = P(i + 1, j, &nb), c = P(i + 1, j + 1, &nc), d = P(i, j + 1, &nd);
      vert(a, na, 0, 0); vert(b, nb, 0, 0); vert(c, nc, 0, 0);
      vert(a, na, 0, 0); vert(c, nc, 0, 0); vert(d, nd, 0, 0);
    }
  end_mesh(BOW, first);
}

/** Unit disc in the yz plane facing +x, with uvs for a portrait seen from the front. */
static void build_disc() {
  int first = begin_mesh();
  const int seg = 28;
  V3 n = v3(1, 0, 0);
  for (int i = 0; i < seg; i++) {
    float a0 = 2 * PI * i / seg, a1 = 2 * PI * (i + 1) / seg;
    float y0 = fsin(a0), z0 = fcos(a0), y1 = fsin(a1), z1 = fcos(a1);
    vert(v3(0, 0, 0), n, 0.5f, 0.5f);
    vert(v3(0, y0, z0), n, 0.5f - z0 * 0.5f, 0.5f - y0 * 0.5f);
    vert(v3(0, y1, z1), n, 0.5f - z1 * 0.5f, 0.5f - y1 * 0.5f);
  }
  end_mesh(DISC, first);
}

/** Unit square on the ground (xz), uv (0,0) at the -x,-z corner. */
static void build_quad() {
  int first = begin_mesh();
  V3 n = v3(0, 1, 0);
  vert(v3(-0.5f, 0, -0.5f), n, 0, 0); vert(v3(0.5f, 0, 0.5f), n, 1, 1); vert(v3(0.5f, 0, -0.5f), n, 1, 0);
  vert(v3(-0.5f, 0, -0.5f), n, 0, 0); vert(v3(-0.5f, 0, 0.5f), n, 0, 1); vert(v3(0.5f, 0, 0.5f), n, 1, 1);
  end_mesh(QUAD, first);
}

static void build_meshes() {
  n_verts = 0;
  build_cylinder(CYL, 14, 1, 1, false);
  build_cylinder(TAPER, 14, 0.72f, 1, false);
  build_cylinder(CONE, 12, 0, 1, false);
  build_sphere(BALL, 18, 12);
  build_box();
  build_cylinder(HEX, 6, 1, 1.12f, true);
  build_cylinder(OCT, 8, 1, 1.1f, true);
  build_gem();
  build_rock();
  build_bow();
  build_disc();
  build_quad();
  build_cylinder(TRUNK, 6, 0.16f, 0.24f, true);
  build_cylinder(TREE, 7, 0, 1, true);
  build_cylinder(BASIN, 32, 0.897f, 1, false);
  build_cylinder(CYL32, 32, 1, 1, false);
  build_cylinder(SPIRE, 8, 0.5f, 1, true);
}

// ------------------------------------------------------------------------------------------- instances

// Every drawn object is an instance: a model matrix, a colour (linear rgb + opacity) and four extra
// floats (lit: x = emissive strength; unlit: a uv rectangle into the bound texture, or x < 0 for none).
enum Pass { LIT, LIT_FADED, UNLIT, ADDITIVE, N_PASS };
struct Inst { float m[16]; float col[4]; float ext[4]; };
struct Batch { int mesh, first, count; };

static const int MAX_TREES = 16000;
static Inst tree_inst[MAX_TREES * 3];
static Batch tree_batches[3];

static const int MAX_INST = 16384;
static Inst scratch[MAX_INST];
static u8 scratch_key[MAX_INST]; // pass * N_MESH + mesh
static int n_scratch;
static Inst frame_inst[MAX_INST + 2]; // + ground and decal quads at the end
static Batch batches[N_PASS][N_MESH];

static void push(int pass, Mesh mesh, const M4& m, Col c, float alpha, float e0 = 0, float e1 = 0, float e2 = 0, float e3 = 0) {
  if (n_scratch >= MAX_INST) return;
  if (pass == LIT && alpha < 1) pass = LIT_FADED;
  Inst& o = scratch[n_scratch];
  scratch_key[n_scratch++] = (u8)(pass * N_MESH + mesh);
  memcpy(o.m, m.m, sizeof o.m);
  o.col[0] = c.r; o.col[1] = c.g; o.col[2] = c.b; o.col[3] = alpha;
  o.ext[0] = pass == ADDITIVE ? -2 : e0; // glows: untextured, soft edged
  o.ext[1] = e1; o.ext[2] = e2; o.ext[3] = e3;
}

/** Group this frame's instances by pass and mesh (counting sort) so each group is one draw call. */
static void sort_instances() {
  int counts[N_PASS * N_MESH] = {}, start[N_PASS * N_MESH];
  for (int i = 0; i < n_scratch; i++) counts[scratch_key[i]]++;
  int at = 0;
  for (int k = 0; k < N_PASS * N_MESH; k++) {
    start[k] = at;
    batches[k / N_MESH][k % N_MESH] = Batch{k % N_MESH, at, counts[k]};
    at += counts[k];
  }
  for (int i = 0; i < n_scratch; i++) frame_inst[start[scratch_key[i]]++] = scratch[i];
}

// ------------------------------------------------------------------------------------------- frame input

// Written by the page before each r3d_draw(). Kinds use the renderer's own numbering (the page maps
// the engine's kind names onto these).
enum Kind { K_HERO, K_MELEE, K_RANGED, K_SIEGE, K_NEUTRAL, K_TOWER, K_ANCIENT };
enum UnitField { U_ID, U_KIND, U_TEAM, U_DEF, U_X, U_Z, U_FACING, U_RADIUS, U_VISIBLE, U_ALIVE, U_LUNGE_X, U_LUNGE_Z,
                 U_VISION, U_PRIMARY, U_STRIDE = 16 };
enum ProjField { P_X, P_Z, P_DX, P_DZ, P_RAD, P_LEN, P_HEIGHT, P_R, P_G, P_B, P_STRIDE = 12 };
static const int MAX_UNITS = 1024, MAX_PROJS = 1024;
static float frame_buf[MAX_UNITS * U_STRIDE + MAX_PROJS * P_STRIDE];

static const int MAX_GRID = 512;
static u8 terrain[MAX_GRID * MAX_GRID];

// Camera block the page reads for picking: view-projection, its inverse, eye position.
static float cam_block[36];

// ------------------------------------------------------------------------------------------- state

static float world, cell;
static int grid, n_heroes;
static float bases[2][2];
static const float FOV = 42 * PI / 180, PITCH = 58 * PI / 180;
static M4 view_proj, sun_vp;
static float cam_x, cam_y, cam_zoom = 0.8f, view_h = 1;

enum Pipe { PIPE_DEPTH, PIPE_LIT, PIPE_LIT_FADED, PIPE_UNLIT, PIPE_DECAL, PIPE_ADD, N_PIPE };
static int pipes[N_PIPE];
static int g_depth, g_lit, g_lit_faded, g_emblem, g_decal, g_add; // bind groups
static int mesh_vbo, tree_vbo, inst_vbo, ubo;
static int tex_ground, tex_decal, tex_emblem, tex_vis, tex_shadow;
struct Uniforms { float vp[16], sun_vp[16], sun_dir[4], params[4], eye[4]; }; // params: fog on, world size
static Uniforms uni;
static const int SHADOW = 2048, VIS = 160;
static u8 vis_buf[VIS * VIS];
static const V3 SUN_DIR = {-0.3618f, 0.8040f, 0.4422f}; // normalised (-0.45, 1, 0.55)

// Per-unit animation state (smoothed facing, walk bob), keyed by unit id; rebuilt every frame.
struct UState { int id; float yaw, walk, lx, lz; bool used; };
static const int US_SLOTS = 4096;
static UState ustate[2][US_SLOTS];
static int us_cur;

static UState* find_state(UState* table, int id) {
  u32 h = ((u32)id * 2654435761u) & (US_SLOTS - 1);
  for (int i = 0; i < US_SLOTS; i++) {
    UState* s = &table[(h + i) & (US_SLOTS - 1)];
    if (!s->used || s->id == id) return s;
  }
  return nullptr;
}

// ------------------------------------------------------------------------------------------- shaders

static const char WGSL[] = R"(
struct U { vp: mat4x4f, sunVp: mat4x4f, sunDir: vec4f, params: vec4f, eye: vec4f };
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var tex: texture_2d<f32>;
@group(0) @binding(3) var shadowSamp: sampler_comparison;
@group(0) @binding(4) var shadowTex: texture_depth_2d;
@group(0) @binding(5) var visTex: texture_2d<f32>;

struct VIn {
  @location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f,
  @location(3) m0: vec4f, @location(4) m1: vec4f, @location(5) m2: vec4f, @location(6) m3: vec4f,
  @location(7) col: vec4f, @location(8) ext: vec4f,
};
struct Out {
  @builtin(position) pos: vec4f,
  @location(0) w: vec3f, @location(1) n: vec3f, @location(2) uv: vec2f,
  @location(3) col: vec4f, @location(4) ext: vec4f, @location(5) sun: vec4f,
};

fn model(v: VIn) -> mat4x4f { return mat4x4f(v.m0, v.m1, v.m2, v.m3); }

@vertex fn vs_lit(v: VIn) -> Out {
  let M = model(v);
  let w = M * vec4f(v.pos, 1.0);
  let c = mat3x3f(M[0].xyz, M[1].xyz, M[2].xyz);
  let cof = mat3x3f(cross(c[1], c[2]), cross(c[2], c[0]), cross(c[0], c[1])); // inverse-transpose up to scale
  var o: Out;
  o.pos = u.vp * w; o.w = w.xyz; o.n = cof * v.nrm; o.uv = v.uv; o.col = v.col; o.ext = v.ext; o.sun = u.sunVp * w;
  return o;
}

fn shadowAt(p: vec4f) -> f32 {
  let q = p.xyz / p.w;
  let uv = vec2f(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
  if (any(uv < vec2f(0.0)) || any(uv > vec2f(1.0)) || q.z > 1.0) { return 1.0; }
  var s = 0.0;
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      s += textureSampleCompareLevel(shadowTex, shadowSamp, uv + vec2f(f32(x), f32(y)) / 2048.0, q.z - 0.0015);
    }
  }
  return s / 9.0;
}

@fragment fn fs_lit(i: Out) -> @location(0) vec4f {
  let texel = textureSample(tex, samp, i.uv).rgb;
  let vis = textureSample(visTex, samp, i.w.xz / u.params.y).r;
  let base = i.col.rgb * mix(vec3f(1.0), texel, i.ext.y);
  let n = normalize(i.n);
  let hemi = mix(vec3f(0.06, 0.075, 0.045), vec3f(0.42, 0.5, 0.72), n.y * 0.5 + 0.5);
  let sun = max(dot(n, u.sunDir.xyz), 0.0) * shadowAt(i.sun);
  var c = base * (hemi + vec3f(1.0, 0.9, 0.74) * 1.9 * sun) + i.col.rgb * i.ext.x;
  c *= mix(1.0, mix(0.28, 1.0, vis), u.params.x);
  c *= 1.1;
  c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
  return vec4f(pow(clamp(c, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)), i.col.a);
}

@vertex fn vs_unlit(v: VIn) -> Out {
  let M = model(v);
  let w = M * vec4f(v.pos, 1.0);
  var o: Out;
  o.pos = u.vp * w; o.w = w.xyz; o.n = mat3x3f(M[0].xyz, M[1].xyz, M[2].xyz) * v.nrm; o.uv = v.uv; o.col = v.col; o.ext = v.ext;
  return o;
}

@fragment fn fs_unlit(i: Out) -> @location(0) vec4f {
  let t = textureSample(tex, samp, mix(i.ext.xy, i.ext.zw, i.uv));
  var c = i.col * select(vec4f(1.0), t, i.ext.x >= 0.0);
  if (i.ext.x < -1.5) { // glow: fade toward the silhouette so the ball reads as soft light
    let facing = abs(dot(normalize(i.n), normalize(u.eye.xyz - i.w)));
    c.a *= facing * facing * facing;
  }
  return vec4f(pow(c.rgb, vec3f(1.0 / 2.2)), c.a);
}

@vertex fn vs_depth(v: VIn) -> @builtin(position) vec4f {
  return u.sunVp * model(v) * vec4f(v.pos, 1.0);
}
)";

static int len(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int pipeline(int shader, const char* vs, const char* fs, int target, int blend, int depth_write, int bias = 0) {
  return gpu_pipeline(shader, vs, len(vs), fs, len(fs), target, blend, depth_write, bias);
}

// ------------------------------------------------------------------------------------------- scenery

static void build_trees() {
  int n = 0;
  auto T = [&](int cx, int cy) -> int { return cx < 0 || cy < 0 || cx >= grid || cy >= grid ? 0 : terrain[cy * grid + cx]; };
  Inst* trunk = tree_inst;
  Inst* low = tree_inst + MAX_TREES;
  Inst* high = tree_inst + 2 * MAX_TREES;
  for (int cy = 0; cy < grid; cy++)
    for (int cx = 0; cx < grid; cx++) {
      if (T(cx, cy) != 0) continue;
      int open = 0;
      for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) open += T(cx + dx, cy + dy) != 0;
      int count = open > 0 ? 2 + (int)(cell_rand(cx, cy, 40) * 2) : 1;
      for (int k = 0; k < count && n < MAX_TREES; k++, n++) {
        float x = (cx + cell_rand(cx, cy, 50 + k)) * cell, z = (cy + cell_rand(cx, cy, 60 + k)) * cell;
        float r = cell * (0.55f + cell_rand(cx, cy, 70 + k) * 0.5f), h = 46 + cell_rand(cx, cy, 80 + k) * 40;
        float c = cell_rand(cx, cy, 90 + k), yaw = c * 6.28f;
        Col leaf = hsl(0.36f + c * 0.06f, 0.42f, (open > 0 ? 0.2f : 0.14f) + c * 0.06f);
        Col leaf2 = hsl(0.36f + c * 0.06f, 0.42f, (open > 0 ? 0.24f : 0.18f) + c * 0.06f);
        Col bark = hex(0x4a3424);
        M4 m;
        Inst* dst[3] = {trunk + n, low + n, high + n};
        Col cols[3] = {bark, leaf, leaf2};
        float ys[3] = {h * 0.18f, h * 0.5f, h * 0.8f}, ss[3][2] = {{r, h * 0.36f}, {r * 1.05f, h * 0.6f}, {r * 0.7f, h * 0.42f}};
        for (int p = 0; p < 3; p++) {
          m = compose(x, ys[p], z, 0, yaw, 0, ss[p][0], ss[p][1], ss[p][0]);
          memcpy(dst[p]->m, m.m, sizeof m.m);
          dst[p]->col[0] = cols[p].r; dst[p]->col[1] = cols[p].g; dst[p]->col[2] = cols[p].b; dst[p]->col[3] = 1;
          dst[p]->ext[0] = dst[p]->ext[1] = dst[p]->ext[2] = dst[p]->ext[3] = 0;
        }
      }
    }
  // pack the three groups next to each other
  memcpy(tree_inst + n, low, n * sizeof(Inst));
  memcpy(tree_inst + 2 * n, high, n * sizeof(Inst));
  tree_batches[0] = Batch{TRUNK, 0, n};
  tree_batches[1] = Batch{TREE, n, n};
  tree_batches[2] = Batch{TREE, 2 * n, n};
  gpu_write_buffer(tree_vbo, 0, tree_inst, 3 * n * (int)sizeof(Inst));
}

static void emit_fountains(float t) {
  for (int team = 0; team < 2; team++) {
    float bx = bases[team][0], bz = bases[team][1];
    Col main = hex(TEAMS[team].main), light = hex(TEAMS[team].light);
    push(LIT, BASIN, compose(bx, 5, bz, 0, 0, 0, 58, 10, 58), hex(0x7d8088), 1);
    push(LIT, CYL32, compose(bx, 10.5f, bz, 0, 0, 0, 46, 2, 46), hex(0x4ea2e0), 1, 0.35f);
    push(LIT, SPIRE, compose(bx, 28, bz, 0, 0, 0, 8, 40, 8), hex(0x9a9da5), 1);
    float y = 54 + fsin(t * 1.5f + team) * 2;
    push(LIT, BALL, compose(bx, y, bz, 0, 0, 0, 9, 9, 9), light, 1, 1.6f);
    push(ADDITIVE, BALL, compose(bx, y, bz, 0, 0, 0, 22, 22, 22), main, 0.6f);
  }
}

// ------------------------------------------------------------------------------------------- unit models

static float unit_height(int kind, float r) {
  switch (kind) {
    case K_HERO: return r * 2.6f;
    case K_SIEGE: return r * 2.2f;
    case K_TOWER: return r * 3.7f;
    case K_ANCIENT: return r * 2.6f;
    case K_NEUTRAL: return r * 2.0f;
    default: return r * 2.4f;
  }
}

/** Glowing crystal: lit emissive core plus an additive halo. */
static void glow(const M4& parent, float x, float y, float z, float sx, float sy, float sz, float spin, int team, float pulse, float alpha, Mesh mesh = GEM) {
  Col main = hex(TEAMS[team].main), light = hex(TEAMS[team].light);
  push(LIT, mesh, mul(parent, compose(x, y, z, 0, spin, 0, sx, sy, sz)), light, alpha, pulse);
  float h = fmax_(sx, sy) * 2.0f;
  push(ADDITIVE, BALL, mul(parent, compose(x, y, z, 0, 0, 0, h, h, h)), main, 0.5f * alpha);
}

static void emit_unit(const float* u, float t) {
  int id = (int)u[U_ID], kind = (int)u[U_KIND], team = (int)u[U_TEAM], def = (int)u[U_DEF];
  float r = u[U_RADIUS], x = u[U_X], z = u[U_Z];
  if (team < 0) team = 0;
  bool building = kind == K_TOWER || kind == K_ANCIENT;
  int tc = team > 1 ? 0 : team;

  // smoothed facing and walk cycle, carried over from last frame
  UState* prev = find_state(ustate[us_cur ^ 1], id);
  UState* s = find_state(ustate[us_cur], id);
  if (!s) return;
  float want = -u[U_FACING];
  if (prev && prev->used && prev->id == id) {
    *s = *prev;
    s->yaw += wrap_pi(want - s->yaw) * 0.35f;
    float moved = fsqrt((x - s->lx) * (x - s->lx) + (z - s->lz) * (z - s->lz));
    s->walk = s->walk * 0.85f + (moved > 0.2f ? 0.15f : 0);
  } else {
    s->yaw = want;
    s->walk = 0;
  }
  s->id = id; s->used = true; s->lx = x; s->lz = z;
  if (u[U_ALIVE] <= 0) return;

  float alpha = u[U_VISIBLE] > 0 ? 1 : 0.55f;
  M4 base = mul(translate(x + u[U_LUNGE_X], 0, z + u[U_LUNGE_Z]), rot_y(building ? 0 : s->yaw));
  float bob = fabsf_(fsin(t * 9 + id)) * r * 0.12f * s->walk;
  M4 body = mul(base, mul(translate(0, bob, 0), rot_z(-0.08f * s->walk)));
  float pulse = 1.8f + fsin(t * 3 + id) * 0.5f;

  Col main = hex(team > 1 ? NEUTRALS[def & 3] : TEAMS[team].main), dark = hex(TEAMS[tc].dark);
  auto P = [&](Mesh mesh, Col c, float px, float py, float pz, float sx, float sy, float sz, float rx = 0, float ry = 0, float rz = 0) {
    push(LIT, mesh, mul(body, compose(px * r, py * r, pz * r, rx, ry, rz, sx * r, sy * r, sz * r)), c, alpha);
  };

  switch (kind) {
    case K_HERO: {
      Col metal = hex(0xc9ccd4), skin = hex(0xe6c2a0);
      P(TAPER, main, 0, 1.0f, 0, 0.62f, 1.3f, 0.62f);
      P(CYL, dark, 0, 0.25f, 0, 0.66f, 0.5f, 0.66f);
      P(BALL, skin, 0, 1.92f, 0, 0.4f, 0.42f, 0.4f);
      P(BALL, main, -0.05f, 1.6f, 0, 0.82f, 0.3f, 0.82f);
      if (n_heroes > 0) {
        float u0 = tc * 0.5f, v0 = (float)def / n_heroes;
        push(UNLIT, DISC, mul(body, compose(0.66f * r, 1.12f * r, 0, 0, 0, 0, 1, r * 0.42f, r * 0.42f)), Col{1, 1, 1}, alpha,
             u0, v0, u0 + 0.5f, v0 + 1.0f / n_heroes);
      }
      int primary = (int)u[U_PRIMARY];
      if (primary == 0) { // strength: sword and shield
        P(BOX, metal, 0.55f, 1.05f, 0.85f, 1.5f, 0.12f, 0.18f);
        P(CYL, metal, 0.15f, 1.05f, -0.85f, 0.62f, 0.12f, 0.62f, PI / 2);
      } else if (primary == 1) { // agility: bow and quiver
        P(BOW, hex(0x8a5a2b), 0.75f, 1.15f, -0.4f, 0.9f, 0.9f, 0.9f);
        P(CONE, dark, -0.55f, 1.35f, 0, 0.25f, 0.7f, 0.25f);
      } else { // intelligence: staff, orb and hood
        P(CYL, hex(0x6b4a2c), 0.45f, 1.2f, 0.75f, 0.07f, 2.3f, 0.07f);
        glow(body, 0.45f * r, 2.4f * r, 0.75f * r, 0.26f * r, 0.26f * r, 0.26f * r, 0, tc, pulse, alpha, BALL);
        P(CONE, dark, 0, 2.25f, 0, 0.45f, 0.6f, 0.45f);
      }
      break;
    }
    case K_MELEE: {
      P(BOX, main, 0, 0.95f, 0, 1.1f, 1.3f, 1.3f);
      P(BOX, dark, 0, 0.25f, 0, 0.9f, 0.5f, 1.1f);
      P(BALL, dark, 0, 1.9f, 0, 0.45f, 0.45f, 0.45f);
      P(BOX, hex(0xb9bcc4), 0.8f, 1.0f, 0.75f, 1.6f, 0.14f, 0.14f);
      break;
    }
    case K_RANGED: {
      P(CONE, main, 0, 0.9f, 0, 0.8f, 1.8f, 0.8f);
      P(BALL, dark, 0, 1.95f, 0, 0.4f, 0.4f, 0.4f);
      glow(body, 0.7f * r, 1.4f * r, 0, 0.3f * r, 0.3f * r, 0.3f * r, t * 2, tc, pulse, alpha);
      break;
    }
    case K_SIEGE: {
      Col wood = hex(0x6d4a2c), iron = hex(0x3a3c42);
      P(BOX, wood, 0, 0.7f, 0, 2.2f, 0.6f, 1.5f);
      P(BOX, main, -0.3f, 1.15f, 0, 1.2f, 0.4f, 1.2f);
      P(CYL, iron, 0.6f, 1.35f, 0, 0.25f, 1.6f, 0.25f, 0, 0, -PI / 2.6f);
      const float wheels[4][2] = {{-0.7f, 0.8f}, {0.7f, 0.8f}, {-0.7f, -0.8f}, {0.7f, -0.8f}};
      for (auto& w : wheels) P(CYL, iron, w[0], 0.38f, w[1], 0.38f, 0.16f, 0.38f, PI / 2);
      break;
    }
    case K_TOWER: {
      Col stone = hex(0x8d9097), dk = hex(0x4a4d55);
      P(HEX, dk, 0, 0.25f, 0, 1.1f, 0.5f, 1.1f);
      P(HEX, stone, 0, 1.3f, 0, 0.75f, 1.8f, 0.75f);
      P(HEX, main, 0, 2.25f, 0, 0.88f, 0.18f, 0.88f);
      P(CONE, dk, 0, 2.75f, 0, 0.7f, 0.5f, 0.7f, 0, PI / 6);
      glow(body, 0, (3.35f + fsin(t * 2 + id) * 0.06f) * r, 0, 0.32f * r, 0.45f * r, 0.32f * r, t * 1.2f + id, tc, pulse, alpha);
      break;
    }
    case K_ANCIENT: {
      Col stone = hex(0x9a9ca3), dk = hex(0x3a3c44);
      P(OCT, dk, 0, 0.18f, 0, 1.25f, 0.36f, 1.25f);
      P(OCT, stone, 0, 0.55f, 0, 0.95f, 0.5f, 0.95f);
      P(OCT, main, 0, 0.84f, 0, 0.98f, 0.08f, 0.98f);
      for (int k = 0; k < 4; k++) {
        float a = k * PI / 2 + PI / 4;
        P(CONE, stone, fcos(a) * 0.85f, 1.2f, fsin(a) * 0.85f, 0.16f, 0.9f, 0.16f);
      }
      glow(body, 0, (1.75f + fsin(t * 2 + id) * 0.06f) * r, 0, 0.42f * r, 0.75f * r, 0.42f * r, t * 1.2f + id, tc, pulse, alpha);
      break;
    }
    default: { // jungle creep
      Col eye = hex(0x2a1608), horn = hex(0xe8dcc0);
      P(ROCK, main, 0, 0.9f, 0, 0.95f, 0.85f, 1.0f);
      P(ROCK, main, 0.75f, 1.25f, 0, 0.45f, 0.42f, 0.45f);
      for (int sgn = -1; sgn <= 1; sgn += 2) {
        P(BALL, eye, 1.12f, 1.35f, sgn * 0.2f, 0.08f, 0.08f, 0.08f);
        P(CONE, horn, 0.75f, 1.7f, sgn * 0.25f, 0.08f, 0.35f, 0.08f);
      }
      break;
    }
  }
}

static void emit_projectile(const float* p) {
  float x = p[P_X], z = p[P_Z], dx = p[P_DX], dz = p[P_DZ], rad = p[P_RAD], ln = p[P_LEN], y = p[P_HEIGHT];
  Col c = Col{p[P_R], p[P_G], p[P_B]};
  push(UNLIT, BALL, compose(x, y, z, 0, 0, 0, rad * 0.6f, rad * 0.6f, rad * 0.6f), Col{1, 1, 1}, 1, -1);
  push(ADDITIVE, BALL, compose(x, y, z, 0, 0, 0, rad * 1.3f, rad * 1.3f, rad * 1.3f), c, 0.8f);
  // tail: a cone whose wide end sits on the projectile and whose tip trails behind it
  float w = rad * 0.9f;
  M4 m = ident();
  m.m[0] = dz * w; m.m[2] = -dx * w;   // side axis
  m.m[4] = -dx * ln; m.m[6] = -dz * ln; // cone axis points backwards
  m.m[9] = w;                            // up
  m.m[12] = x - dx * ln * 0.5f; m.m[13] = y; m.m[14] = z - dz * ln * 0.5f;
  push(ADDITIVE, CONE, m, c, 0.55f);
}

// ------------------------------------------------------------------------------------------- fog of war

static void paint_vision(int n_units) {
  memset(vis_buf, 0, sizeof vis_buf);
  float s = VIS / world;
  for (int i = 0; i < n_units; i++) {
    const float* u = frame_buf + i * U_STRIDE;
    float vr = u[U_VISION];
    if (vr <= 0) continue;
    float cx = u[U_X] * s, cy = u[U_Z] * s, r = vr * s, edge = r * 0.25f;
    int x0 = (int)fmax_(0, cx - r), x1 = (int)fmin_(VIS - 1, cx + r), y0 = (int)fmax_(0, cy - r), y1 = (int)fmin_(VIS - 1, cy + r);
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float v = clampf((r - fsqrt(dx * dx + dy * dy)) / edge, 0, 1);
        u8 b = (u8)(v * 255);
        u8& o = vis_buf[y * VIS + x];
        if (b > o) o = b;
      }
  }
}

// ------------------------------------------------------------------------------------------- GPU setup

static void draw_batch(const Batch& b) {
  if (b.count) gpu_draw(meshes[b.mesh].count, b.count, meshes[b.mesh].first, b.first);
}

static int group(int pipe, int texture, bool lit) {
  gpu_group_begin();
  gpu_group_buffer(0, ubo);
  if (pipe != PIPE_DEPTH) {
    gpu_group_sampler(1, SAMPLER_LINEAR);
    gpu_group_texture(2, texture);
  }
  if (lit) {
    gpu_group_sampler(3, SAMPLER_COMPARE);
    gpu_group_texture(4, tex_shadow);
    gpu_group_texture(5, tex_vis);
  }
  return gpu_group_end(pipes[pipe]);
}

static Inst quad_inst(float cx, float cz, float sx, float sz, float y, float e0, float e1, float e2, float e3) {
  Inst o;
  M4 m = compose(cx, y, cz, 0, 0, 0, sx, 1, sz);
  memcpy(o.m, m.m, sizeof o.m);
  o.col[0] = o.col[1] = o.col[2] = o.col[3] = 1;
  o.ext[0] = e0; o.ext[1] = e1; o.ext[2] = e2; o.ext[3] = e3;
  return o;
}

// ------------------------------------------------------------------------------------------- exports

EXPORT(r3d_terrain_buf) u8* r3d_terrain_buf() { return terrain; }
EXPORT(r3d_frame_buf) float* r3d_frame_buf() { return frame_buf; }
EXPORT(r3d_cam_ptr) float* r3d_cam_ptr() { return cam_block; }
EXPORT(r3d_max_units) int r3d_max_units() { return MAX_UNITS; }
EXPORT(r3d_height) float r3d_height(int kind, float r) { return unit_height(kind, r); }

/**
 * Build the scene for a new match. The page has already written the walkability grid into
 * r3d_terrain_buf() and prepared its canvases (slot 0 ground, 1 ground markings, 2 hero emblems).
 */
EXPORT(r3d_init) int r3d_init(float world_size, int grid_n, float cell_size, int heroes, float b0x, float b0y, float b1x, float b1y) {
  world = world_size; grid = grid_n < MAX_GRID ? grid_n : MAX_GRID; cell = cell_size; n_heroes = heroes;
  bases[0][0] = b0x; bases[0][1] = b0y; bases[1][0] = b1x; bases[1][1] = b1y;
  memset(ustate, 0, sizeof ustate);

  static bool ready = false;
  if (!ready) {
    int sh = gpu_shader(WGSL, len(WGSL));
    if (!sh) return 0;
    pipes[PIPE_DEPTH] = pipeline(sh, "vs_depth", "", TARGET_SHADOW, BLEND_NONE, 1, 1);
    pipes[PIPE_LIT] = pipeline(sh, "vs_lit", "fs_lit", TARGET_SCREEN, BLEND_NONE, 1);
    pipes[PIPE_LIT_FADED] = pipeline(sh, "vs_lit", "fs_lit", TARGET_SCREEN, BLEND_ALPHA, 0);
    pipes[PIPE_UNLIT] = pipeline(sh, "vs_unlit", "fs_unlit", TARGET_SCREEN, BLEND_ALPHA, 1);
    pipes[PIPE_DECAL] = pipeline(sh, "vs_unlit", "fs_unlit", TARGET_SCREEN, BLEND_ALPHA, 0);
    pipes[PIPE_ADD] = pipeline(sh, "vs_unlit", "fs_unlit", TARGET_SCREEN, BLEND_ADD, 0);
    for (int p : pipes) if (!p) return 0;

    build_meshes();
    mesh_vbo = gpu_buffer(n_verts * 32, 0);
    gpu_write_buffer(mesh_vbo, 0, verts, n_verts * 32);
    tree_vbo = gpu_buffer((int)sizeof tree_inst, 0);
    inst_vbo = gpu_buffer((int)sizeof frame_inst, 0);
    ubo = gpu_buffer((int)sizeof(Uniforms), 1);
    tex_vis = gpu_texture(VIS, VIS, FORMAT_R8);
    tex_shadow = gpu_texture(SHADOW, SHADOW, FORMAT_DEPTH32);
    ready = true;
  }
  tex_ground = gpu_canvas_texture(tex_ground, 0, 1);
  tex_emblem = gpu_canvas_texture(tex_emblem, 2, 1);
  tex_decal = gpu_canvas_texture(tex_decal, 1, 0);
  g_depth = group(PIPE_DEPTH, 0, false);
  g_lit = group(PIPE_LIT, tex_ground, true);
  g_lit_faded = group(PIPE_LIT_FADED, tex_ground, true);
  g_emblem = group(PIPE_UNLIT, tex_emblem, false);
  g_decal = group(PIPE_DECAL, tex_decal, false);
  g_add = group(PIPE_ADD, tex_emblem, false);
  build_trees();
  return 1;
}

/** Place the camera: centred on world (x, y), `zoom` screen px per world unit at that point. */
EXPORT(r3d_camera) void r3d_camera(float x, float y, float zoom, float w, float h, float shake_x, float shake_z) {
  cam_x = x; cam_y = y; cam_zoom = zoom; view_h = h;
  float dist = h / (zoom * 2 * (fsin(FOV / 2) / fcos(FOV / 2)));
  V3 at = v3(x + shake_x, 0, y + shake_z);
  V3 eye = v3(at.x, fsin(PITCH) * dist, at.z + fcos(PITCH) * dist);
  M4 view = look_at(eye, at, v3(0, 1, 0));
  view_proj = mul(perspective(FOV, w / h, 10, 12000), view);
  M4 inv = inverse(view_proj);
  memcpy(cam_block, view_proj.m, 64);
  memcpy(cam_block + 16, inv.m, 64);
  cam_block[32] = eye.x; cam_block[33] = eye.y; cam_block[34] = eye.z;
}

/**
 * Draw one frame from frame_buf: n_units unit records then n_projs projectile records. The ground
 * markings canvas covers the square centred on (decal_x, decal_z) with side decal_side.
 */
EXPORT(r3d_draw) void r3d_draw(int n_units, int n_projs, float decal_x, float decal_z, float decal_side, float t, int fog_on) {
  if (n_units > MAX_UNITS) n_units = MAX_UNITS;
  if (n_projs > MAX_PROJS) n_projs = MAX_PROJS;

  // ---- this frame's instances
  n_scratch = 0;
  us_cur ^= 1;
  memset(ustate[us_cur], 0, sizeof ustate[us_cur]);
  emit_fountains(t);
  for (int i = 0; i < n_units; i++) emit_unit(frame_buf + i * U_STRIDE, t);
  const float* projs = frame_buf + MAX_UNITS * U_STRIDE;
  for (int i = 0; i < n_projs; i++) emit_projectile(projs + i * P_STRIDE);
  sort_instances();
  int ground = n_scratch, decal = n_scratch + 1;
  frame_inst[ground] = quad_inst(world / 2, world / 2, world, world, 0, 0, 1, 0, 0); // ext.y = 1: sample the ground texture
  frame_inst[decal] = quad_inst(decal_x, decal_z, decal_side, decal_side, 0.8f, 0, 0, 1, 1);
  gpu_write_buffer(inst_vbo, 0, frame_inst, (n_scratch + 2) * (int)sizeof(Inst));

  // ---- shadow map over what the camera sees, snapped to texels so it does not shimmer
  float span = fmin_(1600, view_h / cam_zoom) * 0.95f;
  float texel = 2 * span / SHADOW;
  V3 at = v3(ffloor(cam_x / texel) * texel, 0, ffloor(cam_y / texel) * texel);
  V3 sun_eye = v3(at.x + SUN_DIR.x * 1600, SUN_DIR.y * 1600, at.z + SUN_DIR.z * 1600);
  sun_vp = mul(ortho(-span, span, -span, span, 100, 3400), look_at(sun_eye, at, v3(0, 1, 0)));

  memcpy(uni.vp, view_proj.m, 64);
  memcpy(uni.sun_vp, sun_vp.m, 64);
  uni.sun_dir[0] = SUN_DIR.x; uni.sun_dir[1] = SUN_DIR.y; uni.sun_dir[2] = SUN_DIR.z;
  uni.params[0] = fog_on ? 1.f : 0.f;
  uni.params[1] = world;
  memcpy(uni.eye, cam_block + 32, 12);
  gpu_write_buffer(ubo, 0, &uni, sizeof uni);
  if (fog_on) paint_vision(n_units);
  else memset(vis_buf, 255, sizeof vis_buf);
  gpu_write_texture(tex_vis, vis_buf, VIS, VIS, VIS);
  gpu_update_canvas_texture(tex_decal, 1);

  gpu_begin_pass(TARGET_SHADOW, tex_shadow, 0, 0, 0);
  gpu_set_pipeline(pipes[PIPE_DEPTH]);
  gpu_set_group(g_depth);
  gpu_set_vertex_buffer(0, mesh_vbo);
  gpu_set_vertex_buffer(1, tree_vbo);
  for (auto& b : tree_batches) draw_batch(b);
  gpu_set_vertex_buffer(1, inst_vbo);
  for (int m = 0; m < N_MESH; m++) draw_batch(batches[LIT][m]);
  gpu_end_pass();

  // ---- main pass
  Col sky = hex(0x070d0a);
  gpu_begin_pass(TARGET_SCREEN, 0, sky.r, sky.g, sky.b);
  gpu_set_vertex_buffer(0, mesh_vbo);
  gpu_set_pipeline(pipes[PIPE_LIT]);
  gpu_set_group(g_lit);
  gpu_set_vertex_buffer(1, tree_vbo);
  for (auto& b : tree_batches) draw_batch(b);
  gpu_set_vertex_buffer(1, inst_vbo);
  draw_batch(Batch{QUAD, ground, 1});
  for (int m = 0; m < N_MESH; m++) draw_batch(batches[LIT][m]);

  // emblems and projectile cores
  gpu_set_pipeline(pipes[PIPE_UNLIT]);
  gpu_set_group(g_emblem);
  for (int m = 0; m < N_MESH; m++) draw_batch(batches[UNLIT][m]);

  // ground markings (zones, ranges, targeting...) painted by the page into a canvas
  gpu_set_pipeline(pipes[PIPE_DECAL]);
  gpu_set_group(g_decal);
  draw_batch(Batch{QUAD, decal, 1});

  // units remembered in the fog, see-through
  gpu_set_pipeline(pipes[PIPE_LIT_FADED]);
  gpu_set_group(g_lit_faded);
  for (int m = 0; m < N_MESH; m++) draw_batch(batches[LIT_FADED][m]);

  // glows
  gpu_set_pipeline(pipes[PIPE_ADD]);
  gpu_set_group(g_add);
  for (int m = 0; m < N_MESH; m++) draw_batch(batches[ADDITIVE][m]);
  gpu_end_pass();
  gpu_submit();
}
