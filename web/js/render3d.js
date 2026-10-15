// 3D view: draws the same engine state as render.js with Three.js. The simulation stays 2D (x, y on the
// ground); this file only lifts it into a tilted perspective scene with real trees, models and shadows.
//
// It extends the 2D Renderer and keeps its screen-space work (health bars, damage numbers, minimap,
// event effects). Ground markings that the 2D view paints into the world (ability zones, tower range,
// targeting previews, selection rings, particles) are painted into a canvas that is laid on the ground
// under the camera, so the 3D view reuses that code too. Fog of war darkens every lit material through
// a small world-space visibility texture.
//
// Three.js is loaded from a CDN on demand; main.js falls back to the 2D view when it cannot be fetched.

import * as THREE from 'https://cdn.jsdelivr.net/npm/three@0.170.0/build/three.module.min.js';
import { Renderer, TEAM, NEUTRAL_COLORS, buildTerrain, drawHeroPortrait } from './render.js';
import { abilityColor } from './icons.js';

const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const FOV = 42;
const PITCH = (58 * Math.PI) / 180; // camera elevation above the ground
const DECAL_PX = 1024;

function cellRand(cx, cy, salt) {
  let h = Math.imul(cx + 101, 73856093) ^ Math.imul(cy + 57, 19349663) ^ Math.imul(salt + 13, 83492791);
  h = Math.imul(h ^ (h >>> 15), 2246822507);
  h = Math.imul(h ^ (h >>> 13), 3266489909);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

let glRenderer = null; // one WebGL context for the page, reused across matches

export class Renderer3D extends Renderer {
  constructor(canvas, mini, info, terrain) {
    super(canvas, mini, info, terrain);
    const W = info.world;
    this.glCanvas = document.getElementById('world3d');
    document.body.classList.add('three');
    if (!glRenderer) {
      glRenderer = new THREE.WebGLRenderer({ canvas: this.glCanvas, antialias: true, powerPreference: 'high-performance' });
      glRenderer.shadowMap.enabled = true;
      glRenderer.shadowMap.type = THREE.PCFSoftShadowMap;
      glRenderer.toneMapping = THREE.ACESFilmicToneMapping;
      glRenderer.toneMappingExposure = 1.15;
    }
    this.gl = glRenderer;
    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color('#0b1118');
    this.camera = new THREE.PerspectiveCamera(FOV, 1, 10, 12000);
    this.ray = new THREE.Raycaster();
    this.tmp = new THREE.Vector3();
    this.camKey = '';

    // fog of war: friendly vision painted into a small world-space texture, sampled by every lit material
    this.visCanvas = document.createElement('canvas');
    this.visCanvas.width = this.visCanvas.height = 160;
    this.visTex = new THREE.CanvasTexture(this.visCanvas);
    this.fowUniforms = { uVis: { value: this.visTex }, uFogOn: { value: 0 }, uWorld: { value: W } };

    // lights
    this.scene.add(new THREE.HemisphereLight('#c4d4ff', '#2a3320', 1.25));
    this.sun = new THREE.DirectionalLight('#fff1d6', 2.4);
    this.sun.castShadow = true;
    this.sun.shadow.mapSize.set(2048, 2048);
    this.sun.shadow.bias = -0.0006;
    this.sun.shadow.normalBias = 0.6;
    this.scene.add(this.sun, this.sun.target);
    this.sunDir = new THREE.Vector3(-0.45, 1, 0.55).normalize();

    // ground
    const ground = buildTerrain(info, terrain, { trees: false });
    const gtex = new THREE.CanvasTexture(ground);
    gtex.colorSpace = THREE.SRGBColorSpace;
    gtex.anisotropy = this.gl.capabilities.getMaxAnisotropy();
    const gmesh = new THREE.Mesh(new THREE.PlaneGeometry(W, W), this.fow(new THREE.MeshStandardMaterial({ map: gtex, roughness: 1 })));
    gmesh.rotation.x = -Math.PI / 2;
    gmesh.position.set(W / 2, 0, W / 2);
    gmesh.receiveShadow = true;
    this.scene.add(gmesh);
    const skirt = new THREE.Mesh(new THREE.PlaneGeometry(W * 6, W * 6), new THREE.MeshBasicMaterial({ color: '#070d0a' }));
    skirt.rotation.x = -Math.PI / 2;
    skirt.position.set(W / 2, -2, W / 2);
    this.scene.add(skirt);
    this.buildTrees(info, terrain);
    this.buildFountains(info);

    // ground markings (zones, rings, targeting...) drawn by the 2D code into a canvas under the camera
    this.decal = document.createElement('canvas');
    this.decal.width = this.decal.height = DECAL_PX;
    this.dctx = this.decal.getContext('2d');
    this.decalTex = new THREE.CanvasTexture(this.decal);
    this.decalTex.colorSpace = THREE.SRGBColorSpace;
    this.decalMesh = new THREE.Mesh(
      new THREE.PlaneGeometry(1, 1),
      new THREE.MeshBasicMaterial({ map: this.decalTex, transparent: true, depthWrite: false, toneMapped: false }),
    );
    this.decalMesh.rotation.x = -Math.PI / 2;
    this.decalMesh.position.y = 0.8;
    this.decalMesh.renderOrder = 1;
    this.scene.add(this.decalMesh);

    this.units = new Map(); // unit id -> { obj, kind, mats, height, ... }
    this.projPool = [];
    this.geo = this.sharedGeometry();
    this.portraits = new Map();
    this.resize();
  }

  // ---- materials

  /** Patch a lit material so fog of war darkens it. */
  fow(mat) {
    const u = this.fowUniforms;
    mat.onBeforeCompile = (sh) => {
      Object.assign(sh.uniforms, u);
      sh.vertexShader = sh.vertexShader
        .replace('#include <common>', '#include <common>\nvarying vec3 vFowW;')
        .replace('#include <project_vertex>', `#include <project_vertex>
          vec4 fowP = vec4(transformed, 1.0);
          #ifdef USE_INSTANCING
            fowP = instanceMatrix * fowP;
          #endif
          vFowW = (modelMatrix * fowP).xyz;`);
      sh.fragmentShader = sh.fragmentShader
        .replace('#include <common>', '#include <common>\nvarying vec3 vFowW;\nuniform sampler2D uVis;\nuniform float uFogOn;\nuniform float uWorld;')
        .replace('#include <dithering_fragment>', `#include <dithering_fragment>
          float fowVis = texture2D(uVis, vec2(vFowW.x / uWorld, 1.0 - vFowW.z / uWorld)).r;
          gl_FragColor.rgb *= mix(1.0, mix(0.28, 1.0, fowVis), uFogOn);`);
    };
    mat.customProgramCacheKey = () => 'fow';
    return mat;
  }

  std(color, extra = {}) {
    return this.fow(new THREE.MeshStandardMaterial({ color, roughness: 0.7, metalness: 0.05, ...extra }));
  }

  sharedGeometry() {
    return {
      cyl: new THREE.CylinderGeometry(1, 1, 1, 14),
      taper: new THREE.CylinderGeometry(0.72, 1, 1, 14),
      cone: new THREE.ConeGeometry(1, 1, 12),
      tri: new THREE.ConeGeometry(1, 1, 3),
      ball: new THREE.SphereGeometry(1, 18, 12),
      box: new THREE.BoxGeometry(1, 1, 1),
      hex: new THREE.CylinderGeometry(1, 1.12, 1, 6),
      oct: new THREE.CylinderGeometry(1, 1.1, 1, 8),
      gem: new THREE.OctahedronGeometry(1, 0),
      ico: new THREE.IcosahedronGeometry(1, 0),
      dodeca: new THREE.DodecahedronGeometry(1, 0),
      ring: new THREE.TorusGeometry(1, 0.08, 6, 24, Math.PI),
      disc: new THREE.CircleGeometry(1, 28),
    };
  }

  // ---- static scenery

  buildTrees(info, terrain) {
    const N = info.grid, cp = info.cell;
    const T = (cx, cy) => (cx < 0 || cy < 0 || cx >= N || cy >= N ? 0 : terrain[cy * N + cx]);
    const trees = [];
    for (let cy = 0; cy < N; cy++) {
      for (let cx = 0; cx < N; cx++) {
        if (T(cx, cy) !== 0) continue;
        let open = 0;
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) if (T(cx + dx, cy + dy) !== 0) open++;
        const n = open > 0 ? 2 + Math.floor(cellRand(cx, cy, 40) * 2) : 1;
        for (let k = 0; k < n; k++) {
          trees.push({
            x: (cx + cellRand(cx, cy, 50 + k)) * cp,
            z: (cy + cellRand(cx, cy, 60 + k)) * cp,
            r: cp * (0.55 + cellRand(cx, cy, 70 + k) * 0.5),
            h: 46 + cellRand(cx, cy, 80 + k) * 40,
            c: cellRand(cx, cy, 90 + k),
            edge: open > 0,
          });
        }
      }
    }
    const trunk = new THREE.InstancedMesh(new THREE.CylinderGeometry(0.16, 0.24, 1, 6), this.std('#4a3424', { roughness: 1 }), trees.length);
    const low = new THREE.InstancedMesh(new THREE.ConeGeometry(1, 1, 7), this.std('#ffffff', { roughness: 0.95 }), trees.length);
    const high = new THREE.InstancedMesh(new THREE.ConeGeometry(1, 1, 7), this.std('#ffffff', { roughness: 0.95 }), trees.length);
    const m = new THREE.Matrix4(), q = new THREE.Quaternion(), s = new THREE.Vector3(), p = new THREE.Vector3(), col = new THREE.Color();
    const up = new THREE.Vector3(0, 1, 0);
    trees.forEach((t, i) => {
      q.setFromAxisAngle(up, t.c * 6.28);
      m.compose(p.set(t.x, t.h * 0.18, t.z), q, s.set(t.r, t.h * 0.36, t.r)); trunk.setMatrixAt(i, m);
      m.compose(p.set(t.x, t.h * 0.5, t.z), q, s.set(t.r * 1.05, t.h * 0.6, t.r * 1.05)); low.setMatrixAt(i, m);
      m.compose(p.set(t.x, t.h * 0.8, t.z), q, s.set(t.r * 0.7, t.h * 0.42, t.r * 0.7)); high.setMatrixAt(i, m);
      col.setHSL(0.36 + t.c * 0.06, 0.42, (t.edge ? 0.2 : 0.14) + t.c * 0.06);
      low.setColorAt(i, col);
      high.setColorAt(i, col.offsetHSL(0, 0, 0.04));
    });
    for (const mesh of [trunk, low, high]) { mesh.castShadow = true; mesh.receiveShadow = true; this.scene.add(mesh); }
  }

  buildFountains(info) {
    info.bases.forEach(([bx, by], team) => {
      const c = TEAM[team];
      const g = new THREE.Group();
      const basin = new THREE.Mesh(new THREE.CylinderGeometry(52, 58, 10, 32), this.std('#7d8088', { roughness: 0.6 }));
      basin.position.y = 5;
      const water = new THREE.Mesh(new THREE.CylinderGeometry(46, 46, 2, 32), new THREE.MeshStandardMaterial({ color: '#4ea2e0', emissive: '#1a4f80', roughness: 0.15, metalness: 0.2 }));
      water.position.y = 10;
      const spire = new THREE.Mesh(new THREE.CylinderGeometry(4, 8, 40, 8), this.std('#9a9da5'));
      spire.position.y = 28;
      const orb = new THREE.Mesh(new THREE.SphereGeometry(9, 16, 12), new THREE.MeshStandardMaterial({ color: c.light, emissive: c.main, emissiveIntensity: 1.6 }));
      orb.position.y = 54;
      const light = new THREE.PointLight(c.main, 2.5, 380, 1.4);
      light.position.y = 60;
      g.add(basin, water, spire, orb, light);
      for (const o of [basin, spire]) { o.castShadow = true; o.receiveShadow = true; }
      g.position.set(bx, 0, by);
      this.scene.add(g);
    });
  }

  // ---- unit models (built facing +x; rotation.y = -facing)

  heroEmblem(def, team) {
    const key = `${def}:${team}`;
    let tex = this.portraits.get(key);
    if (!tex) {
      const c = document.createElement('canvas');
      c.width = c.height = 128;
      drawHeroPortrait(c, def, team);
      tex = new THREE.CanvasTexture(c);
      tex.colorSpace = THREE.SRGBColorSpace;
      this.portraits.set(key, tex);
    }
    return tex;
  }

  makeUnit(kind, team, def, r) {
    const K = this.info.kind, G = this.geo, c = TEAM[team] || null;
    const g = new THREE.Group(), body = new THREE.Group();
    g.add(body);
    const mats = [];
    const M = (color, extra) => { const m = this.std(color, extra); mats.push(m); return m; };
    const add = (geo, mat, x, y, z, sx, sy, sz, parent = body) => {
      const o = new THREE.Mesh(geo, mat);
      o.position.set(x, y, z); o.scale.set(sx, sy, sz);
      o.castShadow = true; o.receiveShadow = true;
      parent.add(o);
      return o;
    };
    const u = { obj: g, body, kind, mats, height: r * 2.4, spin: null, glow: null };

    if (kind === K.hero) {
      const primary = this.info.heroes[def]?.primary ?? 0;
      const main = M(c.main), dark = M(c.dark), metal = M('#c9ccd4', { metalness: 0.6, roughness: 0.35 }), skin = M('#e6c2a0');
      add(G.taper, main, 0, r * 1.0, 0, r * 0.62, r * 1.3, r * 0.62);           // torso
      add(G.cyl, dark, 0, r * 0.25, 0, r * 0.66, r * 0.5, r * 0.66);           // legs / skirt
      add(G.ball, skin, 0, r * 1.92, 0, r * 0.4, r * 0.42, r * 0.4);           // head
      add(G.ball, main, -r * 0.05, r * 1.6, 0, r * 0.82, r * 0.3, r * 0.82);   // shoulders
      const emblem = new THREE.Mesh(G.disc, new THREE.MeshBasicMaterial({ map: this.heroEmblem(def, team), transparent: true, toneMapped: false }));
      emblem.position.set(r * 0.66, r * 1.12, 0); emblem.rotation.y = Math.PI / 2; emblem.scale.setScalar(r * 0.42);
      body.add(emblem);
      if (primary === 0) {        // strength: sword and shield
        add(G.box, metal, r * 0.55, r * 1.05, r * 0.85, r * 1.5, r * 0.12, r * 0.18);
        add(G.cyl, metal, r * 0.15, r * 1.05, -r * 0.85, r * 0.62, r * 0.12, r * 0.62).rotation.x = Math.PI / 2;
      } else if (primary === 1) { // agility: bow
        const bow = add(G.ring, M('#8a5a2b'), r * 0.75, r * 1.15, -r * 0.4, r * 0.9, r * 0.9, r * 0.9);
        bow.rotation.set(0, Math.PI / 2, -Math.PI / 2);
        add(G.cone, dark, -r * 0.55, r * 1.35, 0, r * 0.25, r * 0.7, r * 0.25);  // quiver
      } else {                    // intelligence: staff and orb
        add(G.cyl, M('#6b4a2c'), r * 0.45, r * 1.2, r * 0.75, r * 0.07, r * 2.3, r * 0.07);
        u.glow = add(G.ball, new THREE.MeshStandardMaterial({ color: c.light, emissive: c.main, emissiveIntensity: 2 }), r * 0.45, r * 2.4, r * 0.75, r * 0.26, r * 0.26, r * 0.26);
        add(G.cone, dark, 0, r * 2.25, 0, r * 0.45, r * 0.6, r * 0.45);         // hood
      }
      u.height = r * 2.6;
      u.bob = true;
    } else if (kind === K.melee) {
      const main = M(c.main), dark = M(c.dark), metal = M('#b9bcc4', { metalness: 0.5, roughness: 0.4 });
      add(G.box, main, 0, r * 0.95, 0, r * 1.1, r * 1.3, r * 1.3);
      add(G.box, dark, 0, r * 0.25, 0, r * 0.9, r * 0.5, r * 1.1);
      add(G.ball, dark, 0, r * 1.9, 0, r * 0.45, r * 0.45, r * 0.45);
      add(G.box, metal, r * 0.8, r * 1.0, r * 0.75, r * 1.6, r * 0.14, r * 0.14);
      u.height = r * 2.4;
      u.bob = true;
    } else if (kind === K.ranged) {
      const main = M(c.main), dark = M(c.dark);
      add(G.cone, main, 0, r * 0.9, 0, r * 0.8, r * 1.8, r * 0.8);
      add(G.ball, dark, 0, r * 1.95, 0, r * 0.4, r * 0.4, r * 0.4);
      u.glow = add(G.gem, new THREE.MeshStandardMaterial({ color: c.light, emissive: c.main, emissiveIntensity: 1.8 }), r * 0.7, r * 1.4, 0, r * 0.3, r * 0.3, r * 0.3);
      u.height = r * 2.4;
      u.bob = true;
    } else if (kind === K.siege) {
      const main = M(c.main), wood = M('#6d4a2c'), iron = M('#3a3c42', { metalness: 0.5 });
      add(G.box, wood, 0, r * 0.7, 0, r * 2.2, r * 0.6, r * 1.5);
      add(G.box, main, -r * 0.3, r * 1.15, 0, r * 1.2, r * 0.4, r * 1.2);
      const barrel = add(G.cyl, iron, r * 0.6, r * 1.35, 0, r * 0.25, r * 1.6, r * 0.25);
      barrel.rotation.z = -Math.PI / 2.6;
      for (const [x, z] of [[-0.7, 0.8], [0.7, 0.8], [-0.7, -0.8], [0.7, -0.8]]) {
        const w = add(G.cyl, iron, x * r, r * 0.38, z * r, r * 0.38, r * 0.16, r * 0.38);
        w.rotation.x = Math.PI / 2;
      }
      u.height = r * 2.2;
    } else if (kind === K.tower) {
      const stone = M('#8d9097', { roughness: 0.85 }), dark = M('#4a4d55', { roughness: 0.9 }), trim = M(c.main, { roughness: 0.5 });
      add(G.hex, dark, 0, r * 0.25, 0, r * 1.1, r * 0.5, r * 1.1);
      add(G.hex, stone, 0, r * 1.3, 0, r * 0.75, r * 1.8, r * 0.75);
      add(G.hex, trim, 0, r * 2.25, 0, r * 0.88, r * 0.18, r * 0.88);
      add(G.cone, dark, 0, r * 2.75, 0, r * 0.7, r * 0.5, r * 0.7).rotation.y = Math.PI / 6;
      u.glow = add(G.gem, new THREE.MeshStandardMaterial({ color: c.light, emissive: c.main, emissiveIntensity: 2.2 }), 0, r * 3.35, 0, r * 0.32, r * 0.45, r * 0.32);
      u.spin = u.glow;
      u.light = new THREE.PointLight(c.main, 1.2, r * 7, 1.5);
      u.light.position.y = r * 3.3;
      g.add(u.light);
      u.height = r * 3.7;
    } else if (kind === K.ancient) {
      const stone = M('#9a9ca3', { roughness: 0.8 }), dark = M('#3a3c44', { roughness: 0.9 }), trim = M(c.main, { roughness: 0.45 });
      add(G.oct, dark, 0, r * 0.18, 0, r * 1.25, r * 0.36, r * 1.25);
      add(G.oct, stone, 0, r * 0.55, 0, r * 0.95, r * 0.5, r * 0.95);
      add(G.oct, trim, 0, r * 0.84, 0, r * 0.98, r * 0.08, r * 0.98);
      for (let k = 0; k < 4; k++) {
        const a = (k * Math.PI) / 2 + Math.PI / 4;
        add(G.cone, stone, Math.cos(a) * r * 0.85, r * 1.2, Math.sin(a) * r * 0.85, r * 0.16, r * 0.9, r * 0.16);
      }
      u.glow = add(G.gem, new THREE.MeshStandardMaterial({ color: c.light, emissive: c.main, emissiveIntensity: 2.4 }), 0, r * 1.75, 0, r * 0.42, r * 0.75, r * 0.42);
      u.spin = u.glow;
      u.light = new THREE.PointLight(c.main, 3, r * 9, 1.4);
      u.light.position.y = r * 2;
      g.add(u.light);
      u.height = r * 2.6;
    } else { // neutral creep
      const col = NEUTRAL_COLORS[def % NEUTRAL_COLORS.length], main = M(col), dark = M('#2a1608');
      add(G.dodeca, main, 0, r * 0.9, 0, r * 0.95, r * 0.85, r * 1.0);
      add(G.dodeca, main, r * 0.75, r * 1.25, 0, r * 0.45, r * 0.42, r * 0.45);
      for (const z of [-1, 1]) add(G.ball, dark, r * 1.12, r * 1.35, z * r * 0.2, r * 0.08, r * 0.08, r * 0.08);
      for (const z of [-1, 1]) add(G.cone, M('#e8dcc0'), r * 0.75, r * 1.7, z * r * 0.25, r * 0.08, r * 0.35, r * 0.08);
      u.height = r * 2.0;
      u.bob = true;
    }
    for (const m of mats) m.transparent = true;
    this.scene.add(g);
    return u;
  }

  syncUnits(view, t) {
    const f = view.cur, { U } = this.info, us = this.info.unit_fields.length, u = f.units;
    const live = new Set();
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us, id = u[o + U.id], k = u[o + U.kind], team = u[o + U.team], def = u[o + U.def], r = u[o + U.radius];
      let m = this.units.get(id);
      if (m && m.kind !== k) { this.dropUnit(id, m); m = null; }
      if (!m) { m = this.makeUnit(k, team, def, r); this.units.set(id, m); }
      live.add(id);
      const vis = u[o + U.visible] > 0, alive = u[o + U.alive] > 0;
      m.obj.visible = alive;
      if (!alive) continue;
      const facing = u[o + U.facing];
      let lx = 0, lz = 0;
      const sw = u[o + U.swing_t], sl = u[o + U.swing_len];
      if (sw > 0 && sl > 0 && u[o + U.range] < 150) {
        const l = Math.sin((1 - sw / sl) * Math.PI) * 7;
        lx = Math.cos(facing) * l; lz = Math.sin(facing) * l;
      }
      m.obj.position.set(view.px[i] + lx, 0, view.py[i] + lz);
      // turn smoothly toward the engine's facing
      const want = -facing;
      if (m.yaw === undefined) m.yaw = want;
      let d = want - m.yaw;
      d = Math.atan2(Math.sin(d), Math.cos(d));
      m.yaw += d * 0.35;
      if (!m.spin) m.obj.rotation.y = m.yaw;
      // walk bob when the unit moved since last frame
      if (m.bob) {
        const moved = m.lx !== undefined ? Math.hypot(view.px[i] - m.lx, view.py[i] - m.lz) : 0;
        m.walk = (m.walk || 0) * 0.85 + (moved > 0.2 ? 0.15 : 0);
        m.body.position.y = Math.abs(Math.sin(t * 9 + id)) * r * 0.12 * m.walk;
        m.body.rotation.z = -0.08 * m.walk;
      }
      m.lx = view.px[i]; m.lz = view.py[i];
      if (m.spin) { m.spin.rotation.y = t * 1.2 + id; m.spin.position.y = m.spin.userData.y ??= m.spin.position.y; m.spin.position.y += Math.sin(t * 2 + id) * r * 0.06; }
      if (m.glow?.material) m.glow.material.emissiveIntensity = 1.8 + Math.sin(t * 3 + id) * 0.5;
      const op = vis ? 1 : 0.55;
      if (m.op !== op) { m.op = op; for (const mat of m.mats) { mat.opacity = op; mat.depthWrite = op === 1; } }
      m.index = i;
    }
    for (const [id, m] of this.units) if (!live.has(id)) this.dropUnit(id, m);
  }

  dropUnit(id, m) {
    this.scene.remove(m.obj);
    m.obj.traverse((o) => { if (o.material && !o.material.map) o.material.dispose(); });
    this.units.delete(id);
  }

  syncProjectiles(f) {
    const { P } = this.info, ps = this.info.proj_fields.length, n = f.projs.length / ps;
    while (this.projPool.length < n) {
      const g = new THREE.Group();
      const core = new THREE.Mesh(this.geo.ball, new THREE.MeshBasicMaterial({ color: '#ffffff', toneMapped: false }));
      const halo = new THREE.Mesh(this.geo.ball, new THREE.MeshBasicMaterial({ color: '#ffffff', transparent: true, opacity: 0.45, blending: THREE.AdditiveBlending, depthWrite: false, toneMapped: false }));
      const tail = new THREE.Mesh(this.geo.cone, halo.material);
      g.add(core, halo, tail);
      this.scene.add(g);
      this.projPool.push({ g, core, halo, tail });
    }
    for (let k = 0; k < this.projPool.length; k++) {
      const p = this.projPool[k];
      p.g.visible = k < n;
      if (k >= n) continue;
      const i = k * ps, pr = f.projs;
      const x = pr[i + P.x], y = pr[i + P.y], dx = pr[i + P.dx], dy = pr[i + P.dy], ab = pr[i + P.ability], team = pr[i + P.team];
      const attack = ab === 255;
      const col = attack ? TEAM[team]?.light || '#fff' : abilityColor(this.info, ab);
      const rad = attack ? 4 : pr[i + P.homing] ? 7 : Math.max(8, pr[i + P.radius] * 0.5);
      const len = attack ? 18 : pr[i + P.homing] ? 26 : 50;
      p.g.position.set(x, attack ? 34 : 26, y);
      p.g.rotation.set(0, Math.atan2(-dy, dx), 0);
      p.core.scale.setScalar(rad * 0.6);
      p.halo.scale.setScalar(rad * 1.3);
      p.halo.material.color.set(col);
      p.tail.scale.set(rad * 0.9, len, rad * 0.9);
      p.tail.rotation.z = Math.PI / 2;
      p.tail.position.x = -len / 2;
    }
  }

  // ---- camera

  resize() {
    super.resize();
    if (!this.gl) return;
    this.gl.setPixelRatio(this.dpr);
    this.gl.setSize(this.W, this.H, false);
    this.glCanvas.style.width = this.W + 'px';
    this.glCanvas.style.height = this.H + 'px';
    this.camera.aspect = this.W / this.H;
    this.camKey = '';
  }

  syncCamera(ox = 0, oz = 0) {
    const c = this.cam, key = `${c.x},${c.y},${c.zoom},${ox},${oz},${this.W},${this.H}`;
    if (key === this.camKey) return;
    this.camKey = key;
    const cam = this.camera;
    const dist = this.H / (c.zoom * 2 * Math.tan((FOV * Math.PI) / 360));
    cam.position.set(c.x + ox, Math.sin(PITCH) * dist, c.y + Math.cos(PITCH) * dist + oz);
    cam.lookAt(c.x + ox, 0, c.y + oz);
    cam.updateProjectionMatrix();
    cam.updateMatrixWorld();
  }

  w2s(x, y, h = 0) {
    this.syncCamera();
    const v = this.tmp.set(x, h, y).project(this.camera);
    if (v.z > 1) return [-1e4, -1e4]; // behind the camera
    return [((v.x + 1) / 2) * this.W, ((1 - v.y) / 2) * this.H];
  }

  s2w(sx, sy) {
    this.syncCamera();
    this.ray.setFromCamera({ x: (sx / this.W) * 2 - 1, y: -(sy / this.H) * 2 + 1 }, this.camera);
    const o = this.ray.ray.origin, d = this.ray.ray.direction;
    const t = d.y < -1e-4 ? -o.y / d.y : 20000;
    return [o.x + d.x * t, o.z + d.z * t];
  }

  barAnchor(view, i, r, hero, bld) {
    const m = this.units.get(view.cur.units[i * this.info.unit_fields.length + this.info.U.id]);
    const [sx, sy] = this.w2s(view.px[i], view.py[i], (m ? m.height : r * 2.4) + 6);
    return [sx, sy];
  }

  drawViewport(m, sc) {
    const pts = [[0, 0], [this.W, 0], [this.W, this.H], [0, this.H]].map(([x, y]) => this.s2w(x, y));
    m.strokeStyle = 'rgba(255,255,255,.9)';
    m.lineWidth = 1.5;
    m.beginPath();
    for (const [x, y] of pts) m.lineTo(clamp(x, -50, this.info.world + 50) * sc, clamp(y, -50, this.info.world + 50) * sc);
    m.closePath();
    m.stroke();
  }

  // ---- per-frame

  drawVision(view) {
    const f = view.cur, on = !!f.g[this.info.G.fog];
    this.fowUniforms.uFogOn.value = on ? 1 : 0;
    if (!on) return;
    const c = this.visCanvas, x = c.getContext('2d'), s = c.width / this.info.world, { U } = this.info, us = this.info.unit_fields.length;
    x.fillStyle = '#000';
    x.fillRect(0, 0, c.width, c.height);
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us;
      if (f.units[o + U.team] !== f.playerTeam) continue;
      const cx = view.px[i] * s, cy = view.py[i] * s, r = f.units[o + U.vision] * s;
      const g = x.createRadialGradient(cx, cy, r * 0.75, cx, cy, r);
      g.addColorStop(0, '#fff');
      g.addColorStop(1, 'rgba(255,255,255,0)');
      x.fillStyle = g;
      x.beginPath(); x.arc(cx, cy, r, 0, Math.PI * 2); x.fill();
    }
    this.visTex.needsUpdate = true;
  }

  /** Paint the 2D ground markings into the decal canvas covering what the camera sees. */
  drawDecal(view, ui, t) {
    const W = this.info.world;
    const pts = [[0, 0], [this.W, 0], [this.W, this.H], [0, this.H]].map(([x, y]) => this.s2w(x, y));
    let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
    for (const [x, z] of pts) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); z0 = Math.min(z0, z); z1 = Math.max(z1, z); }
    x0 = clamp(x0, -100, W + 100); x1 = clamp(x1, -100, W + 100); z0 = clamp(z0, -100, W + 100); z1 = clamp(z1, -100, W + 100);
    const side = Math.max(200, x1 - x0, z1 - z0), cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    const ox = cx - side / 2, oz = cz - side / 2, s = DECAL_PX / side;
    this.decalMesh.position.set(cx, 0.8, cz);
    this.decalMesh.scale.set(side, side, 1);

    const ctx = this.dctx, f = view.cur, { U } = this.info, us = this.info.unit_fields.length;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, DECAL_PX, DECAL_PX);
    ctx.setTransform(s, 0, 0, s, -ox * s, -oz * s);
    const zoom = this.cam.zoom;
    this.cam.zoom = s; // the 2D helpers size their lines by zoom
    try {
      this.drawBaseMarkers(ctx, f, t);
      this.drawDangerRings(ctx, view);
      this.drawZones(ctx, f);
      for (let i = 0; i < f.nUnits; i++) {
        const o = i * us, id = f.units[o + U.id], team = f.units[o + U.team], r = f.units[o + U.radius];
        if (f.units[o + U.visible] <= 0 || f.units[o + U.alive] <= 0) continue;
        const x = view.px[i], y = view.py[i];
        if (view.hover === i || ui.selectedId === id) {
          ctx.strokeStyle = team > 1 ? '#ffe9a8' : team !== f.playerTeam ? '#ff6b5e' : '#7dff9a';
          ctx.lineWidth = 3;
          ctx.beginPath(); ctx.arc(x, y, r * 1.45, 0, Math.PI * 2); ctx.stroke();
        }
        if (id === f.player) {
          ctx.strokeStyle = `rgba(255,222,120,${0.65 + 0.3 * Math.sin(t * 5)})`;
          ctx.lineWidth = 3;
          ctx.beginPath(); ctx.arc(x, y, r * 1.7, 0, Math.PI * 2); ctx.stroke();
        }
        const st = f.units[o + U.status];
        if (st) { ctx.save(); ctx.translate(x, y); this.paintStatus(ctx, st & ~this.info.status.stun, r, t); ctx.restore(); }
      }
      this.drawParticles(ctx);
      this.drawTargeting(ctx, view, ui);
      for (const p of this.pings) {
        const k = 1 - p.life / p.max;
        ctx.strokeStyle = p.color; ctx.globalAlpha = 1 - k; ctx.lineWidth = 3;
        ctx.beginPath(); ctx.arc(p.x, p.y, 26 * (1 - k * 0.7), 0, Math.PI * 2); ctx.stroke();
        ctx.globalAlpha = 1;
      }
    } finally {
      this.cam.zoom = zoom;
    }
    this.decalTex.needsUpdate = true;
  }

  /** Stun stars spin above the head, in screen space. */
  drawStuns(ctx, view, t) {
    const f = view.cur, { U } = this.info, us = this.info.unit_fields.length, stun = this.info.status.stun;
    ctx.fillStyle = '#ffe14a';
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us;
      if (!(f.units[o + U.status] & stun) || f.units[o + U.visible] <= 0) continue;
      const m = this.units.get(f.units[o + U.id]), r = f.units[o + U.radius];
      for (let k = 0; k < 3; k++) {
        const a = t * 4 + (k * Math.PI * 2) / 3;
        const [x, y] = this.w2s(view.px[i] + Math.cos(a) * r * 0.9, view.py[i] + Math.sin(a) * r * 0.9, (m ? m.height : r * 2.4) + 2);
        ctx.beginPath();
        for (let j = 0; j < 10; j++) { const rr = j % 2 ? 2 : 5, aa = (j * Math.PI) / 5 - Math.PI / 2; ctx.lineTo(x + Math.cos(aa) * rr, y + Math.sin(aa) * rr); }
        ctx.closePath(); ctx.fill();
      }
    }
  }

  draw(view, ui) {
    const { ctx, W, H, dpr } = this, f = view.cur, t = view.time;
    const sh = this.shake;
    this.syncCamera(sh ? (Math.random() * 2 - 1) * sh * 0.6 : 0, sh ? (Math.random() * 2 - 1) * sh * 0.6 : 0);

    this.drawVision(view);
    this.syncUnits(view, t);
    this.syncProjectiles(f);
    this.drawDecal(view, ui, t);

    // keep the shadow map on what the camera sees
    const c = this.cam, span = Math.min(1600, this.H / c.zoom) * 0.95;
    this.sun.target.position.set(c.x, 0, c.y);
    this.sun.position.copy(this.sunDir).multiplyScalar(1600).add(this.sun.target.position);
    const sc = this.sun.shadow.camera;
    if (sc.right !== span) { sc.left = -span; sc.right = span; sc.top = span; sc.bottom = -span; sc.near = 100; sc.far = 3400; sc.updateProjectionMatrix(); }

    this.gl.render(this.scene, this.camera);
    this.camKey = ''; // the shake offset is only for this frame
    this.syncCamera();

    // screen-space overlay on the 2D canvas
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const order = [];
    for (let i = 0; i < f.nUnits; i++) order.push(i);
    order.sort((a, b) => view.py[a] - view.py[b]);
    this.drawStuns(ctx, view, t);
    for (const i of order) this.drawUnitBars(ctx, view, i, ui);
    this.drawFloaters(ctx);
    if (this.flash > 0.01) {
      const g = ctx.createRadialGradient(W / 2, H / 2, Math.min(W, H) * 0.3, W / 2, H / 2, Math.max(W, H) * 0.75);
      g.addColorStop(0, 'rgba(255,0,0,0)');
      g.addColorStop(1, `rgba(255,30,20,${this.flash})`);
      ctx.fillStyle = g;
      ctx.fillRect(0, 0, W, H);
    }
    this.drawMinimap(view);
  }

  dispose() {
    document.body.classList.remove('three');
    this.scene.traverse((o) => {
      if (o.geometry) o.geometry.dispose();
      if (o.material) { o.material.map?.dispose(); o.material.dispose(); }
    });
    for (const g of Object.values(this.geo)) g.dispose();
    this.visTex.dispose();
    this.gl.renderLists.dispose();
    const ctx = this.ctx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
  }
}
