// 3D view. The drawing happens in render3d.wasm, a C++ renderer (cpp/render/, built by
// ./build-render.sh) that drives WebGPU through the small bridge below. It draws a match from
// either engine: each frame this file copies the units and projectiles out of the current snapshot
// into the renderer's frame buffer, then calls r3d_draw().
//
// What stays in JavaScript is what the 2D view already does well and the 3D view shares with it:
// health bars, damage numbers, the minimap and event effects (inherited from Renderer), plus the
// ground markings (ability zones, tower range, targeting, selection rings, particles), which the 2D
// drawing code paints into a canvas that the renderer lays on the ground under the camera.

import { Renderer, TEAM, buildTerrain, drawHeroPortrait } from './render.js';
import { abilityColor } from './icons.js';

const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const TAU = Math.PI * 2;
const DECAL_PX = 1024;
const KINDS = ['hero', 'melee', 'ranged', 'siege', 'neutral', 'tower', 'ancient']; // renderer numbering
const U_STRIDE = 16, P_STRIDE = 12;
const utf8 = new TextDecoder();

let R = null; // the loaded renderer module: { x: exports, canvases, canvas }

/** Same sRGB -> linear curve as hex() in render3d.cpp. */
function linear(hex) {
  if (typeof hex !== 'string' || !/^#[0-9a-f]{6}$/i.test(hex)) return [1, 1, 1];
  const n = parseInt(hex.slice(1), 16);
  return [n >> 16, (n >> 8) & 255, n & 255].map((c) => { c /= 255; return c * c * (0.75 + 0.25 * c); });
}

/**
 * WebGPU functions for the renderer's `gpu` imports (cpp/render/gpu.h). Objects become integer handles.
 * Fixed here: the two vertex buffer layouts, the 4x multisampled screen target and its depth buffer.
 */
function gpuBridge(device, context, format, canvas, mem, canvases) {
  const objs = [null];
  const add = (o) => { objs.push(o); return objs.length - 1; };
  const str = (p, n) => utf8.decode(new Uint8Array(mem().buffer, p, n));
  const SAMPLES = 4;
  const attr = (loc, off, fmt = 'float32x4') => ({ shaderLocation: loc, offset: off, format: fmt });
  const buffers = [
    { arrayStride: 32, stepMode: 'vertex', attributes: [attr(0, 0, 'float32x3'), attr(1, 12, 'float32x3'), attr(2, 24, 'float32x2')] },
    { arrayStride: 96, stepMode: 'instance', attributes: [attr(3, 0), attr(4, 16), attr(5, 32), attr(6, 48), attr(7, 64), attr(8, 80)] },
  ];
  const samplers = [
    device.createSampler({ magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear', maxAnisotropy: 8 }),
    device.createSampler({ magFilter: 'linear', minFilter: 'linear', compare: 'less-equal' }),
  ];
  let screen = null; // { w, h, color, depth } multisampled targets, remade when the canvas resizes
  let encoder = null, pass = null, entries = null;

  const blends = [
    undefined,
    { color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' }, alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } },
    { color: { srcFactor: 'src-alpha', dstFactor: 'one' }, alpha: { srcFactor: 'zero', dstFactor: 'one' } },
  ];
  const copyCanvas = (tex, src, mips) => {
    let c = src;
    for (let level = 0; level < mips; level++) {
      if (level) { // each mip level is the canvas scaled down by the browser
        const d = document.createElement('canvas');
        d.width = Math.max(1, c.width >> 1); d.height = Math.max(1, c.height >> 1);
        d.getContext('2d').drawImage(c, 0, 0, d.width, d.height);
        c = d;
      }
      device.queue.copyExternalImageToTexture({ source: c }, { texture: tex, mipLevel: level }, [c.width, c.height]);
    }
  };

  return {
    shader(p, n) {
      const code = str(p, n), m = device.createShaderModule({ code });
      m.getCompilationInfo?.().then((info) => { for (const msg of info.messages) if (msg.type === 'error') console.error('render3d WGSL:', msg.lineNum, msg.message); });
      return add(m);
    },
    pipeline(sh, vs, vl, fs, fl, target, blend, depthWrite, bias) {
      const shadow = target === 1, fsName = str(fs, fl);
      return add(device.createRenderPipeline({
        layout: 'auto',
        vertex: { module: objs[sh], entryPoint: str(vs, vl), buffers },
        fragment: fsName ? { module: objs[sh], entryPoint: fsName, targets: [{ format, blend: blends[blend] }] } : undefined,
        primitive: { topology: 'triangle-list', cullMode: 'none' },
        depthStencil: {
          format: shadow ? 'depth32float' : 'depth24plus', depthWriteEnabled: !!depthWrite, depthCompare: 'less',
          ...(bias ? { depthBias: 2, depthBiasSlopeScale: 2.5 } : {}),
        },
        multisample: { count: shadow ? 1 : SAMPLES },
      }));
    },
    buffer: (bytes, uniform) => add(device.createBuffer({
      size: Math.max(16, (bytes + 15) & ~15),
      usage: (uniform ? GPUBufferUsage.UNIFORM : GPUBufferUsage.VERTEX) | GPUBufferUsage.COPY_DST,
    })),
    write_buffer: (b, off, p, n) => device.queue.writeBuffer(objs[b], off, mem().buffer, p, n),
    texture: (w, h, fmt) => add(device.createTexture({
      size: [w, h],
      format: fmt === 1 ? 'depth32float' : 'r8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | (fmt === 1 ? GPUTextureUsage.RENDER_ATTACHMENT : GPUTextureUsage.COPY_DST),
    })),
    write_texture: (t, p, w, h, row) => device.queue.writeTexture({ texture: objs[t] }, mem().buffer, { offset: p, bytesPerRow: row }, [w, h]),
    canvas_texture(old, slot, mipmaps) {
      objs[old]?.destroy();
      const c = canvases[slot], mips = mipmaps ? Math.floor(Math.log2(Math.max(c.width, c.height))) + 1 : 1;
      const tex = device.createTexture({
        size: [c.width, c.height], format: 'rgba8unorm-srgb', mipLevelCount: mips,
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
      });
      copyCanvas(tex, c, mips);
      if (old) { objs[old] = tex; return old; }
      return add(tex);
    },
    update_canvas_texture: (t, slot) => copyCanvas(objs[t], canvases[slot], 1),
    group_begin: () => { entries = []; },
    group_buffer: (binding, b) => entries.push({ binding, resource: { buffer: objs[b] } }),
    group_texture: (binding, t) => entries.push({ binding, resource: objs[t].createView() }),
    group_sampler: (binding, kind) => entries.push({ binding, resource: samplers[kind] }),
    group_end: (p) => add(device.createBindGroup({ layout: objs[p].getBindGroupLayout(0), entries })),
    begin_pass(target, depthTex, r, g, b) {
      encoder ||= device.createCommandEncoder();
      if (target === 1) {
        pass = encoder.beginRenderPass({
          colorAttachments: [],
          depthStencilAttachment: { view: objs[depthTex].createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'store' },
        });
        return;
      }
      const w = canvas.width, h = canvas.height;
      if (!screen || screen.w !== w || screen.h !== h) {
        screen?.color.destroy(); screen?.depth.destroy();
        const mk = (f) => device.createTexture({ size: [w, h], format: f, sampleCount: SAMPLES, usage: GPUTextureUsage.RENDER_ATTACHMENT });
        screen = { w, h, color: mk(format), depth: mk('depth24plus') };
      }
      pass = encoder.beginRenderPass({
        colorAttachments: [{
          view: screen.color.createView(), resolveTarget: context.getCurrentTexture().createView(),
          clearValue: { r, g, b, a: 1 }, loadOp: 'clear', storeOp: 'discard',
        }],
        depthStencilAttachment: { view: screen.depth.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'discard' },
      });
    },
    set_pipeline: (p) => pass.setPipeline(objs[p]),
    set_group: (g) => pass.setBindGroup(0, objs[g]),
    set_vertex_buffer: (slot, b) => pass.setVertexBuffer(slot, objs[b]),
    draw: (verts, inst, firstVert, firstInst) => pass.draw(verts, inst, firstVert, firstInst),
    end_pass: () => { pass.end(); pass = null; },
    submit: () => { device.queue.submit([encoder.finish()]); encoder = null; },
  };
}

/**
 * Load render3d.wasm and set up WebGPU on the #world3d canvas. Resolves to the Renderer3D class;
 * throws when the browser has no WebGPU or the module has not been built.
 */
export async function load() {
  if (R) return Renderer3D;
  if (!navigator.gpu) throw new Error('this browser has no WebGPU');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter (GPU blocklisted or disabled?)');
  const device = await adapter.requestDevice();
  device.lost.then((info) => {
    console.error('WebGPU device lost:', info.message);
    if (R) R.lost = true;
    window.dispatchEvent(new Event('render3d-lost')); // main.js drops back to the 2D view
  });
  device.addEventListener?.('uncapturederror', (e) => console.error('WebGPU:', e.error.message));
  const canvas = document.getElementById('world3d');
  const context = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'opaque' });

  const res = await fetch('render3d.wasm', { cache: 'no-cache' });
  if (!res.ok) throw new Error(`could not load render3d.wasm (HTTP ${res.status}); build it with ./build-render.sh`);
  const module = await WebAssembly.compile(await res.arrayBuffer());
  const canvases = [];
  let exports = null;
  const bridge = gpuBridge(device, context, format, canvas, () => exports.memory, canvases);
  // gpu.* comes from the bridge; anything else a toolchain adds (Emscripten / WASI stubs) is inert
  const imports = {};
  for (const imp of WebAssembly.Module.imports(module)) {
    const ns = (imports[imp.module] ||= {});
    if (imp.kind === 'memory') { ns[imp.name] = new WebAssembly.Memory({ initial: 256, maximum: 32768 }); continue; }
    if (imp.kind !== 'function') continue;
    if (imp.module === 'gpu' && !bridge[imp.name]) throw new Error(`render3d.wasm wants gpu.${imp.name}, which this page does not provide`);
    ns[imp.name] = imp.module === 'gpu' ? bridge[imp.name] : () => 0;
  }
  const instance = await WebAssembly.instantiate(module, imports);
  exports = instance.exports;
  if (typeof exports._initialize === 'function') exports._initialize();
  R = { x: exports, canvases, canvas, adapter, device }; // keep the adapter referenced, or Dawn may drop the device
  return Renderer3D;
}

export class Renderer3D extends Renderer {
  is3D = true;

  constructor(canvas, mini, info, terrain) {
    super(canvas, mini, info, terrain);
    const x = R.x;
    this.x = x;
    document.body.classList.add('three');

    // engine kind value -> renderer kind
    this.kindMap = [];
    KINDS.forEach((name, i) => { if (info.kind[name] !== undefined) this.kindMap[info.kind[name]] = i; });

    // canvases the renderer uploads: 0 ground, 1 ground markings, 2 hero emblems (teams across, heroes down)
    this.decal = document.createElement('canvas');
    this.decal.width = this.decal.height = DECAL_PX;
    this.dctx = this.decal.getContext('2d');
    const atlas = document.createElement('canvas'), n = Math.max(1, info.heroes.length);
    atlas.width = 256; atlas.height = 128 * n;
    const tile = document.createElement('canvas');
    tile.width = tile.height = 128;
    for (let h = 0; h < n; h++) for (let t = 0; t < 2; t++) {
      drawHeroPortrait(tile, h, t);
      atlas.getContext('2d').drawImage(tile, t * 128, h * 128);
    }
    R.canvases[0] = buildTerrain(info, terrain, { trees: false });
    R.canvases[1] = this.decal;
    R.canvases[2] = atlas;

    new Uint8Array(x.memory.buffer, x.r3d_terrain_buf(), info.grid * info.grid).set(terrain);
    const [b0, b1] = info.bases;
    if (!x.r3d_init(info.world, info.grid, info.cell, info.heroes.length, b0[0], b0[1], b1[0], b1[1])) throw new Error('3D renderer failed to start (see console)');
    this.camKey = '';
    this.resize();
  }

  // ---- camera (the renderer owns it; picking reads its matrices back)

  resize() {
    super.resize();
    if (!this.x) return;
    const c = R.canvas;
    c.width = Math.round(this.W * this.dpr);
    c.height = Math.round(this.H * this.dpr);
    c.style.width = this.W + 'px';
    c.style.height = this.H + 'px';
    this.camKey = '';
  }

  syncCamera(ox = 0, oz = 0) {
    const c = this.cam, key = `${c.x},${c.y},${c.zoom},${ox},${oz},${this.W},${this.H}`;
    if (key === this.camKey) return;
    this.camKey = key;
    this.x.r3d_camera(c.x, c.y, c.zoom, this.W, this.H, ox, oz);
    this.camF = new Float32Array(this.x.memory.buffer, this.x.r3d_cam_ptr(), 36);
  }

  w2s(x, y, h = 0) {
    this.syncCamera();
    const m = this.camF;
    const cx = m[0] * x + m[4] * h + m[8] * y + m[12], cy = m[1] * x + m[5] * h + m[9] * y + m[13], cw = m[3] * x + m[7] * h + m[11] * y + m[15];
    if (cw <= 0) return [-1e4, -1e4]; // behind the camera
    return [((cx / cw + 1) / 2) * this.W, ((1 - cy / cw) / 2) * this.H];
  }

  s2w(sx, sy) {
    this.syncCamera();
    const m = this.camF, nx = (sx / this.W) * 2 - 1, ny = 1 - (sy / this.H) * 2;
    const un = (z) => {
      const w = m[19] * nx + m[23] * ny + m[27] * z + m[31];
      return [(m[16] * nx + m[20] * ny + m[24] * z + m[28]) / w, (m[17] * nx + m[21] * ny + m[25] * z + m[29]) / w, (m[18] * nx + m[22] * ny + m[26] * z + m[30]) / w];
    };
    const a = un(0), b = un(1), d = [b[0] - a[0], b[1] - a[1], b[2] - a[2]]; // WebGPU clip depth runs 0..1
    const t = d[1] < -1e-6 ? -a[1] / d[1] : 20;
    return [a[0] + d[0] * t, a[2] + d[2] * t];
  }

  heightOf(i, view) {
    const u = view.cur.units, o = i * this.info.unit_fields.length, { U } = this.info;
    return this.x.r3d_height(this.kindMap[u[o + U.kind]] ?? 4, u[o + U.radius]);
  }

  barAnchor(view, i) {
    return this.w2s(view.px[i], view.py[i], this.heightOf(i, view) + 6);
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

  // ---- per frame

  /** Paint the 2D ground markings into the decal canvas covering what the camera sees. Returns [cx, cz, side]. */
  drawDecal(view, ui, t) {
    const W = this.info.world;
    let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
    for (const [sx, sy] of [[0, 0], [this.W, 0], [this.W, this.H], [0, this.H]]) {
      const [x, z] = this.s2w(sx, sy);
      x0 = Math.min(x0, x); x1 = Math.max(x1, x); z0 = Math.min(z0, z); z1 = Math.max(z1, z);
    }
    x0 = clamp(x0, -100, W + 100); x1 = clamp(x1, -100, W + 100); z0 = clamp(z0, -100, W + 100); z1 = clamp(z1, -100, W + 100);
    const side = Math.max(200, x1 - x0, z1 - z0), cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    const ox = cx - side / 2, oz = cz - side / 2, s = DECAL_PX / side;

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
          ctx.beginPath(); ctx.arc(x, y, r * 1.45, 0, TAU); ctx.stroke();
        }
        if (id === f.player) {
          ctx.strokeStyle = `rgba(255,222,120,${0.65 + 0.3 * Math.sin(t * 5)})`;
          ctx.lineWidth = 3;
          ctx.beginPath(); ctx.arc(x, y, r * 1.7, 0, TAU); ctx.stroke();
        }
        const st = f.units[o + U.status];
        if (st) { ctx.save(); ctx.translate(x, y); this.paintStatus(ctx, st & ~this.info.status.stun, r, t); ctx.restore(); }
      }
      this.drawParticles(ctx);
      this.drawTargeting(ctx, view, ui);
      for (const p of this.pings) {
        const k = 1 - p.life / p.max;
        ctx.strokeStyle = p.color; ctx.globalAlpha = 1 - k; ctx.lineWidth = 3;
        ctx.beginPath(); ctx.arc(p.x, p.y, 26 * (1 - k * 0.7), 0, TAU); ctx.stroke();
        ctx.globalAlpha = 1;
      }
    } finally {
      this.cam.zoom = zoom;
    }
    return [cx, cz, side];
  }

  /** Copy this frame's units and projectiles into the renderer's frame buffer. */
  packFrame(view) {
    const f = view.cur, info = this.info, { U, P } = info, us = info.unit_fields.length, ps = info.proj_fields.length;
    const max = this.x.r3d_max_units();
    const buf = new Float32Array(this.x.memory.buffer, this.x.r3d_frame_buf(), max * (U_STRIDE + P_STRIDE));
    const nu = Math.min(f.nUnits, max), u = f.units;
    for (let i = 0; i < nu; i++) {
      const o = i * us, b = i * U_STRIDE, kind = this.kindMap[u[o + U.kind]] ?? 4, team = u[o + U.team];
      let lx = 0, lz = 0;
      const sw = u[o + U.swing_t], sl = u[o + U.swing_len], facing = u[o + U.facing];
      if (sw > 0 && sl > 0 && u[o + U.range] < 150) { // melee lunge while winding up an attack
        const l = Math.sin((1 - sw / sl) * Math.PI) * 7;
        lx = Math.cos(facing) * l; lz = Math.sin(facing) * l;
      }
      buf[b] = u[o + U.id]; buf[b + 1] = kind; buf[b + 2] = team; buf[b + 3] = u[o + U.def];
      buf[b + 4] = view.px[i]; buf[b + 5] = view.py[i]; buf[b + 6] = facing; buf[b + 7] = u[o + U.radius];
      buf[b + 8] = u[o + U.visible]; buf[b + 9] = u[o + U.alive]; buf[b + 10] = lx; buf[b + 11] = lz;
      buf[b + 12] = team === f.playerTeam ? u[o + U.vision] : 0;
      buf[b + 13] = kind === 0 ? info.heroes[u[o + U.def]]?.primary ?? 0 : 0;
    }
    const np = Math.min(f.projs.length / ps, max), pr = f.projs, base = max * U_STRIDE;
    for (let k = 0; k < np; k++) {
      const i = k * ps, b = base + k * P_STRIDE, ab = pr[i + P.ability], attack = ab === 255;
      const [r, g, bl] = linear(attack ? TEAM[pr[i + P.team]]?.light : abilityColor(info, ab));
      buf[b] = pr[i + P.x]; buf[b + 1] = pr[i + P.y]; buf[b + 2] = pr[i + P.dx]; buf[b + 3] = pr[i + P.dy];
      buf[b + 4] = attack ? 4 : pr[i + P.homing] ? 7 : Math.max(8, pr[i + P.radius] * 0.5);
      buf[b + 5] = attack ? 18 : pr[i + P.homing] ? 26 : 50;
      buf[b + 6] = attack ? 34 : 26;
      buf[b + 7] = r; buf[b + 8] = g; buf[b + 9] = bl;
    }
    return [nu, np];
  }

  /** Stun stars spin above the head, in screen space. */
  drawStuns(ctx, view, t) {
    const f = view.cur, { U } = this.info, us = this.info.unit_fields.length, stun = this.info.status.stun;
    ctx.fillStyle = '#ffe14a';
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us;
      if (!(f.units[o + U.status] & stun) || f.units[o + U.visible] <= 0) continue;
      const r = f.units[o + U.radius], h = this.heightOf(i, view) + 2;
      for (let k = 0; k < 3; k++) {
        const a = t * 4 + (k * TAU) / 3;
        const [x, y] = this.w2s(view.px[i] + Math.cos(a) * r * 0.9, view.py[i] + Math.sin(a) * r * 0.9, h);
        ctx.beginPath();
        for (let j = 0; j < 10; j++) { const rr = j % 2 ? 2 : 5, aa = (j * Math.PI) / 5 - Math.PI / 2; ctx.lineTo(x + Math.cos(aa) * rr, y + Math.sin(aa) * rr); }
        ctx.closePath(); ctx.fill();
      }
    }
  }

  draw(view, ui) {
    const { ctx, W, H, dpr } = this, f = view.cur, t = view.time;
    if (R.lost) return;
    const sh = this.shake;
    this.syncCamera(sh ? (Math.random() * 2 - 1) * sh * 0.6 : 0, sh ? (Math.random() * 2 - 1) * sh * 0.6 : 0);
    const [dx, dz, side] = this.drawDecal(view, ui, t);
    const [nu, np] = this.packFrame(view);
    this.x.r3d_draw(nu, np, dx, dz, side, t, f.g[this.info.G.fog] ? 1 : 0);
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
    const ctx = this.ctx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
  }
}
