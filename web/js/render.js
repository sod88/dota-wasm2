// Canvas renderer: world, effects, fog of war and minimap. Pure drawing — no game logic.

import { abilityColor } from './icons.js';

const TAU = Math.PI * 2;
const FOG_SCALE = 0.25; // fog canvas pixels per CSS pixel (see drawFog)
export const TEAM = [
  { main: '#4fd17a', light: '#a8f5c2', dark: '#1b6e42', rgb: '79,209,122', name: 'Radiant' },
  { main: '#ff5d52', light: '#ffb0a8', dark: '#842019', rgb: '255,93,82', name: 'Dire' },
];
export const NEUTRAL_COLORS = ['#c9a45f', '#a8794a', '#8fae5a', '#8a7ab0'];
const DMG_COLORS = ['#ffffff', '#7fd0ff', '#e49bff'];

const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const rgba = (rgb, a) => `rgba(${rgb},${a})`;
function hexRgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return `${n >> 16},${(n >> 8) & 255},${n & 255}`;
}
function rnd(a, b) {
  return a + Math.random() * (b - a);
}
function cellRand(cx, cy, salt) {
  let h = Math.imul(cx + 101, 73856093) ^ Math.imul(cy + 57, 19349663) ^ Math.imul(salt + 13, 83492791);
  h = Math.imul(h ^ (h >>> 15), 2246822507);
  h = Math.imul(h ^ (h >>> 13), 3266489909);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

// ------------------------------------------------------------------------------ terrain

/** The map as one big canvas. `trees: false` leaves the forest floor bare (the 3D view grows real trees). */
export function buildTerrain(info, terrain, { trees = true } = {}) {
  const S = 0.75, N = info.grid, cp = info.cell * S;
  const size = Math.round(info.world * S);
  const c = document.createElement('canvas');
  c.width = c.height = size;
  const ctx = c.getContext('2d');
  const T = (cx, cy) => (cx < 0 || cy < 0 || cx >= N || cy >= N ? 0 : terrain[cy * N + cx]);
  ctx.fillStyle = '#0a110d';
  ctx.fillRect(0, 0, size, size);

  for (let cy = 0; cy < N; cy++) {
    for (let cx = 0; cx < N; cx++) {
      const t = T(cx, cy), n = cellRand(cx, cy, 1);
      ctx.fillStyle =
        t === 0 ? `hsl(150,24%,${6 + n * 4}%)` :
        t === 1 ? `hsl(96,24%,${16 + n * 6}%)` :
        t === 2 ? `hsl(36,30%,${27 + n * 6}%)` :
                  `hsl(205,55%,${23 + n * 8}%)`;
      ctx.fillRect(cx * cp, cy * cp, cp + 1, cp + 1);
    }
  }
  // soft shadow along every wall edge
  for (let cy = 0; cy < N; cy++) {
    for (let cx = 0; cx < N; cx++) {
      if (T(cx, cy) === 0) continue;
      for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        if (T(cx + dx, cy + dy) !== 0) continue;
        const x = cx * cp, y = cy * cp;
        const g = dx
          ? ctx.createLinearGradient(dx > 0 ? x + cp : x, 0, dx > 0 ? x + cp - cp * 0.8 : x + cp * 0.8, 0)
          : ctx.createLinearGradient(0, dy > 0 ? y + cp : y, 0, dy > 0 ? y + cp - cp * 0.8 : y + cp * 0.8);
        g.addColorStop(0, 'rgba(0,0,0,.42)');
        g.addColorStop(1, 'rgba(0,0,0,0)');
        ctx.fillStyle = g;
        ctx.fillRect(x, y, cp, cp);
      }
    }
  }
  // texture: grass tufts, lane pebbles, river ripples
  for (let cy = 0; cy < N; cy++) {
    for (let cx = 0; cx < N; cx++) {
      const t = T(cx, cy);
      if (t === 0) continue;
      for (let k = 0; k < 3; k++) {
        const px = (cx + cellRand(cx, cy, 10 + k)) * cp, py = (cy + cellRand(cx, cy, 20 + k)) * cp;
        if (t === 1) {
          ctx.strokeStyle = 'rgba(150,200,110,.18)';
          ctx.lineWidth = 1;
          ctx.beginPath(); ctx.moveTo(px, py); ctx.lineTo(px - 1.5, py - 4); ctx.moveTo(px, py); ctx.lineTo(px + 1.5, py - 3.5); ctx.stroke();
        } else if (t === 2) {
          ctx.fillStyle = k % 2 ? 'rgba(255,230,180,.10)' : 'rgba(0,0,0,.12)';
          ctx.beginPath(); ctx.arc(px, py, 1 + cellRand(cx, cy, 30 + k) * 1.4, 0, TAU); ctx.fill();
        } else {
          ctx.strokeStyle = 'rgba(190,230,255,.17)';
          ctx.lineWidth = 1;
          ctx.beginPath(); ctx.moveTo(px - 5, py); ctx.quadraticCurveTo(px, py - 3, px + 5, py); ctx.stroke();
        }
      }
    }
  }
  // trees on the walls
  for (let cy = 0; trees && cy < N; cy++) {
    for (let cx = 0; cx < N; cx++) {
      if (T(cx, cy) !== 0) continue;
      let open = 0;
      for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) if (T(cx + dx, cy + dy) !== 0) open++;
      const trees = open > 0 ? 2 + Math.floor(cellRand(cx, cy, 40) * 2) : 1;
      for (let k = 0; k < trees; k++) {
        const x = (cx + cellRand(cx, cy, 50 + k)) * cp, y = (cy + cellRand(cx, cy, 60 + k)) * cp;
        const r = cp * (0.55 + cellRand(cx, cy, 70 + k) * 0.55);
        const lum = open > 0 ? 17 : 11;
        const g = ctx.createRadialGradient(x - r * 0.3, y - r * 0.35, r * 0.1, x, y, r);
        g.addColorStop(0, `hsl(135,34%,${lum + 9}%)`);
        g.addColorStop(1, `hsl(150,38%,${lum - 7}%)`);
        ctx.fillStyle = 'rgba(0,0,0,.28)';
        ctx.beginPath(); ctx.ellipse(x + 2, y + r * 0.8, r * 0.9, r * 0.35, 0, 0, TAU); ctx.fill();
        ctx.fillStyle = g;
        ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.fill();
      }
    }
  }
  // base glows
  info.bases.forEach(([bx, by], i) => {
    const g = ctx.createRadialGradient(bx * S, by * S, 10, bx * S, by * S, 270 * S);
    g.addColorStop(0, rgba(TEAM[i].rgb, 0.3));
    g.addColorStop(1, rgba(TEAM[i].rgb, 0));
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, size, size);
    // fountain
    ctx.strokeStyle = 'rgba(150,220,255,.55)';
    ctx.lineWidth = 3;
    ctx.beginPath(); ctx.arc(bx * S, by * S, 66 * S, 0, TAU); ctx.stroke();
    ctx.fillStyle = 'rgba(70,150,220,.28)';
    ctx.fill();
  });
  // jungle camps
  for (const camp of info.camps) {
    ctx.strokeStyle = 'rgba(0,0,0,.35)';
    ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(camp.x * S, camp.y * S, 52 * S, 0, TAU); ctx.stroke();
  }
  return c;
}

// ------------------------------------------------------------------------------ glyphs

// Hero emblems. A mod picks one by name (`"glyph": "bolt"`) and may tint it (`"color"`); heroes without a
// glyph get one from their primary attribute, which is what the three built-in heroes use.
const HERO_GLYPHS = {
  shield(ctx, r) {
    ctx.beginPath(); ctx.moveTo(-r * 0.7, -r * 0.6); ctx.lineTo(r * 0.7, -r * 0.6); ctx.lineTo(r * 0.7, r * 0.1); ctx.quadraticCurveTo(r * 0.6, r * 0.8, 0, r * 0.95); ctx.quadraticCurveTo(-r * 0.6, r * 0.8, -r * 0.7, r * 0.1); ctx.closePath(); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(0, -r * 0.45); ctx.lineTo(0, r * 0.65); ctx.moveTo(-r * 0.4, -r * 0.05); ctx.lineTo(r * 0.4, -r * 0.05); ctx.stroke();
  },
  bow(ctx, r) {
    ctx.beginPath(); ctx.arc(-r * 0.25, 0, r * 0.85, -1.15, 1.15); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(-r * 0.25 + Math.cos(-1.15) * r * 0.85, Math.sin(-1.15) * r * 0.85); ctx.lineTo(-r * 0.25 + Math.cos(1.15) * r * 0.85, Math.sin(1.15) * r * 0.85); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(-r * 0.4, 0); ctx.lineTo(r * 0.85, 0); ctx.lineTo(r * 0.55, -r * 0.25); ctx.moveTo(r * 0.85, 0); ctx.lineTo(r * 0.55, r * 0.25); ctx.stroke();
  },
  star(ctx, r) {
    ctx.beginPath();
    for (let i = 0; i < 8; i++) { const a = (i * Math.PI) / 4 - Math.PI / 2, rr = i % 2 ? r * 0.3 : r * 0.95; ctx.lineTo(Math.cos(a) * rr, Math.sin(a) * rr); }
    ctx.closePath(); ctx.fill();
  },
  sword(ctx, r) {
    ctx.beginPath(); ctx.moveTo(-r * 0.62, r * 0.62); ctx.lineTo(r * 0.7, -r * 0.7); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(-r * 0.62, r * 0.12); ctx.lineTo(-r * 0.12, r * 0.62); ctx.stroke();
    ctx.beginPath(); ctx.arc(-r * 0.74, r * 0.74, r * 0.12, 0, TAU); ctx.fill();
  },
  bolt(ctx, r) {
    ctx.beginPath(); ctx.moveTo(r * 0.15, -r * 0.95); ctx.lineTo(-r * 0.5, r * 0.12); ctx.lineTo(-r * 0.02, r * 0.12); ctx.lineTo(-r * 0.2, r * 0.95); ctx.lineTo(r * 0.55, -r * 0.18); ctx.lineTo(r * 0.04, -r * 0.18); ctx.closePath(); ctx.fill();
  },
  moon(ctx, r) {
    ctx.beginPath(); ctx.arc(0, 0, r * 0.85, Math.PI * 0.32, Math.PI * 1.68); ctx.arc(r * 0.38, 0, r * 0.62, Math.PI * 1.55, Math.PI * 0.45, true); ctx.closePath(); ctx.fill();
  },
  flame(ctx, r) {
    ctx.beginPath(); ctx.moveTo(0, -r * 0.95);
    ctx.bezierCurveTo(r * 0.8, -r * 0.2, r * 0.6, r * 0.9, 0, r * 0.9); ctx.bezierCurveTo(-r * 0.65, r * 0.9, -r * 0.7, r * 0.05, -r * 0.2, -r * 0.25);
    ctx.bezierCurveTo(-r * 0.15, r * 0.05, 0, r * 0.05, 0, -r * 0.95); ctx.fill();
  },
  crown(ctx, r) {
    ctx.beginPath(); ctx.moveTo(-r * 0.8, r * 0.55); ctx.lineTo(-r * 0.8, -r * 0.45); ctx.lineTo(-r * 0.4, -r * 0.05); ctx.lineTo(0, -r * 0.7); ctx.lineTo(r * 0.4, -r * 0.05); ctx.lineTo(r * 0.8, -r * 0.45); ctx.lineTo(r * 0.8, r * 0.55); ctx.closePath(); ctx.fill();
  },
  leaf(ctx, r) {
    ctx.beginPath(); ctx.moveTo(-r * 0.75, r * 0.75); ctx.quadraticCurveTo(-r * 0.75, -r * 0.75, r * 0.8, -r * 0.8); ctx.quadraticCurveTo(r * 0.75, r * 0.75, -r * 0.75, r * 0.75); ctx.fill();
    ctx.strokeStyle = 'rgba(0,0,0,.35)'; ctx.beginPath(); ctx.moveTo(-r * 0.6, r * 0.6); ctx.lineTo(r * 0.5, -r * 0.5); ctx.stroke();
  },
  skull(ctx, r) {
    ctx.beginPath(); ctx.arc(0, -r * 0.12, r * 0.7, Math.PI * 0.85, Math.PI * 2.15); ctx.lineTo(r * 0.42, r * 0.85); ctx.lineTo(-r * 0.42, r * 0.85); ctx.closePath(); ctx.fill();
    ctx.fillStyle = 'rgba(0,0,0,.6)';
    ctx.beginPath(); ctx.arc(-r * 0.28, -r * 0.1, r * 0.17, 0, TAU); ctx.arc(r * 0.28, -r * 0.1, r * 0.17, 0, TAU); ctx.fill();
  },
};
/** Names a mod may use for a hero's `glyph`. */
export const HERO_GLYPH_NAMES = Object.keys(HERO_GLYPHS);
const PRIMARY_GLYPH = ['shield', 'bow', 'star'];
let heroStyles = [];

/** Remember each hero's glyph and tint for the current content (call whenever mods change). */
export function setHeroStyles(info) {
  heroStyles = info.heroes.map((h) => ({
    glyph: HERO_GLYPHS[h.glyph] ? h.glyph : PRIMARY_GLYPH[h.primary] || 'star',
    color: h.color || 'rgba(255,255,255,.92)',
  }));
}

/** Class emblem for hero `def`, centred at (x,y) with half-size r. */
export function drawHeroGlyph(ctx, def, x, y, r) {
  const st = heroStyles[def] || { glyph: PRIMARY_GLYPH[def] || 'star', color: 'rgba(255,255,255,.92)' };
  ctx.save();
  ctx.translate(x, y);
  ctx.fillStyle = ctx.strokeStyle = st.color;
  ctx.lineWidth = Math.max(1.2, r * 0.16);
  ctx.lineCap = ctx.lineJoin = 'round';
  HERO_GLYPHS[st.glyph](ctx, r);
  ctx.restore();
}

/** Paint a hero portrait on a small canvas (menu cards, HUD). */
export function drawHeroPortrait(canvas, def, team = 0) {
  const ctx = canvas.getContext('2d'), s = canvas.width, c = TEAM[team];
  ctx.clearRect(0, 0, s, s);
  const g = ctx.createRadialGradient(s * 0.35, s * 0.3, s * 0.05, s / 2, s / 2, s * 0.5);
  g.addColorStop(0, c.light);
  g.addColorStop(0.35, c.main);
  g.addColorStop(1, c.dark);
  ctx.fillStyle = g;
  ctx.beginPath(); ctx.arc(s / 2, s / 2, s * 0.46, 0, TAU); ctx.fill();
  ctx.lineWidth = s * 0.05;
  ctx.strokeStyle = '#e8c875';
  ctx.stroke();
  drawHeroGlyph(ctx, def, s / 2, s / 2, s * 0.26);
}

// ------------------------------------------------------------------------------ renderer

export class Renderer {
  constructor(canvas, mini, info, terrain) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.mini = mini;
    this.mctx = mini.getContext('2d');
    this.info = info;
    this.bg = buildTerrain(info, terrain);
    this.cam = { x: info.bases[0][0], y: info.bases[0][1], zoom: 0.8 };
    this.parts = [];
    this.floaters = [];
    this.pings = [];
    this.shake = 0;
    this.flash = 0;
    this.fogCanvas = document.createElement('canvas');
    this.mfog = document.createElement('canvas');
    this.dpr = 1;
    this.W = this.H = 0;
    this.resize();
  }

  resize() {
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    this.dpr = dpr;
    this.W = window.innerWidth;
    this.H = window.innerHeight;
    this.canvas.width = Math.round(this.W * dpr);
    this.canvas.height = Math.round(this.H * dpr);
    this.canvas.style.width = this.W + 'px';
    this.canvas.style.height = this.H + 'px';
    this.fogCanvas.width = Math.ceil(this.W * FOG_SCALE);
    this.fogCanvas.height = Math.ceil(this.H * FOG_SCALE);
    const m = Math.round(this.mini.clientWidth * dpr) || 220;
    this.mini.width = this.mini.height = m;
    this.mfog.width = this.mfog.height = m;
  }

  // ---- camera
  w2s(x, y) {
    return [(x - this.cam.x) * this.cam.zoom + this.W / 2, (y - this.cam.y) * this.cam.zoom + this.H / 2];
  }
  s2w(sx, sy) {
    return [(sx - this.W / 2) / this.cam.zoom + this.cam.x, (sy - this.H / 2) / this.cam.zoom + this.cam.y];
  }
  clampCam() {
    const w = this.info.world, c = this.cam;
    c.x = clamp(c.x, 0, w);
    c.y = clamp(c.y, 0, w);
    c.zoom = clamp(c.zoom, 0.32, 1.7);
  }

  // ---- effects from engine events
  onEvents(frame, ui) {
    const { E } = this.info, ev = this.info.ev, es = this.info.event_fields.length;
    const me = frame.player;
    for (let i = 0; i < frame.events.length; i += es) {
      const e = frame.events, t = e[i + E.type], a = e[i + E.a], b = e[i + E.b], x = e[i + E.x], y = e[i + E.y], v = e[i + E.v], w = e[i + E.w], tm = e[i + E.t];
      if (!e[i + E.visible]) continue;
      if (t === ev.damage) {
        const dmg = Math.round(v), victimIsMe = a === me, byMe = b === me;
        const idx = frame.index.get(a), isHero = idx !== undefined && frame.units[idx * this.info.unit_fields.length + this.info.U.kind] === this.info.kind.hero;
        if (victimIsMe || byMe || isHero || dmg >= 70) {
          this.floaters.push({ x: x + rnd(-8, 8), y: y - 18, text: String(dmg), color: victimIsMe ? '#ff6a5c' : DMG_COLORS[w] || '#fff', size: clamp(11 + dmg / 22, 11, 22), life: 0.9, max: 0.9, vy: 38 });
        }
        for (let k = 0; k < 3; k++) this.parts.push({ type: 'dot', x, y, vx: rnd(-70, 70), vy: rnd(-70, 40), life: 0.3, max: 0.3, size: rnd(1.5, 3), color: DMG_COLORS[w] || '#fff' });
        if (victimIsMe) this.flash = Math.min(0.5, this.flash + 0.18);
      } else if (t === ev.kill) {
        const kind = w, team = tm;
        const col = team < 2 ? TEAM[team].rgb : '220,180,100';
        const n = kind === this.info.kind.hero ? 26 : 8;
        for (let k = 0; k < n; k++) this.parts.push({ type: 'dot', x, y, vx: rnd(-130, 130), vy: rnd(-130, 130), life: rnd(0.4, 0.8), max: 0.8, size: rnd(2, 4.5), color: `rgb(${col})` });
        if (kind === this.info.kind.hero) this.parts.push({ type: 'ring', x, y, r0: 10, r1: 130, life: 0.6, max: 0.6, color: `rgb(${col})`, width: 4 });
      } else if (t === ev.building) {
        const col = tm < 2 ? TEAM[tm].rgb : '255,200,120';
        for (let k = 0; k < 46; k++) this.parts.push({ type: 'dot', x, y, vx: rnd(-240, 240), vy: rnd(-240, 120), life: rnd(0.6, 1.5), max: 1.5, size: rnd(3, 8), color: Math.random() < 0.5 ? `rgb(${col})` : '#ffcf7a' });
        for (let k = 0; k < 3; k++) this.parts.push({ type: 'ring', x, y, r0: 20, r1: 220 + k * 60, life: 0.8 + k * 0.2, max: 1, color: '#ffb35c', width: 5 });
        this.shake = Math.max(this.shake, b === this.info.kind.ancient ? 20 : 10);
      } else if (t === ev.cast) {
        const id = w, col = id < 100 ? abilityColor(this.info, id) : '#cfd8ff';
        const casterIdx = frame.index.get(a);
        const rad = v > 0 ? v : 46;
        this.parts.push({ type: 'ring', x, y, r0: 8, r1: rad, life: 0.45, max: 0.45, color: col, width: 4 });
        if (v > 0) this.parts.push({ type: 'disc', x, y, r1: rad, life: 0.3, max: 0.3, color: col });
        if (casterIdx !== undefined && b < 4) { /* caster flash handled by ring */ }
      } else if (t === ev.blink) {
        this.parts.push({ type: 'ring', x, y, r0: 6, r1: 50, life: 0.35, max: 0.35, color: '#c9a7ff', width: 3 });
        this.parts.push({ type: 'ring', x: v, y: w, r0: 50, r1: 8, life: 0.35, max: 0.35, color: '#c9a7ff', width: 3 });
        for (let k = 0; k < 12; k++) { const f = Math.random(); this.parts.push({ type: 'dot', x: x + (v - x) * f, y: y + (w - y) * f, vx: rnd(-20, 20), vy: rnd(-20, 20), life: 0.5, max: 0.5, size: rnd(2, 4), color: '#d9c4ff' }); }
      } else if (t === ev.level) {
        this.parts.push({ type: 'ring', x, y, r0: 12, r1: 90, life: 0.8, max: 0.8, color: '#ffd96a', width: 4 });
        for (let k = 0; k < 14; k++) this.parts.push({ type: 'dot', x, y, vx: rnd(-40, 40), vy: rnd(-150, -50), life: 0.9, max: 0.9, size: rnd(2, 3.5), color: '#ffe48a' });
        if (a === me) this.floaters.push({ x, y: y - 36, text: 'LEVEL UP!', color: '#ffd96a', size: 17, life: 1.4, max: 1.4, vy: 30 });
      } else if (t === ev.respawn) {
        this.parts.push({ type: 'ring', x, y, r0: 80, r1: 8, life: 0.7, max: 0.7, color: '#9bffc0', width: 4 });
      } else if (t === ev.gold) {
        if (a === me && v >= 1) this.floaters.push({ x, y: y - 10, text: `+${Math.round(v)}`, color: '#ffd24a', size: 13, life: 1.1, max: 1.1, vy: 36 });
      } else if (t === ev.gameover) {
        this.shake = 22;
      }
    }
  }

  ping(x, y, kind) {
    this.pings.push({ x, y, life: 0.5, max: 0.5, color: kind === 'attack' ? '#ff5d52' : kind === 'cast' ? '#9bd0ff' : '#7dff9a' });
  }

  update(dt) {
    for (const p of this.parts) {
      p.life -= dt;
      if (p.type === 'dot') { p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 1 - 2.2 * dt; p.vy *= 1 - 2.2 * dt; }
    }
    this.parts = this.parts.filter((p) => p.life > 0);
    for (const f of this.floaters) { f.life -= dt; f.y -= f.vy * dt; }
    this.floaters = this.floaters.filter((f) => f.life > 0);
    for (const p of this.pings) p.life -= dt;
    this.pings = this.pings.filter((p) => p.life > 0);
    this.shake = Math.max(0, this.shake - 40 * dt);
    this.flash = Math.max(0, this.flash - 0.9 * dt);
    if (this.parts.length > 900) this.parts.splice(0, this.parts.length - 900);
  }

  // ---- main draw
  draw(view, ui) {
    const { ctx, W, H, cam, dpr, info } = this;
    const { U } = info, us = info.unit_fields.length;
    const f = view.cur, t = view.time;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = '#05070a';
    ctx.fillRect(0, 0, W, H);

    const sh = this.shake, ox = sh ? rnd(-sh, sh) * 0.5 : 0, oy = sh ? rnd(-sh, sh) * 0.5 : 0;
    ctx.save();
    ctx.translate(W / 2 - cam.x * cam.zoom + ox, H / 2 - cam.y * cam.zoom + oy);
    ctx.scale(cam.zoom, cam.zoom);
    ctx.drawImage(this.bg, 0, 0, info.world, info.world);
    this.drawBaseMarkers(ctx, f, t);

    // draw order: by y
    const order = [];
    for (let i = 0; i < f.nUnits; i++) order.push(i);
    order.sort((a, b) => view.py[a] - view.py[b]);

    this.drawDangerRings(ctx, view);
    this.drawZones(ctx, f);
    for (const i of order) this.drawUnit(ctx, view, i, ui, t);
    this.drawProjectiles(ctx, f);
    this.drawParticles(ctx);
    ctx.restore();

    this.drawFog(view);

    // screen-space overlays
    for (const i of order) this.drawUnitBars(ctx, view, i, ui);
    this.drawFloaters(ctx);
    if (this.flash > 0.01) {
      const g = ctx.createRadialGradient(W / 2, H / 2, Math.min(W, H) * 0.3, W / 2, H / 2, Math.max(W, H) * 0.75);
      g.addColorStop(0, 'rgba(255,0,0,0)');
      g.addColorStop(1, `rgba(255,30,20,${this.flash})`);
      ctx.fillStyle = g;
      ctx.fillRect(0, 0, W, H);
    }
    ctx.save();
    ctx.translate(W / 2 - cam.x * cam.zoom, H / 2 - cam.y * cam.zoom);
    ctx.scale(cam.zoom, cam.zoom);
    this.drawTargeting(ctx, view, ui);
    for (const p of this.pings) {
      const k = 1 - p.life / p.max;
      ctx.strokeStyle = p.color; ctx.globalAlpha = 1 - k; ctx.lineWidth = 3 / cam.zoom;
      ctx.beginPath(); ctx.arc(p.x, p.y, 26 * (1 - k * 0.7), 0, TAU); ctx.stroke();
      ctx.globalAlpha = 1;
    }
    ctx.restore();
    this.drawMinimap(view);
  }

  drawBaseMarkers(ctx, f, t) {
    const info = this.info, team = f.playerTeam, [bx, by] = info.bases[team];
    ctx.save();
    ctx.setLineDash([14, 12]);
    ctx.lineDashOffset = -t * 12;
    ctx.strokeStyle = rgba(TEAM[team].rgb, 0.4);
    ctx.lineWidth = 2.5;
    ctx.beginPath(); ctx.arc(bx, by, info.shop_radius, 0, TAU); ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = rgba(TEAM[team].rgb, 0.75);
    ctx.font = '700 15px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('FOUNTAIN · SHOP', bx, by + info.shop_radius + 20);
    ctx.restore();
  }

  drawDangerRings(ctx, view) {
    const f = view.cur, { U } = this.info, us = this.info.unit_fields.length, kind = this.info.kind;
    const me = f.index.get(f.player);
    if (me === undefined) return;
    const mx = view.px[me], my = view.py[me];
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us;
      if (f.units[o + U.kind] !== kind.tower || f.units[o + U.team] === f.playerTeam) continue;
      const d = Math.hypot(view.px[i] - mx, view.py[i] - my), r = f.units[o + U.range] + f.units[o + U.radius];
      if (d < r + 320) {
        const a = clamp(1 - (d - r) / 320, 0.12, 0.6);
        ctx.strokeStyle = `rgba(255,70,60,${a})`;
        ctx.fillStyle = `rgba(255,50,40,${a * 0.12})`;
        ctx.lineWidth = 3;
        ctx.beginPath(); ctx.arc(view.px[i], view.py[i], r, 0, TAU); ctx.fill(); ctx.stroke();
      }
    }
  }

  drawZones(ctx, f) {
    const { Z } = this.info, zs = this.info.zone_fields.length;
    for (let i = 0; i < f.zones.length; i += zs) {
      const z = f.zones, x = z[i + Z.x], y = z[i + Z.y], r = z[i + Z.radius], prog = 1 - z[i + Z.t_left] / Math.max(0.01, z[i + Z.t_total]);
      const col = hexRgb(abilityColor(this.info, z[i + Z.ability]));
      ctx.fillStyle = rgba(col, 0.08 + 0.24 * prog);
      ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.fill();
      ctx.strokeStyle = rgba(col, 0.85);
      ctx.lineWidth = 3 / this.cam.zoom;
      ctx.setLineDash([12, 8]);
      ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.stroke();
      ctx.setLineDash([]);
      ctx.strokeStyle = rgba(col, 0.9);
      ctx.beginPath(); ctx.arc(x, y, r * prog, 0, TAU); ctx.stroke();
    }
  }

  drawProjectiles(ctx, f) {
    const { P } = this.info, ps = this.info.proj_fields.length;
    for (let i = 0; i < f.projs.length; i += ps) {
      const p = f.projs, x = p[i + P.x], y = p[i + P.y], dx = p[i + P.dx], dy = p[i + P.dy], ab = p[i + P.ability], team = p[i + P.team];
      const attack = ab === 255;
      const col = attack ? TEAM[team]?.light || '#fff' : abilityColor(this.info, ab);
      const rad = attack ? 4 : p[i + P.homing] ? 7 : Math.max(8, p[i + P.radius] * 0.5);
      const len = attack ? 16 : p[i + P.homing] ? 22 : 44;
      const g = ctx.createLinearGradient(x, y, x - dx * len, y - dy * len);
      g.addColorStop(0, col);
      g.addColorStop(1, 'rgba(255,255,255,0)');
      ctx.strokeStyle = g;
      ctx.lineWidth = rad * 1.1;
      ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x - dx * len, y - dy * len); ctx.stroke();
      ctx.fillStyle = '#fff';
      ctx.shadowColor = col;
      ctx.shadowBlur = 10;
      ctx.beginPath(); ctx.arc(x, y, rad * 0.65, 0, TAU); ctx.fill();
      ctx.shadowBlur = 0;
    }
  }

  drawParticles(ctx) {
    for (const p of this.parts) {
      const k = clamp(p.life / p.max, 0, 1);
      if (p.type === 'dot') {
        ctx.globalAlpha = k;
        ctx.fillStyle = p.color;
        ctx.beginPath(); ctx.arc(p.x, p.y, p.size * (0.4 + k * 0.6), 0, TAU); ctx.fill();
      } else if (p.type === 'ring') {
        ctx.globalAlpha = k;
        ctx.strokeStyle = p.color;
        ctx.lineWidth = (p.width || 3) * k + 0.5;
        ctx.beginPath(); ctx.arc(p.x, p.y, p.r0 + (p.r1 - p.r0) * (1 - k), 0, TAU); ctx.stroke();
      } else if (p.type === 'disc') {
        ctx.globalAlpha = k * 0.35;
        ctx.fillStyle = p.color;
        ctx.beginPath(); ctx.arc(p.x, p.y, p.r1, 0, TAU); ctx.fill();
      }
    }
    ctx.globalAlpha = 1;
  }

  // ---- units
  drawUnit(ctx, view, i, ui, t) {
    const f = view.cur, { U, kind } = this.info, o = i * this.info.unit_fields.length, u = f.units;
    const k = u[o + U.kind], team = u[o + U.team], x = view.px[i], y = view.py[i], r = u[o + U.radius];
    const vis = u[o + U.visible] > 0, id = u[o + U.id], facing = u[o + U.facing], def = u[o + U.def];
    const isMe = id === f.player;
    ctx.globalAlpha = vis ? 1 : 0.5;

    // melee lunge while winding up an attack
    let lx = 0, ly = 0;
    const sw = u[o + U.swing_t], sl = u[o + U.swing_len];
    if (sw > 0 && sl > 0 && u[o + U.range] < 150) {
      const l = Math.sin((1 - sw / sl) * Math.PI) * 7;
      lx = Math.cos(facing) * l; ly = Math.sin(facing) * l;
    }
    // hover / selection ring at the feet
    if (vis && (view.hover === i || ui.selectedId === id)) {
      const enemy = team !== f.playerTeam;
      ctx.strokeStyle = team > 1 ? '#ffe9a8' : enemy ? '#ff6b5e' : '#7dff9a';
      ctx.lineWidth = 2.5 / this.cam.zoom;
      ctx.beginPath(); ctx.ellipse(x, y + r * 0.45, r * 1.5, r * 0.8, 0, 0, TAU); ctx.stroke();
    }
    ctx.save();
    ctx.translate(x + lx, y + ly);
    if (k === kind.hero) this.paintHero(ctx, def, r, team, facing, isMe, t);
    else if (k === kind.tower) this.paintTower(ctx, r, team, def, vis, t);
    else if (k === kind.ancient) this.paintAncient(ctx, r, team, t);
    else if (k === kind.melee) this.paintCreep(ctx, r, team, facing, 'melee');
    else if (k === kind.ranged) this.paintCreep(ctx, r, team, facing, 'ranged');
    else if (k === kind.siege) this.paintCreep(ctx, r, team, facing, 'siege');
    else this.paintNeutral(ctx, r, def, facing);
    this.paintStatus(ctx, u[o + U.status], r, t);
    ctx.restore();
    ctx.globalAlpha = 1;
  }

  paintHero(ctx, def, r, team, facing, isMe, t) {
    const c = TEAM[team] || TEAM[0], R = r * 1.25;
    ctx.fillStyle = 'rgba(0,0,0,.38)';
    ctx.beginPath(); ctx.ellipse(0, R * 0.7, R, R * 0.45, 0, 0, TAU); ctx.fill();
    if (isMe) {
      ctx.strokeStyle = `rgba(255,222,120,${0.65 + 0.3 * Math.sin(t * 5)})`;
      ctx.lineWidth = 3;
      ctx.beginPath(); ctx.arc(0, 0, R * 1.42, 0, TAU); ctx.stroke();
    }
    ctx.fillStyle = c.main;
    ctx.beginPath(); ctx.arc(0, 0, R * 1.14, 0, TAU); ctx.fill();
    const g = ctx.createRadialGradient(-R * 0.3, -R * 0.35, R * 0.1, 0, 0, R);
    g.addColorStop(0, c.light); g.addColorStop(0.45, c.main); g.addColorStop(1, c.dark);
    ctx.fillStyle = g;
    ctx.beginPath(); ctx.arc(0, 0, R, 0, TAU); ctx.fill();
    drawHeroGlyph(ctx, def, 0, 0, R * 0.62);
    // facing notch
    ctx.fillStyle = '#fff';
    ctx.beginPath();
    ctx.moveTo(Math.cos(facing) * R * 1.45, Math.sin(facing) * R * 1.45);
    ctx.lineTo(Math.cos(facing + 2.6) * R * 1.05, Math.sin(facing + 2.6) * R * 1.05);
    ctx.lineTo(Math.cos(facing - 2.6) * R * 1.05, Math.sin(facing - 2.6) * R * 1.05);
    ctx.closePath(); ctx.globalAlpha *= 0.85; ctx.fill(); ctx.globalAlpha /= 0.85;
  }

  paintCreep(ctx, r, team, facing, type) {
    const c = TEAM[team] || TEAM[0], R = r * 1.2;
    ctx.fillStyle = 'rgba(0,0,0,.3)';
    ctx.beginPath(); ctx.ellipse(0, R * 0.7, R, R * 0.4, 0, 0, TAU); ctx.fill();
    ctx.lineWidth = 1.6;
    ctx.strokeStyle = 'rgba(0,0,0,.65)';
    ctx.fillStyle = c.main;
    if (type === 'melee') {
      ctx.beginPath(); ctx.arc(0, 0, R, 0, TAU); ctx.fill(); ctx.stroke();
      ctx.strokeStyle = c.light; ctx.lineWidth = 2.2;
      ctx.beginPath(); ctx.moveTo(Math.cos(facing) * R * 0.2, Math.sin(facing) * R * 0.2); ctx.lineTo(Math.cos(facing) * R * 1.5, Math.sin(facing) * R * 1.5); ctx.stroke();
    } else if (type === 'ranged') {
      ctx.beginPath();
      for (let k = 0; k < 3; k++) { const a = facing + (k * TAU) / 3; ctx.lineTo(Math.cos(a) * R * 1.25, Math.sin(a) * R * 1.25); }
      ctx.closePath(); ctx.fill(); ctx.stroke();
      ctx.fillStyle = c.light; ctx.beginPath(); ctx.arc(0, 0, R * 0.28, 0, TAU); ctx.fill();
    } else {
      ctx.save(); ctx.rotate(facing);
      ctx.fillRect(-R * 1.1, -R * 0.8, R * 2.2, R * 1.6); ctx.strokeRect(-R * 1.1, -R * 0.8, R * 2.2, R * 1.6);
      ctx.fillStyle = '#222';
      for (const sy of [-1, 1]) { ctx.beginPath(); ctx.arc(-R * 0.6, sy * R * 0.85, R * 0.3, 0, TAU); ctx.arc(R * 0.6, sy * R * 0.85, R * 0.3, 0, TAU); ctx.fill(); }
      ctx.fillStyle = c.light; ctx.fillRect(R * 0.3, -R * 0.2, R * 1.2, R * 0.4);
      ctx.restore();
    }
  }

  paintNeutral(ctx, r, def, facing) {
    const R = r * 1.2, col = NEUTRAL_COLORS[def % NEUTRAL_COLORS.length];
    ctx.fillStyle = 'rgba(0,0,0,.3)';
    ctx.beginPath(); ctx.ellipse(0, R * 0.7, R, R * 0.4, 0, 0, TAU); ctx.fill();
    ctx.fillStyle = col; ctx.strokeStyle = 'rgba(0,0,0,.7)'; ctx.lineWidth = 1.8;
    ctx.beginPath();
    for (let k = 0; k < 8; k++) { const a = (k * TAU) / 8 + 0.4, rr = R * (k % 2 ? 0.85 : 1.1); ctx.lineTo(Math.cos(a) * rr, Math.sin(a) * rr); }
    ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.fillStyle = '#2a1608';
    for (const s of [-1, 1]) { ctx.beginPath(); ctx.arc(Math.cos(facing + s * 0.5) * R * 0.5, Math.sin(facing + s * 0.5) * R * 0.5, R * 0.15, 0, TAU); ctx.fill(); }
  }

  paintTower(ctx, r, team, tier, vis, t) {
    const c = TEAM[team] || TEAM[0], R = r * 1.1;
    ctx.fillStyle = 'rgba(0,0,0,.4)';
    ctx.beginPath(); ctx.ellipse(0, R * 0.75, R * 1.15, R * 0.5, 0, 0, TAU); ctx.fill();
    const g = ctx.createLinearGradient(-R, -R, R, R);
    g.addColorStop(0, '#8d8f95'); g.addColorStop(1, '#3b3d44');
    ctx.fillStyle = g; ctx.strokeStyle = '#15161a'; ctx.lineWidth = 2.5;
    ctx.beginPath();
    for (let k = 0; k < 6; k++) { const a = (k * TAU) / 6 + TAU / 12; ctx.lineTo(Math.cos(a) * R, Math.sin(a) * R); }
    ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.strokeStyle = c.main; ctx.lineWidth = 3.5;
    ctx.beginPath(); ctx.arc(0, 0, R * 0.78, 0, TAU); ctx.stroke();
    const pulse = vis ? 0.75 + 0.25 * Math.sin(t * 3 + tier) : 0.6;
    const gg = ctx.createRadialGradient(0, 0, 1, 0, 0, R * 0.55);
    gg.addColorStop(0, '#fff'); gg.addColorStop(0.35, c.light); gg.addColorStop(1, c.dark);
    ctx.fillStyle = gg; ctx.globalAlpha *= pulse;
    ctx.beginPath(); ctx.arc(0, 0, R * 0.5, 0, TAU); ctx.fill();
    ctx.globalAlpha /= pulse;
    ctx.fillStyle = '#fff';
    for (let k = 0; k <= tier; k++) { const a = -Math.PI / 2 + (k - tier / 2) * 0.55; ctx.beginPath(); ctx.arc(Math.cos(a) * R * 0.78, Math.sin(a) * R * 0.78, 2.6, 0, TAU); ctx.fill(); }
  }

  paintAncient(ctx, r, team, t) {
    const c = TEAM[team] || TEAM[0];
    ctx.fillStyle = 'rgba(0,0,0,.4)';
    ctx.beginPath(); ctx.ellipse(0, r * 0.75, r * 1.25, r * 0.55, 0, 0, TAU); ctx.fill();
    const g = ctx.createLinearGradient(-r, -r, r, r);
    g.addColorStop(0, '#9a9ca3'); g.addColorStop(1, '#2f3138');
    ctx.fillStyle = g; ctx.strokeStyle = '#111'; ctx.lineWidth = 3;
    ctx.beginPath();
    for (let k = 0; k < 8; k++) { const a = (k * TAU) / 8 + TAU / 16; ctx.lineTo(Math.cos(a) * r * 1.2, Math.sin(a) * r * 1.2); }
    ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.setLineDash([10, 8]); ctx.lineDashOffset = -t * 20;
    ctx.strokeStyle = c.main; ctx.lineWidth = 3;
    ctx.beginPath(); ctx.arc(0, 0, r * 0.95, 0, TAU); ctx.stroke();
    ctx.setLineDash([]);
    const pulse = 0.8 + 0.2 * Math.sin(t * 2.5);
    ctx.shadowColor = c.main; ctx.shadowBlur = 24 * pulse;
    const cg = ctx.createLinearGradient(0, -r * 0.8, 0, r * 0.8);
    cg.addColorStop(0, '#fff'); cg.addColorStop(0.4, c.light); cg.addColorStop(1, c.dark);
    ctx.fillStyle = cg;
    ctx.beginPath(); ctx.moveTo(0, -r * 0.82 * pulse); ctx.lineTo(r * 0.45, 0); ctx.lineTo(0, r * 0.82 * pulse); ctx.lineTo(-r * 0.45, 0); ctx.closePath(); ctx.fill();
    ctx.shadowBlur = 0;
  }

  paintStatus(ctx, s, r, t) {
    const st = this.info.status;
    if (!s) return;
    if (s & st.stun) {
      ctx.fillStyle = '#ffe14a';
      for (let k = 0; k < 3; k++) {
        const a = t * 4 + (k * TAU) / 3;
        const x = Math.cos(a) * r * 0.9, y = -r * 1.7 + Math.sin(a) * r * 0.3;
        ctx.beginPath();
        for (let j = 0; j < 10; j++) { const rr = j % 2 ? 2 : 5, aa = (j * Math.PI) / 5 - Math.PI / 2; ctx.lineTo(x + Math.cos(aa) * rr, y + Math.sin(aa) * rr); }
        ctx.closePath(); ctx.fill();
      }
    }
    if (s & st.root) {
      ctx.strokeStyle = '#58d84a'; ctx.lineWidth = 3; ctx.setLineDash([5, 4]);
      ctx.beginPath(); ctx.arc(0, r * 0.3, r * 1.5, 0, TAU); ctx.stroke(); ctx.setLineDash([]);
    }
    if (s & st.slow) {
      ctx.strokeStyle = 'rgba(130,200,255,.9)'; ctx.fillStyle = 'rgba(130,200,255,.18)'; ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(0, r * 0.3, r * 1.5, 0, TAU); ctx.fill(); ctx.stroke();
    }
    if (s & (st.haste | st.aspd)) {
      ctx.strokeStyle = (s & st.aspd) ? 'rgba(255,190,90,.9)' : 'rgba(255,255,255,.8)'; ctx.lineWidth = 2;
      for (let k = 0; k < 3; k++) { ctx.beginPath(); ctx.moveTo(-r * 1.6, -r * 0.5 + k * r * 0.5); ctx.lineTo(-r * 2.4 - Math.sin(t * 20 + k) * 3, -r * 0.5 + k * r * 0.5); ctx.stroke(); }
    }
    if (s & st.salve) {
      ctx.fillStyle = '#7dff9a';
      for (let k = 0; k < 2; k++) { const a = t * 3 + k * 3; ctx.fillRect(Math.cos(a) * r - 2, -((t * 24 + k * 14) % 30) - r * 0.5, 4, 4); }
    }
  }

  // ---- screen-space bars (drawn after fog so they never get darkened)
  drawUnitBars(ctx, view, i, ui) {
    const f = view.cur, { U, kind } = this.info, o = i * this.info.unit_fields.length, u = f.units, z = this.cam.zoom;
    if (u[o + U.visible] <= 0) return;
    const k = u[o + U.kind], hp = u[o + U.hp], mhp = u[o + U.max_hp], team = u[o + U.team], r = u[o + U.radius];
    const hero = k === kind.hero, bld = k === kind.tower || k === kind.ancient;
    if (!hero && !bld && hp >= mhp - 0.5 && view.hover !== i) return; // creeps: only once damaged
    const h = hero ? 7 : bld ? 7 : 4;
    const [sx, top] = this.barAnchor(view, i, r, hero, bld);
    if (sx < -60 || sx > this.W + 60 || top < -60 || top > this.H + 60) return;
    const w = clamp(r * 2.6 * z, hero ? 46 : 26, bld ? 96 : 70);
    const x = sx - w / 2, y = top - h - (hero ? 8 : 0);
    ctx.fillStyle = 'rgba(0,0,0,.75)';
    ctx.fillRect(x - 1, y - 1, w + 2, h + 2 + (hero ? 4 : 0));
    const pct = clamp(hp / mhp, 0, 1);
    ctx.fillStyle = team === f.playerTeam ? (pct > 0.3 ? '#3ed06a' : '#e0c33a') : team > 1 ? '#d9a441' : '#e0453c';
    ctx.fillRect(x, y, w * pct, h);
    if (hero) {
      const mm = u[o + U.max_mana];
      if (mm > 0) { ctx.fillStyle = '#2b5fe0'; ctx.fillRect(x, y + h + 1, w * clamp(u[o + U.mana] / mm, 0, 1), 3); }
      const lvl = u[o + U.level];
      ctx.fillStyle = '#10131a'; ctx.beginPath(); ctx.arc(x - 8, y + 4, 8, 0, TAU); ctx.fill();
      ctx.strokeStyle = '#e8c875'; ctx.lineWidth = 1.5; ctx.stroke();
      ctx.fillStyle = '#fff'; ctx.font = '700 10px system-ui, sans-serif'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText(String(lvl), x - 8, y + 4.5);
    }
  }

  /** Screen x and the y just above the unit's head, where its bars sit. */
  barAnchor(view, i, r, hero, bld) {
    const [sx, sy] = this.w2s(view.px[i], view.py[i]);
    return [sx, sy - (r * (hero ? 1.25 : 1.2) + (bld ? 22 : 12)) * this.cam.zoom];
  }

  drawFloaters(ctx) {
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (const fl of this.floaters) {
      const [sx, sy] = this.w2s(fl.x, fl.y, 40); // height is only used by the 3D view
      const k = fl.life / fl.max;
      ctx.globalAlpha = clamp(k * 2, 0, 1);
      ctx.font = `800 ${Math.round(fl.size * (0.9 + 0.25 * (1 - k)))}px system-ui, sans-serif`;
      ctx.lineWidth = 3;
      ctx.strokeStyle = 'rgba(0,0,0,.85)';
      ctx.strokeText(fl.text, sx, sy);
      ctx.fillStyle = fl.color;
      ctx.fillText(fl.text, sx, sy);
    }
    ctx.globalAlpha = 1;
  }

  // ---- fog of war
  /**
   * Darken what the player's team can't see. The fog is painted at FOG_SCALE of the screen size and
   * stretched over it: it is all soft edges, and at full resolution this was most of the frame time.
   */
  drawFog(view) {
    const f = view.cur;
    if (!f.g[this.info.G.fog]) return;
    const fc = this.fogCanvas, fx = fc.getContext('2d'), { U } = this.info, us = this.info.unit_fields.length, s = FOG_SCALE, z = this.cam.zoom;
    fx.setTransform(1, 0, 0, 1, 0, 0);
    fx.globalCompositeOperation = 'source-over';
    fx.clearRect(0, 0, fc.width, fc.height);
    fx.fillStyle = 'rgba(3,6,12,0.8)';
    fx.fillRect(0, 0, fc.width, fc.height);
    fx.globalCompositeOperation = 'destination-out';
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us;
      if (f.units[o + U.team] !== f.playerTeam) continue;
      const [sx, sy] = this.w2s(view.px[i], view.py[i]);
      const r = f.units[o + U.vision] * z * s, cx = sx * s, cy = sy * s;
      if (cx + r < 0 || cy + r < 0 || cx - r > fc.width || cy - r > fc.height) continue;
      const g = fx.createRadialGradient(cx, cy, r * 0.8, cx, cy, r);
      g.addColorStop(0, 'rgba(0,0,0,1)');
      g.addColorStop(1, 'rgba(0,0,0,0)');
      fx.fillStyle = g;
      fx.beginPath(); fx.arc(cx, cy, r, 0, TAU); fx.fill();
    }
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.drawImage(fc, 0, 0, this.canvas.width, this.canvas.height);
    this.ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
  }

  // ---- ability / attack targeting previews
  drawTargeting(ctx, view, ui) {
    const f = view.cur, z = this.cam.zoom, me = f.index.get(f.player);
    if (me === undefined || !ui.mouseWorld) return;
    const hx = view.px[me], hy = view.py[me], [mx, my] = ui.mouseWorld;
    ctx.lineWidth = 2.5 / z;
    if (ui.mode === 'attackmove') {
      ctx.strokeStyle = '#ff6b5e';
      ctx.beginPath(); ctx.arc(mx, my, 18, 0, TAU); ctx.moveTo(mx - 28, my); ctx.lineTo(mx + 28, my); ctx.moveTo(mx, my - 28); ctx.lineTo(mx, my + 28); ctx.stroke();
      return;
    }
    if (ui.mode !== 'cast' || !ui.cast) return;
    const c = ui.cast, col = c.color || '#ffffff', rgb = hexRgb(col.length === 7 ? col : '#ffffff');
    ctx.strokeStyle = 'rgba(255,255,255,.55)';
    ctx.setLineDash([10, 8]);
    if (c.range > 0) { ctx.beginPath(); ctx.arc(hx, hy, c.range, 0, TAU); ctx.stroke(); }
    ctx.setLineDash([]);
    if (c.kind === 'target') {
      if (view.hover >= 0) {
        const h = view.hover, hv = view.cur.units;
        const o = h * this.info.unit_fields.length;
        if (hv[o + this.info.U.team] !== f.playerTeam) {
          ctx.strokeStyle = col; ctx.lineWidth = 3 / z;
          ctx.beginPath(); ctx.arc(view.px[h], view.py[h], hv[o + this.info.U.radius] * 1.8, 0, TAU); ctx.stroke();
        }
      }
    } else if (c.kind === 'point') {
      ctx.fillStyle = rgba(rgb, 0.22); ctx.strokeStyle = col;
      ctx.beginPath(); ctx.arc(mx, my, c.radius, 0, TAU); ctx.fill(); ctx.stroke();
    } else if (c.kind === 'line' || c.kind === 'blink') {
      let dx = mx - hx, dy = my - hy;
      const d = Math.hypot(dx, dy) || 1;
      dx /= d; dy /= d;
      const len = Math.min(d, c.range || d);
      ctx.fillStyle = rgba(rgb, 0.2); ctx.strokeStyle = col;
      if (c.kind === 'line') {
        const hw = Math.max(10, c.radius);
        ctx.beginPath();
        ctx.moveTo(hx - dy * hw, hy + dx * hw); ctx.lineTo(hx + dx * len - dy * hw, hy + dy * len + dx * hw);
        ctx.lineTo(hx + dx * len + dy * hw, hy + dy * len - dx * hw); ctx.lineTo(hx + dy * hw, hy - dx * hw);
        ctx.closePath(); ctx.fill(); ctx.stroke();
      } else {
        ctx.beginPath(); ctx.moveTo(hx, hy); ctx.lineTo(hx + dx * len, hy + dy * len); ctx.stroke();
        ctx.beginPath(); ctx.arc(hx + dx * len, hy + dy * len, 20, 0, TAU); ctx.fill(); ctx.stroke();
        if (c.radius > 0) { ctx.beginPath(); ctx.arc(hx + dx * len, hy + dy * len, c.radius, 0, TAU); ctx.stroke(); }
      }
    }
  }

  // ---- minimap
  drawMinimap(view) {
    const m = this.mctx, S = this.mini.width, sc = S / this.info.world, f = view.cur;
    const { U, kind } = this.info, us = this.info.unit_fields.length;
    m.setTransform(1, 0, 0, 1, 0, 0);
    m.clearRect(0, 0, S, S);
    m.drawImage(this.bg, 0, 0, S, S);
    if (f.g[this.info.G.fog]) {
      const fx = this.mfog.getContext('2d');
      fx.globalCompositeOperation = 'source-over';
      fx.clearRect(0, 0, S, S);
      fx.fillStyle = 'rgba(3,6,12,.68)';
      fx.fillRect(0, 0, S, S);
      fx.globalCompositeOperation = 'destination-out';
      for (let i = 0; i < f.nUnits; i++) {
        const o = i * us;
        if (f.units[o + U.team] !== f.playerTeam) continue;
        fx.beginPath(); fx.arc(view.px[i] * sc, view.py[i] * sc, f.units[o + U.vision] * sc, 0, TAU); fx.fill();
      }
      m.drawImage(this.mfog, 0, 0);
    }
    for (let i = 0; i < f.nUnits; i++) {
      const o = i * us, k = f.units[o + U.kind], team = f.units[o + U.team], x = view.px[i] * sc, y = view.py[i] * sc;
      const vis = f.units[o + U.visible] > 0;
      if (!vis && !(k === kind.tower || k === kind.ancient)) continue;
      m.globalAlpha = vis ? 1 : 0.55;
      m.fillStyle = team > 1 ? '#c9a45f' : TEAM[team].main;
      if (k === kind.tower) { const s = S * 0.022; m.fillRect(x - s, y - s, s * 2, s * 2); }
      else if (k === kind.ancient) { const s = S * 0.04; m.fillRect(x - s, y - s, s * 2, s * 2); m.strokeStyle = '#fff'; m.lineWidth = 1.5; m.strokeRect(x - s, y - s, s * 2, s * 2); }
      else if (k === kind.hero) { m.beginPath(); m.arc(x, y, S * 0.026, 0, TAU); m.fill(); m.strokeStyle = f.units[o + U.id] === f.player ? '#fff' : '#000'; m.lineWidth = 1.5; m.stroke(); }
      else if (team <= 1) { m.beginPath(); m.arc(x, y, S * 0.009, 0, TAU); m.fill(); }
      else { m.fillRect(x - 1.5, y - 1.5, 3, 3); }
    }
    m.globalAlpha = 1;
    this.drawViewport(m, sc);
  }

  /** The camera's footprint on the minimap. */
  drawViewport(m, sc) {
    const [x0, y0] = this.s2w(0, 0), [x1, y1] = this.s2w(this.W, this.H);
    m.strokeStyle = 'rgba(255,255,255,.9)';
    m.lineWidth = 1.5;
    m.strokeRect(x0 * sc, y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc);
  }

  /** Release anything the renderer owns (the 3D view holds GPU resources). */
  dispose() {}
}
