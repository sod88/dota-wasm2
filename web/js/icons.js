// Procedural icons: nothing to download, nothing that depends on installed emoji fonts.
// Ability icons are derived from the ability's *kind* (cast type + status effect), so a new hero
// added in data.rs gets sensible icons automatically.

const PALETTE = ['#e8b04a', '#6cc4ff', '#9be36b', '#d68cff', '#ff7a5c', '#4fe0c8', '#ffd966', '#ff8fb1'];

export function hashStr(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function tile(size, base) {
  const c = document.createElement('canvas');
  c.width = c.height = size;
  const ctx = c.getContext('2d');
  const g = ctx.createLinearGradient(0, 0, size, size);
  g.addColorStop(0, base);
  g.addColorStop(1, '#10131a');
  roundRect(ctx, 1.5, 1.5, size - 3, size - 3, size * 0.16);
  ctx.fillStyle = g;
  ctx.fill();
  ctx.lineWidth = 2;
  ctx.strokeStyle = 'rgba(255,225,160,.55)';
  ctx.stroke();
  return [c, ctx];
}

// ---- ability glyphs (drawn in a 0..1 box scaled to the tile)
const GLYPH = {
  hammer(ctx, s) {
    ctx.lineWidth = s * 0.07;
    ctx.beginPath(); ctx.moveTo(s * 0.32, s * 0.78); ctx.lineTo(s * 0.62, s * 0.34); ctx.stroke();
    ctx.save(); ctx.translate(s * 0.62, s * 0.3); ctx.rotate(0.55); ctx.fillRect(-s * 0.2, -s * 0.1, s * 0.4, s * 0.2); ctx.restore();
  },
  net(ctx, s) {
    ctx.lineWidth = s * 0.04;
    for (let i = 0; i < 5; i++) {
      const p = s * (0.22 + i * 0.14);
      ctx.beginPath(); ctx.moveTo(p, s * 0.2); ctx.lineTo(p, s * 0.8); ctx.moveTo(s * 0.2, p); ctx.lineTo(s * 0.8, p); ctx.stroke();
    }
    ctx.beginPath(); ctx.arc(s / 2, s / 2, s * 0.3, 0, 7); ctx.stroke();
  },
  flame(ctx, s) {
    ctx.beginPath();
    ctx.moveTo(s * 0.5, s * 0.12);
    ctx.bezierCurveTo(s * 0.9, s * 0.45, s * 0.78, s * 0.86, s * 0.5, s * 0.86);
    ctx.bezierCurveTo(s * 0.2, s * 0.86, s * 0.14, s * 0.5, s * 0.4, s * 0.38);
    ctx.bezierCurveTo(s * 0.42, s * 0.5, s * 0.5, s * 0.5, s * 0.5, s * 0.12);
    ctx.fill();
  },
  arrow(ctx, s) {
    ctx.lineWidth = s * 0.07;
    ctx.beginPath(); ctx.moveTo(s * 0.15, s * 0.85); ctx.lineTo(s * 0.78, s * 0.22); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(s * 0.86, s * 0.14); ctx.lineTo(s * 0.52, s * 0.24); ctx.lineTo(s * 0.76, s * 0.48); ctx.closePath(); ctx.fill();
  },
  rain(ctx, s) {
    ctx.lineWidth = s * 0.05;
    for (const [x, y] of [[0.3, 0.3], [0.52, 0.2], [0.72, 0.38]]) {
      ctx.beginPath(); ctx.moveTo(s * x, s * y); ctx.lineTo(s * (x - 0.08), s * (y + 0.34)); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(s * (x - 0.1), s * (y + 0.38)); ctx.lineTo(s * (x - 0.14), s * (y + 0.26)); ctx.lineTo(s * (x - 0.02), s * (y + 0.28)); ctx.fill();
    }
    ctx.beginPath(); ctx.ellipse(s / 2, s * 0.82, s * 0.32, s * 0.08, 0, 0, 7); ctx.stroke();
  },
  snow(ctx, s) {
    ctx.lineWidth = s * 0.06;
    for (let i = 0; i < 3; i++) {
      const a = (i * Math.PI) / 3;
      const dx = Math.cos(a) * s * 0.32, dy = Math.sin(a) * s * 0.32;
      ctx.beginPath(); ctx.moveTo(s / 2 - dx, s / 2 - dy); ctx.lineTo(s / 2 + dx, s / 2 + dy); ctx.stroke();
    }
    ctx.beginPath(); ctx.arc(s / 2, s / 2, s * 0.12, 0, 7); ctx.fill();
  },
  meteor(ctx, s) {
    ctx.beginPath(); ctx.arc(s * 0.62, s * 0.58, s * 0.2, 0, 7); ctx.fill();
    ctx.lineWidth = s * 0.07;
    ctx.beginPath(); ctx.moveTo(s * 0.5, s * 0.46); ctx.lineTo(s * 0.14, s * 0.12); ctx.moveTo(s * 0.46, s * 0.6); ctx.lineTo(s * 0.12, s * 0.38); ctx.moveTo(s * 0.6, s * 0.46); ctx.lineTo(s * 0.4, s * 0.1); ctx.stroke();
  },
  waves(ctx, s) {
    ctx.lineWidth = s * 0.06;
    for (const r of [0.14, 0.26, 0.38]) { ctx.beginPath(); ctx.arc(s / 2, s / 2, s * r, 0, 7); ctx.stroke(); }
  },
  chevrons(ctx, s) {
    ctx.lineWidth = s * 0.09;
    for (const y of [0.34, 0.58]) { ctx.beginPath(); ctx.moveTo(s * 0.25, s * (y + 0.16)); ctx.lineTo(s * 0.5, s * y); ctx.lineTo(s * 0.75, s * (y + 0.16)); ctx.stroke(); }
  },
  blink(ctx, s) {
    ctx.lineWidth = s * 0.07;
    ctx.setLineDash([s * 0.07, s * 0.07]);
    ctx.beginPath(); ctx.moveTo(s * 0.14, s * 0.8); ctx.lineTo(s * 0.5, s * 0.46); ctx.stroke();
    ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(s * 0.86, s * 0.14); ctx.lineTo(s * 0.5, s * 0.24); ctx.lineTo(s * 0.74, s * 0.5); ctx.closePath(); ctx.fill();
  },
  fist(ctx, s) {
    ctx.beginPath(); ctx.moveTo(s * 0.5, s * 0.14); ctx.lineTo(s * 0.62, s * 0.4); ctx.lineTo(s * 0.88, s * 0.42); ctx.lineTo(s * 0.68, s * 0.6); ctx.lineTo(s * 0.74, s * 0.86); ctx.lineTo(s * 0.5, s * 0.72); ctx.lineTo(s * 0.26, s * 0.86); ctx.lineTo(s * 0.32, s * 0.6); ctx.lineTo(s * 0.12, s * 0.42); ctx.lineTo(s * 0.38, s * 0.4); ctx.closePath(); ctx.fill();
  },
};

/** Names a mod may use for an ability's `icon`. */
export const ICON_NAMES = Object.keys(GLYPH);

function glyphFor(ab, cast, fx) {
  if (ab.icon && GLYPH[ab.icon]) return ab.icon; // chosen by a mod
  const stun = fx & 1, slow = fx & 2, root = fx & 4, haste = fx & 8, aspd = fx & 16;
  switch (ab.cast) {
    case cast.target: return stun ? 'hammer' : root ? 'net' : 'flame';
    case cast.line: return 'arrow';
    case cast.point: return slow ? 'snow' : stun ? 'meteor' : 'rain';
    case cast.self_aoe: return 'waves';
    case cast.self_buff: return aspd ? 'fist' : haste ? 'chevrons' : 'fist';
    case cast.blink: return ab.aoe_on_land ? 'meteor' : 'blink';
    default: return 'fist';
  }
}

const cache = new Map();

/** Icons are cached by index, so drop them whenever the content (mods) changes. */
export function clearIconCache() {
  cache.clear();
}

/** 48px canvas for ability `idx` (= hero * 4 + slot). */
export function abilityIcon(info, idx, size = 48) {
  const key = `a${idx}:${size}`;
  if (cache.has(key)) return cache.get(key);
  const ab = info.abilities[idx];
  const [c, ctx] = tile(size, ab.color || PALETTE[hashStr(ab.name) % PALETTE.length]);
  ctx.fillStyle = ctx.strokeStyle = 'rgba(255,255,255,.93)';
  ctx.lineCap = ctx.lineJoin = 'round';
  GLYPH[glyphFor(ab, info.cast, ab.fx)](ctx, size);
  cache.set(key, c);
  return c;
}

const STAT_COLORS = [
  ['str', '#c0504d'], ['agi', '#5aa65a'], ['int', '#4b7fd0'], ['dmg', '#d8873a'], ['armor', '#8a97a8'], ['hp', '#b8304a'],
  ['mana', '#3c6cc8'], ['aspd', '#d6c13c'], ['ms', '#3cc0c8'], ['hpreg', '#d070a0'], ['mpreg', '#6a74d8'],
];

export function itemInitials(name) {
  const w = name.replace(/'s\b/, '').split(/\s+/).filter(Boolean);
  return (w.length > 1 ? w[0][0] + w[1][0] : name.slice(0, 2)).toUpperCase();
}

/** Item icon: coloured by the item's strongest stat, labelled with initials. */
export function itemIcon(info, id, size = 44) {
  const key = `i${id}:${size}`;
  if (cache.has(key)) return cache.get(key);
  const it = info.items[id];
  let col = '#7a7f8a';
  if (it.color) col = it.color; // chosen by a mod
  else if (it.active === info.act.blink) col = '#9a5de0';
  else if (it.active === info.act.salve) col = '#58b86a';
  else if (it.active === info.act.mana) col = '#4a86e0';
  else {
    let best = 0;
    for (const [k, c] of STAT_COLORS) {
      const v = Math.abs(it[k] || 0) * ({ hp: 0.05, mana: 0.05, ms: 0.4, aspd: 0.4, hpreg: 2, mpreg: 2, dmg: 0.5, armor: 1.2 }[k] || 1);
      if (v > best) { best = v; col = c; }
    }
  }
  const [c, ctx] = tile(size, col);
  ctx.fillStyle = '#fff';
  ctx.font = `700 ${Math.round(size * 0.38)}px system-ui, sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.shadowColor = 'rgba(0,0,0,.7)';
  ctx.shadowBlur = 3;
  ctx.fillText(itemInitials(it.name), size / 2, size / 2 + 1);
  cache.set(key, c);
  return c;
}

/** Colour used for an ability's projectiles / ground effects. */
export function abilityColor(info, idx) {
  const ab = info.abilities[idx];
  if (!ab) return '#fff';
  if (ab.color) return ab.color;
  return PALETTE[hashStr(ab.name) % PALETTE.length];
}
