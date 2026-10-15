// Front end: boots the WebAssembly engine, runs the fixed-timestep loop, handles input and the HUD.
//
// The simulation lives entirely in Rust (src/). This file only
//   1. steps it at its native tick rate and interpolates between snapshots for smooth rendering,
//   2. turns mouse / keyboard input into engine commands, and
//   3. mirrors engine state into the DOM (bars, cooldowns, shop, scoreboard, kill feed).

import { Engine, ENGINES } from './engine.js';
import { Renderer, TEAM, drawHeroPortrait, setHeroStyles } from './render.js';
import { abilityIcon, itemIcon, abilityColor, clearIconCache } from './icons.js';

const $ = (id) => document.getElementById(id);
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const fmtTime = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const KEYS = ['Q', 'W', 'E', 'R'];
const SIZES = [1, 2, 3, 5];
const SPEEDS = [1, 2, 4];
const ATTR = ['STR', 'AGI', 'INT'];
const DMG_TYPE = ['Physical', 'Magical', 'Pure'];

// ------------------------------------------------------------------------------ state

const S = {
  engine: null,
  info: null,
  renderer: null,
  opts: { hero: 0, size: 1, fog: true, quick: false, watch: false, engine: 'rust', view3d: true },
  R3D: null, // the 3D renderer class once render3d.wasm and WebGPU are ready (null = not loaded / unavailable)
  mods: [], // { id, file, text, meta, custom, enabled, errors }
  built: {}, // which engine .wasm files exist
  running: false,
  paused: false,
  over: false,
  auto: false,
  speed: 1,
  acc: 0,
  last: 0,
  prev: null,
  cur: null,
  view: { cur: null, time: 0, px: new Float32Array(0), py: new Float32Array(0), hover: -1 },
  ui: { mode: null, cast: null, selectedId: 0, mouseWorld: null },
  mouse: { x: 0, y: 0, inCanvas: false, inWindow: false },
  keys: new Set(),
  lockCam: true,
  boardHeld: false,
  boardPinned: false,
  seen: new Map(), // unit id -> { kind, def, team }: lets the feed name units that already died
  heroPos: null,
  miniDrag: false,
};

const load = () => { try { return JSON.parse(localStorage.getItem('lanewars.opts') || '{}'); } catch { return {}; } };
const save = () => { try { localStorage.setItem('lanewars.opts', JSON.stringify(S.opts)); } catch { /* storage unavailable */ } };

/** Only touch the DOM when a value actually changed. */
function setText(el, v) { v = String(v); if (el.__t !== v) { el.__t = v; el.textContent = v; } }
function setHtml(el, v) { if (el.__h !== v) { el.__h = v; el.innerHTML = v; } }
function setStyle(el, prop, v) { if (el.__s?.[prop] !== v) { (el.__s ||= {})[prop] = v; el.style[prop] = v; } }
function setClass(el, name, on) { if (el.classList.contains(name) !== !!on) el.classList.toggle(name, !!on); }

function paintCanvas(canvas, src) {
  const c = canvas.getContext('2d');
  c.clearRect(0, 0, canvas.width, canvas.height);
  c.drawImage(src, 0, 0, canvas.width, canvas.height);
}

let toastTimer = 0;
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 1700);
}

// ------------------------------------------------------------------------------ frame accessors

/** Player's hero row helpers for the current snapshot. */
function me() {
  const f = S.cur, info = S.info;
  if (!f || !info) return null;
  const hr = f.heroIndex.get(f.player);
  if (hr === undefined) return null;
  const hs = info.hero_fields.length, H = info.H, o = hr * hs, h = f.heroes;
  const get = (name) => h[o + H[name]];
  const ur = f.index.get(f.player);
  const us = info.unit_fields.length, U = info.U, uo = (ur ?? 0) * us;
  const unit = (name) => (ur === undefined ? 0 : f.units[uo + U[name]]);
  return { get, unit, alive: get('alive') > 0, def: get('def'), hasUnit: ur !== undefined, unitIndex: ur };
}

function unitName(def, kind) {
  const info = S.info, K = info.kind;
  if (kind === K.hero) return info.heroes[def]?.name ?? 'Hero';
  if (kind === K.neutral) return info.neutral_names[def] ?? 'Neutral';
  if (kind === K.tower) return `Tier ${def + 1} Tower`;
  return info.kind_names[kind] ?? 'Unit';
}

function refreshSeen(f) {
  const { U } = S.info, us = S.info.unit_fields.length;
  for (let i = 0; i < f.nUnits; i++) {
    const o = i * us;
    S.seen.set(f.units[o + U.id], { kind: f.units[o + U.kind], def: f.units[o + U.def], team: f.units[o + U.team] });
  }
}

function label(id) {
  const s = S.seen.get(id);
  if (!s) return '<span class="g-c">something</span>';
  const cls = s.team === 0 ? 'r-c' : s.team === 1 ? 'd-c' : 'g-c';
  return `<span class="${cls}">${esc(unitName(s.def, s.kind))}</span>`;
}

// ------------------------------------------------------------------------------ boot + menu

async function boot() {
  Object.assign(S.opts, load());
  const q = new URLSearchParams(location.search);
  if (q.has('hero')) S.opts.hero = +q.get('hero') || 0;
  if (q.has('size')) S.opts.size = +q.get('size') || 1;
  if (q.has('fog')) S.opts.fog = q.get('fog') !== '0';
  if (q.has('watch')) S.opts.watch = q.get('watch') !== '0';
  if (q.has('quick')) S.opts.quick = q.get('quick') !== '0';
  if (q.has('view')) S.opts.view3d = q.get('view') === '3d';

  if (q.has('engine') && ENGINES[q.get('engine')]) S.opts.engine = q.get('engine');

  openOverlay('menu', true);
  bindModUi();
  try {
    if (typeof WebAssembly !== 'object') throw new Error('This browser has no WebAssembly support.');
    await probeEngines();
    S.engine = await loadEngine(S.opts.engine);
    await loadModList();
    applyMods();
    buildMenu();
    if (S.opts.view3d && !(await load3D())) S.opts.view3d = false; // no WebGPU or renderer: play in 2D
  } catch (e) {
    console.error(e);
    $('err').innerHTML =
      `<b>Could not start the engine.</b><br>${esc(e.message)}<br>` +
      `Build it with <code>./build.sh</code> (Rust) or <code>./build-cpp.sh</code> (C++) and serve the <code>web/</code> folder with ` +
      `<code>./serve.sh</code> (opening index.html from disk will not work: browsers block <code>fetch</code> on file:// URLs).`;
    $('startBtn').disabled = true;
  }
  $('loading').style.display = 'none';
  if (S.engine && q.has('autostart')) {
    if (q.has('speed')) S.speed = SPEEDS.includes(+q.get('speed')) ? +q.get('speed') : 1;
    startMatch();
  }
}

/** Which engines have been built (a missing .wasm just disables that choice). */
async function probeEngines() {
  await Promise.all(Object.entries(ENGINES).map(async ([k, m]) => {
    try { S.built[k] = (await fetch(m.file, { method: 'HEAD', cache: 'no-cache' })).ok; } catch { S.built[k] = false; }
  }));
}

/** Load the preferred engine, falling back to the other one if it has not been built. */
async function loadEngine(kind) {
  const order = [kind, ...Object.keys(ENGINES).filter((k) => k !== kind && S.built[k])];
  let first = null;
  for (const k of order) {
    try {
      const e = await Engine.load(k);
      if (k !== kind) toast(`${ENGINES[kind].label} engine is not built — using ${ENGINES[k].label}`);
      S.opts.engine = k;
      return e;
    } catch (err) { first ||= err; }
  }
  throw first;
}

async function switchEngine(kind) {
  if (kind === S.engine?.kind) return;
  try {
    const e = await Engine.load(kind);
    S.engine = e;
    S.opts.engine = kind;
    save();
    applyMods();
    buildMenu();
  } catch (err) {
    $('err').innerHTML = `<b>Could not switch engines.</b><br>${esc(err.message)}`;
  }
}

/** New content (engine switched or mods changed): refresh everything derived from it. */
function onContent(info) {
  setHeroStyles(info);
  clearIconCache();
  if (S.opts.hero >= info.heroes.length) S.opts.hero = 0;
}

// ------------------------------------------------------------------------------ mods

const MOD_KEY = 'lanewars.mods';
function loadModPrefs() { try { return JSON.parse(localStorage.getItem(MOD_KEY) || '{}'); } catch { return {}; } }
function saveModPrefs() {
  const prefs = { enabled: S.mods.filter((m) => m.enabled).map((m) => m.id), custom: S.mods.filter((m) => m.custom).map((m) => ({ id: m.id, file: m.file, text: m.text })) };
  try { localStorage.setItem(MOD_KEY, JSON.stringify(prefs)); } catch { /* too big or storage unavailable: still works this session */ }
}

function modEntry(id, file, text, custom) {
  let meta = {};
  try { const j = JSON.parse(text); if (j && typeof j === 'object' && !Array.isArray(j)) meta = j; } catch { /* the engine reports it */ }
  const str = (v) => (typeof v === 'string' ? v : '');
  return { id, file, text, custom, enabled: false, errors: null, meta: { name: str(meta.name) || file, version: str(meta.version), author: str(meta.author), description: str(meta.description) } };
}

/** Mods shipped in web/mods/ (listed by mods/index.json) plus the ones the player added. */
async function loadModList() {
  const prefs = loadModPrefs(), list = [];
  try {
    const idx = await (await fetch('mods/index.json', { cache: 'no-cache' })).json();
    for (const file of idx.mods || []) {
      try {
        const res = await fetch(`mods/${file}`, { cache: 'no-cache' });
        if (res.ok) list.push(modEntry(`mods/${file}`, file, await res.text(), false));
      } catch { /* skip unreadable files */ }
    }
  } catch { /* no mods folder: fine */ }
  for (const c of prefs.custom || []) list.push(modEntry(c.id, c.file, c.text, true));
  const on = new Set(prefs.enabled || []);
  for (const m of list) m.enabled = on.has(m.id);
  const q = new URLSearchParams(location.search);
  if (q.has('mods')) { const want = new Set(q.get('mods').split(',').filter(Boolean)); for (const m of list) m.enabled = want.has(m.file); }
  S.mods = list;
}

/** Push every enabled mod into the engine (in list order) and re-read the content. */
function applyMods() {
  const e = S.engine;
  e.clearMods();
  for (const m of S.mods) {
    m.errors = m.enabled ? e.loadMod(m.text) : null;
  }
  S.info = e.start({ hero: 0, perTeam: 1, fog: true });
  onContent(S.info);
}

function addModFile(file, text) {
  const id = `custom:${file}`;
  const old = S.mods.findIndex((m) => m.id === id);
  const m = modEntry(id, file, text, true);
  m.enabled = true;
  if (old >= 0) S.mods[old] = m; else S.mods.push(m);
  saveModPrefs();
  applyMods();
  buildMenu();
  const bad = m.errors && m.errors.length;
  toast(bad ? `${m.meta.name}: ${m.errors.length} problem${m.errors.length > 1 ? 's' : ''} — not loaded` : `Loaded mod: ${m.meta.name}`);
}

async function readModFiles(files) {
  for (const f of files) {
    if (f.size > 1 << 20) { toast(`${f.name} is larger than 1 MiB`); continue; }
    addModFile(f.name, await f.text());
  }
}

function buildModList() {
  const box = $('modList');
  box.innerHTML = '';
  if (!S.mods.length) box.innerHTML = '<div class="modempty">No mods yet. Add a <b>.json</b> mod file, or export the content to make your own.</div>';
  for (const m of S.mods) {
    const row = document.createElement('label');
    const failed = m.enabled && m.errors && m.errors.length;
    row.className = 'modrow' + (failed ? ' bad' : m.enabled ? ' on' : '');
    const sub = [m.meta.version && `v${esc(m.meta.version)}`, m.meta.author && `by ${esc(m.meta.author)}`, m.custom ? 'added by you' : esc(m.file)].filter(Boolean).join(' · ');
    const errs = failed
      ? `<div class="errs"><b>Not loaded — ${m.errors.length} problem${m.errors.length > 1 ? 's' : ''}:</b><ul>${m.errors.slice(0, 8).map((x) => `<li>${esc(x)}</li>`).join('')}</ul>${m.errors.length > 8 ? `<i>…and ${m.errors.length - 8} more</i>` : ''}</div>`
      : '';
    row.innerHTML =
      `<input type="checkbox" ${m.enabled ? 'checked' : ''}><div class="mtxt"><b>${esc(m.meta.name)}</b> <span class="msub">${sub}</span>` +
      (m.meta.description ? `<div class="mdesc">${esc(m.meta.description)}</div>` : '') + errs + '</div>' +
      (m.custom ? '<button class="mx" title="Remove this mod">×</button>' : '');
    row.querySelector('input').addEventListener('change', (e) => {
      m.enabled = e.target.checked;
      saveModPrefs();
      applyMods();
      buildMenu();
    });
    row.querySelector('.mx')?.addEventListener('click', (e) => {
      e.preventDefault();
      S.mods = S.mods.filter((x) => x !== m);
      saveModPrefs();
      applyMods();
      buildMenu();
    });
    box.appendChild(row);
  }
  const active = S.mods.filter((m) => m.enabled && m.errors && !m.errors.length).length;
  const failed = S.mods.filter((m) => m.enabled && m.errors && m.errors.length).length;
  $('modSummary').textContent = active || failed ? `${active} active${failed ? ` · ${failed} failed` : ''}` : 'built-in game';
}

function bindModUi() {
  $('modAdd').addEventListener('click', () => $('modFile').click());
  $('modFile').addEventListener('change', async (e) => { await readModFiles(e.target.files); e.target.value = ''; });
  $('modExport').addEventListener('click', () => {
    if (!S.engine) return;
    const blob = new Blob([S.engine.exportContent()], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'lane-wars-content.json';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
  window.addEventListener('dragover', (e) => { if (menuOpen()) { e.preventDefault(); document.body.classList.add('dropping'); } });
  window.addEventListener('dragleave', (e) => { if (!e.relatedTarget) document.body.classList.remove('dropping'); });
  window.addEventListener('drop', (e) => {
    document.body.classList.remove('dropping');
    if (!menuOpen()) return;
    e.preventDefault();
    if (S.engine) readModFiles([...e.dataTransfer.files].filter((f) => /\.json$/i.test(f.name) || f.type === 'application/json'));
  });
}

function openOverlay(id, on) { $(id).classList.toggle('open', on); }
function menuOpen() { return $('menu').classList.contains('open'); }

function buildMenu() {
  const info = S.info, cards = $('cards');
  cards.innerHTML = '';
  info.heroes.forEach((h, i) => {
    const el = document.createElement('div');
    el.className = 'card' + (i === S.opts.hero ? ' sel' : '');
    const abil = [0, 1, 2, 3].map((s) => {
      const a = info.abilities[i * 4 + s];
      return `<li><canvas width="48" height="48" data-ab="${i * 4 + s}"></canvas><span><b>${KEYS[s]}</b> ${esc(a.name)}</span></li>`;
    }).join('');
    el.innerHTML =
      `<div class="row"><canvas class="pt" width="128" height="128"></canvas><div><h3>${esc(h.name)}</h3>` +
      `<div class="t">${esc(h.title)} · ${ATTR[h.primary]}</div></div></div>` +
      `<div class="st"><span>STR <b>${h.str}</b></span><span>AGI <b>${h.agi}</b></span><span>INT <b>${h.int}</b></span>` +
      `<span>${h.range > 200 ? 'Ranged' : 'Melee'} <b>${h.range}</b></span></div><ul>${abil}</ul>`;
    drawHeroPortrait(el.querySelector('canvas.pt'), i, 0);
    el.querySelectorAll('canvas[data-ab]').forEach((c) => paintCanvas(c, abilityIcon(info, +c.dataset.ab, 48)));
    el.addEventListener('click', () => {
      S.opts.hero = i;
      cards.querySelectorAll('.card').forEach((c, k) => c.classList.toggle('sel', k === i));
      save();
    });
    cards.appendChild(el);
  });

  const seg = $('sizeSeg');
  seg.innerHTML = '';
  for (const n of SIZES) {
    const b = document.createElement('button');
    b.textContent = `${n}v${n}`;
    b.className = n === S.opts.size ? 'on' : '';
    b.addEventListener('click', () => {
      S.opts.size = n;
      seg.querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
      save();
    });
    seg.appendChild(b);
  }
  const eseg = $('engineSeg');
  eseg.innerHTML = '<span class="lbl">Engine</span>';
  for (const [k, m] of Object.entries(ENGINES)) {
    const b = document.createElement('button');
    b.textContent = m.label;
    b.className = k === S.engine.kind ? 'on' : '';
    b.disabled = !S.built[k] && k !== S.engine.kind;
    b.title = b.disabled ? `Not built yet — run ${m.build}` : `Run the simulation on the ${m.label} engine`;
    b.addEventListener('click', () => switchEngine(k));
    eseg.appendChild(b);
  }
  $('engineName').textContent = S.engine.label;
  buildModList();
  for (const [id, key] of [['optFog', 'fog'], ['optQuick', 'quick'], ['optWatch', 'watch'], ['opt3d', 'view3d']]) {
    const el = $(id);
    el.checked = !!S.opts[key];
    el.onchange = () => { S.opts[key] = el.checked; save(); if (key === 'view3d' && el.checked) load3D(); };
  }
  $('err').textContent = '';
}

// ------------------------------------------------------------------------------ match lifecycle

/** Load the 3D renderer (render3d.wasm, the C++ renderer). Resolves to false when that is not possible. */
let loading3D = null;
function load3D() {
  loading3D ||= import('./render3d.js')
    .then((m) => m.load())
    .then((R) => { S.R3D = R; return true; })
    .catch((e) => { console.warn('3D view unavailable:', e); loading3D = null; return false; });
  return loading3D;
}

function makeRenderer() {
  const R = S.opts.view3d && S.R3D ? S.R3D : Renderer;
  return new R($('world'), $('mini'), S.info, S.engine.terrain);
}

/** Flip between the 2D and 3D views, mid-match too; the camera carries over. */
async function toggleView() {
  S.opts.view3d = !S.opts.view3d;
  save();
  if (S.opts.view3d && !(await load3D())) {
    S.opts.view3d = false;
    toast('The 3D view needs a browser with WebGPU and render3d.wasm (build it with ./build-render.sh).');
  }
  setText($('bView'), S.opts.view3d ? '2D' : '3D');
  if (!S.renderer || !S.info) return;
  const cam = { ...S.renderer.cam };
  S.renderer.dispose();
  S.renderer = makeRenderer();
  Object.assign(S.renderer.cam, cam);
}

function startMatch() {
  const o = S.opts;
  S.info = S.engine.start({ hero: o.hero, perTeam: o.size, fog: o.fog, bot: o.watch });
  onContent(S.info);
  S.renderer?.dispose();
  S.renderer = makeRenderer();
  setText($('bView'), S.renderer.is3D ? '2D' : '3D');
  S.seen.clear();
  S.over = false;
  S.paused = false;
  S.auto = o.watch;
  S.acc = 0;
  S.ui.mode = null;
  S.ui.cast = null;
  S.ui.selectedId = 0;
  S.boardPinned = S.boardHeld = false;
  S.lockCam = true;
  S.heroPos = null;
  S.prev = S.cur = S.engine.snapshot();
  refreshSeen(S.cur);

  const base = S.info.bases[0];
  S.renderer.cam.x = base[0];
  S.renderer.cam.y = base[1];
  buildHud();
  document.body.classList.remove('dead');
  for (const id of ['menu', 'end', 'pause']) openOverlay(id, false);
  closePanel('shop');
  closePanel('board');
  $('hud').classList.remove('hidden');
  $('feed').innerHTML = '';
  S.running = true;
  S.last = performance.now();
  setText($('bSpeed'), `${S.speed}×`);
  setClass($('bAuto'), 'on', S.auto);
  toast('Destroy the enemy Ancient. Right-click to move.');
}

function leaveToMenu() {
  S.running = false;
  S.paused = false;
  $('hud').classList.add('hidden');
  for (const id of ['end', 'pause']) openOverlay(id, false);
  document.body.classList.remove('dead');
  buildMenu();
  openOverlay('menu', true);
}

function showEnd(f) {
  const win = f.winner === f.playerTeam;
  const m = me();
  $('end').className = `overlay open ${win ? 'win' : 'lose'}`;
  $('endTitle').textContent = win ? 'VICTORY' : 'DEFEAT';
  const kda = m ? ` · ${m.get('kills')}/${m.get('deaths')}/${m.get('assists')} K/D/A` : '';
  $('endSub').textContent = `${TEAM[f.winner].name} destroyed the ${TEAM[1 - f.winner].name} Ancient in ${fmtTime(f.time)}${kda}`;
}

// ------------------------------------------------------------------------------ HUD construction

function buildHud() {
  const info = S.info, hero = S.opts.hero;
  drawHeroPortrait($('portrait'), hero, 0);
  setText($('hname'), info.heroes[hero].name);

  const ab = $('abilities');
  ab.innerHTML = '';
  for (let s = 0; s < 4; s++) {
    const a = info.abilities[hero * 4 + s];
    const el = document.createElement('div');
    el.className = 'slot locked';
    el.innerHTML =
      `<canvas width="124" height="124"></canvas><div class="key">${KEYS[s]}</div><div class="cd"></div>` +
      `<div class="pips">${'<i></i>'.repeat(a.max_level)}</div><div class="plus">+</div>`;
    paintCanvas(el.querySelector('canvas'), abilityIcon(info, hero * 4 + s, 62));
    el.addEventListener('click', () => beginCast(s));
    el.querySelector('.plus').addEventListener('click', (e) => { e.stopPropagation(); learn(s); });
    el.addEventListener('mouseenter', () => showTip(() => abilityTip(s), el));
    el.addEventListener('mouseleave', hideTip);
    ab.appendChild(el);
  }

  const items = $('items');
  items.innerHTML = '';
  for (let i = 0; i < info.inv_slots; i++) {
    const el = document.createElement('div');
    el.className = 'slot item';
    el.innerHTML = '<canvas width="100" height="100"></canvas><div class="key">' + (i + 1) + '</div><div class="cd"></div><div class="ch"></div>';
    el.addEventListener('click', () => beginCast(10 + i));
    el.addEventListener('contextmenu', (e) => { e.preventDefault(); sell(i); });
    el.addEventListener('mouseenter', () => showTip(() => inventoryTip(i), el));
    el.addEventListener('mouseleave', hideTip);
    items.appendChild(el);
  }

  // shop grid
  const grid = $('shopGrid');
  grid.innerHTML = '';
  for (let id = 1; id < info.items.length; id++) {
    const it = info.items[id];
    const el = document.createElement('div');
    el.className = 'shopItem';
    el.dataset.id = id;
    el.innerHTML = `<canvas width="92" height="92"></canvas><span>${esc(it.name)}</span><span class="c">${it.cost}</span>`;
    paintCanvas(el.querySelector('canvas'), itemIcon(info, id, 46));
    el.addEventListener('click', () => buy(id));
    el.addEventListener('mouseenter', () => showTip(() => itemTip(id), el));
    el.addEventListener('mouseleave', hideTip);
    grid.appendChild(el);
  }
}

// ------------------------------------------------------------------------------ tooltips

const tipEl = () => $('tip');
let tipBuilder = null, tipAnchor = null;

/** `build` is re-run every frame while the tooltip is visible, so numbers and slots stay live. */
function showTip(build, anchor) {
  tipBuilder = build;
  tipAnchor = anchor;
  renderTip();
}

function renderTip() {
  const t = tipEl(), html = tipBuilder();
  if (t.__h !== html) { t.__h = html; t.innerHTML = html; }
  t.style.display = 'block';
  const r = tipAnchor.getBoundingClientRect(), w = t.offsetWidth, h = t.offsetHeight;
  t.style.left = clamp(r.left + r.width / 2 - w / 2, 8, window.innerWidth - w - 8) + 'px';
  t.style.top = (r.top - h - 12 < 8 ? r.bottom + 12 : r.top - h - 12) + 'px';
}

function hideTip() {
  tipBuilder = tipAnchor = null;
  tipEl().style.display = 'none';
}

function levels(arr, cur, fmt = (v) => v) {
  return arr.map((v, i) => (i === cur - 1 ? `<span class="cur">${fmt(v)}</span>` : fmt(v))).join(' / ');
}

function abilityTip(slot) {
  const info = S.info, m = me(), hero = S.opts.hero, a = info.abilities[hero * 4 + slot];
  const lvl = m ? m.get(`ab${slot}`) : 0;
  const rows = [];
  const row = (name, arr, fmt) => { if (arr.some((v) => v > 0)) rows.push(`<tr><td>${name}</td><td>${levels(arr, lvl, fmt)}</td></tr>`); };
  row('Damage', a.dmg, Math.round);
  row('Range', a.range, Math.round);
  row('Radius', a.radius, Math.round);
  row('Duration', a.dur, (v) => `${+v.toFixed(1)}s`);
  row('Mana', a.mana, Math.round);
  row('Cooldown', a.cd, (v) => `${+v.toFixed(1)}s`);
  let hint = '';
  if (lvl < a.max_level) {
    const req = a.req_level[lvl];
    if (m && m.get('level') < req) hint = `<div class="act">Next level needs hero level ${req}</div>`;
    else if (m && m.get('skill_pts') > 0) hint = `<div class="act">Shift+${KEYS[slot]} or click + to learn</div>`;
  }
  return `<h4><span>${esc(a.name)}</span><span>${KEYS[slot]}</span></h4><p>${esc(a.desc)}</p>` +
    `<div style="color:#9aa0ad;font-size:12px">${DMG_TYPE[a.dtype] ?? ''} damage · level ${lvl}/${a.max_level}</div>` +
    `<table>${rows.join('')}</table>${hint}`;
}

function statLines(it) {
  const names = { str: 'Strength', agi: 'Agility', int: 'Intelligence', dmg: 'Damage', armor: 'Armor', hp: 'Health', mana: 'Mana', aspd: 'Attack speed', ms: 'Move speed', hpreg: 'Health regen', mpreg: 'Mana regen' };
  const out = [];
  for (const k of Object.keys(names)) if (it[k]) out.push(`<tr><td>${names[k]}</td><td class="bonus">+${it[k]}${k === 'aspd' ? '%' : ''}</td></tr>`);
  return out.join('');
}

function itemActive(it) {
  const a = S.info.act;
  if (it.active === a.salve) return 'Active: restores health over time. Stacks up to 5.';
  if (it.active === a.mana) return 'Active: restores mana over time. Stacks up to 5.';
  if (it.active === a.blink) return `Active: teleport up to ${S.info.blink_dagger_range} units toward the cursor.`;
  return '';
}

function itemTip(id) {
  const it = S.info.items[id];
  const act = itemActive(it);
  return `<h4><span>${esc(it.name)}</span><span class="g-c">${it.cost}</span></h4><table>${statLines(it)}</table>` +
    (act ? `<div class="act">${act}</div>` : '') + `<div style="color:#9aa0ad;font-size:12px;margin-top:4px">Sells for ${Math.floor(it.cost / 2)}</div>`;
}

function inventoryTip(i) {
  const m = me();
  const id = m ? m.get(`it${i}`) : 0;
  return id ? itemTip(id) : '<h4><span>Empty slot</span></h4>';
}

// ------------------------------------------------------------------------------ commands

function worldMouse() { return S.renderer.s2w(S.mouse.x, S.mouse.y); }

function isEnemy(i) {
  const f = S.cur, o = i * S.info.unit_fields.length;
  return f.units[o + S.info.U.team] !== f.playerTeam;
}

function describeCast(slot) {
  const info = S.info, m = me();
  if (!m || !m.alive || !m.hasUnit) { toast('You are dead'); return null; }
  if (m.unit('status') & info.status.stun) { toast('Stunned'); return null; }
  if (slot < 4) {
    const a = info.abilities[m.def * 4 + slot], lvl = m.get(`ab${slot}`);
    if (lvl <= 0) { toast(m.get('skill_pts') > 0 ? `Not learned — Shift+${KEYS[slot]} to learn` : 'Ability not learned yet'); return null; }
    if (m.get(`cd${slot}`) > 0.05) { toast('Ability is on cooldown'); return null; }
    if (m.unit('mana') < a.mana[lvl - 1]) { toast('Not enough mana'); return null; }
    const base = { slot, range: a.range[lvl - 1], radius: a.radius[lvl - 1], color: abilityColor(info, m.def * 4 + slot) };
    const c = info.cast;
    if (a.cast === c.target) return { ...base, kind: 'target' };
    if (a.cast === c.line) return { ...base, kind: 'line' };
    if (a.cast === c.point) return { ...base, kind: 'point' };
    if (a.cast === c.blink) return { ...base, kind: 'blink' };
    if (a.cast === c.self_aoe || a.cast === c.self_buff) return { ...base, kind: 'self' };
    return null;
  }
  const i = slot - 10, id = m.get(`it${i}`);
  if (!id) return null;
  const it = info.items[id];
  if (!it.active) { toast(`${it.name} is a passive item`); return null; }
  if (m.get(`icd${i}`) > 0.05) { toast('Item is on cooldown'); return null; }
  if (it.active === info.act.blink) return { slot, kind: 'blink', range: info.blink_dagger_range, radius: 0, color: '#c9a7ff' };
  return { slot, kind: 'self' };
}

/** Hotkey / click on an ability or item: cast now, or enter targeting mode. */
function beginCast(slot) {
  if (!S.running || S.paused || S.over) return;
  const d = describeCast(slot);
  if (!d) return;
  if (d.kind === 'self') {
    const m = me();
    S.engine.cast(slot, 0, m.unit('x'), m.unit('y'));
    return;
  }
  if (S.opts.quick && S.mouse.inCanvas) {
    const [wx, wy] = worldMouse();
    if (castAt(d, wx, wy, S.view.hover, true)) return;
  }
  S.ui.mode = 'cast';
  S.ui.cast = d;
}

/** Fire a targeted cast. Returns false (and keeps targeting) when there is no valid target. */
function castAt(d, wx, wy, hover, quiet = false) {
  const f = S.cur, us = S.info.unit_fields.length, U = S.info.U;
  if (d.kind === 'target') {
    if (hover < 0 || !isEnemy(hover)) { if (!quiet) toast('Click an enemy unit'); return false; }
    const o = hover * us;
    S.engine.cast(d.slot, f.units[o + U.id], S.view.px[hover], S.view.py[hover]);
    S.renderer.ping(S.view.px[hover], S.view.py[hover], 'cast');
  } else {
    S.engine.cast(d.slot, 0, wx, wy);
    S.renderer.ping(wx, wy, 'cast');
  }
  S.ui.mode = null;
  S.ui.cast = null;
  return true;
}

function learn(slot) {
  const m = me();
  if (!m || S.over) return;
  if (m.get('skill_pts') <= 0) { toast('No skill points to spend'); return; }
  S.engine.levelUp(slot);
}

function buy(id) {
  const m = me(), info = S.info, it = info.items[id];
  if (!m || S.over) return;
  if (!m.alive) return toast('You are dead');
  if (!S.cur.g[info.G.near_shop]) return toast('Return to your fountain to buy items');
  if (m.get('gold') < it.cost) return toast('Not enough gold');
  let room = false;
  for (let i = 0; i < info.inv_slots; i++) {
    const have = m.get(`it${i}`);
    if (!have || (it.stack && have === id && m.get(`ch${i}`) < 5)) room = true;
  }
  if (!room) return toast('Inventory is full');
  S.engine.buy(id);
}

function sell(slot) {
  const m = me();
  if (!m || S.over || !m.get(`it${slot}`)) return;
  if (!S.cur.g[S.info.G.near_shop]) return toast('Return to your fountain to sell items');
  S.engine.sell(slot);
}

function setMode(mode) {
  S.ui.mode = mode;
  S.ui.cast = null;
}

function togglePause(force) {
  if (!S.running || S.over) return;
  S.paused = force ?? !S.paused;
  openOverlay('pause', S.paused);
  hideTip();
}

function togglePanel(id) {
  const el = $(id);
  if (el.classList.contains('open')) closePanel(id);
  else { if (id === 'shop') closePanel('board'); el.classList.add('open'); }
}
function closePanel(id) {
  $(id).classList.remove('open');
  if (id === 'board') S.boardPinned = false;
  hideTip(); // a hidden element never fires mouseleave, so its tooltip would stay stuck
}

function setAuto(on) {
  S.auto = on;
  S.engine.setBot(on);
  setClass($('bAuto'), 'on', on);
  toast(on ? 'Autopilot on — press T to take over' : 'You are in control');
}

function centerOnHero() {
  if (S.heroPos) { S.renderer.cam.x = S.heroPos[0]; S.renderer.cam.y = S.heroPos[1]; }
}

function unlockCamera() {
  if (S.lockCam) { S.lockCam = false; toast('Camera unlocked — press Y to lock'); }
}

// ------------------------------------------------------------------------------ input

function pickUnit(wx, wy) {
  const f = S.cur;
  if (!f) return -1;
  const { U } = S.info, us = S.info.unit_fields.length;
  let best = -1, bd = Infinity;
  for (let i = 0; i < f.nUnits; i++) {
    const o = i * us;
    if (f.units[o + U.visible] <= 0) continue;
    const r = f.units[o + U.radius] * 1.3 + 6;
    const d = Math.hypot(S.view.px[i] - wx, S.view.py[i] - wy) - r;
    if (d < 0 && d < bd) { bd = d; best = i; }
  }
  return best;
}

function rightClick(wx, wy) {
  const f = S.cur, { U, kind } = S.info, us = S.info.unit_fields.length;
  const h = S.view.hover;
  if (h >= 0) {
    const o = h * us, id = f.units[o + U.id];
    if (id !== f.player) {
      if (isEnemy(h)) {
        S.engine.attack(id);
        S.renderer.ping(S.view.px[h], S.view.py[h], 'attack');
        return;
      }
      // deny: finish off a hurt allied creep so the enemy cannot farm it
      const k = f.units[o + U.kind];
      const creep = k === kind.melee || k === kind.ranged || k === kind.siege;
      if (creep && f.units[o + U.hp] < f.units[o + U.max_hp] * 0.5) {
        S.engine.attack(id);
        S.renderer.ping(S.view.px[h], S.view.py[h], 'attack');
        return;
      }
    }
  }
  S.engine.move(wx, wy);
  S.renderer.ping(wx, wy, 'move');
}

function leftClick(wx, wy) {
  const h = S.view.hover;
  if (S.ui.mode === 'cast' && S.ui.cast) {
    castAt(S.ui.cast, wx, wy, h);
    return;
  }
  if (S.ui.mode === 'attackmove') {
    if (h >= 0 && isEnemy(h)) {
      S.engine.attack(S.cur.units[h * S.info.unit_fields.length + S.info.U.id]);
    } else {
      S.engine.attackMove(wx, wy);
    }
    S.renderer.ping(wx, wy, 'attack');
    setMode(null);
    return;
  }
  S.ui.selectedId = h >= 0 ? S.cur.units[h * S.info.unit_fields.length + S.info.U.id] : 0;
}

function miniWorld(e) {
  const r = $('mini').getBoundingClientRect(), w = S.info.world;
  return [clamp((e.clientX - r.left) / r.width, 0, 1) * w, clamp((e.clientY - r.top) / r.height, 0, 1) * w];
}

function bindInput() {
  const canvas = $('world'), mini = $('mini');
  document.addEventListener('contextmenu', (e) => e.preventDefault());

  window.addEventListener('mousemove', (e) => {
    S.mouse.x = e.clientX;
    S.mouse.y = e.clientY;
    S.mouse.inWindow = true;
    if (S.miniDrag && S.running) { const [x, y] = miniWorld(e); S.renderer.cam.x = x; S.renderer.cam.y = y; }
  });
  document.addEventListener('mouseleave', () => { S.mouse.inWindow = false; });
  window.addEventListener('mouseup', () => { S.miniDrag = false; });
  canvas.addEventListener('mouseenter', () => { S.mouse.inCanvas = true; });
  canvas.addEventListener('mouseleave', () => { S.mouse.inCanvas = false; S.view.hover = -1; });

  canvas.addEventListener('mousedown', (e) => {
    if (!S.running || S.paused || S.over) return;
    const [wx, wy] = S.renderer.s2w(e.clientX, e.clientY);
    if (e.button === 0) leftClick(wx, wy);
    else if (e.button === 2) {
      if (S.ui.mode) setMode(null);
      else rightClick(wx, wy);
    }
  });
  canvas.addEventListener('wheel', (e) => {
    if (!S.running) return;
    e.preventDefault();
    const R = S.renderer, [bx, by] = R.s2w(e.clientX, e.clientY);
    R.cam.zoom *= Math.exp(-e.deltaY * 0.001);
    R.clampCam();
    if (!S.lockCam) { // keep the point under the cursor fixed
      const [ax, ay] = R.s2w(e.clientX, e.clientY);
      R.cam.x += bx - ax;
      R.cam.y += by - ay;
      R.clampCam();
    }
  }, { passive: false });

  mini.addEventListener('mousedown', (e) => {
    if (!S.running || S.over) return;
    const [x, y] = miniWorld(e);
    if (e.button === 2) { S.engine.move(x, y); S.renderer.ping(x, y, 'move'); return; }
    if (e.button !== 0) return;
    if (S.ui.mode === 'attackmove') { S.engine.attackMove(x, y); S.renderer.ping(x, y, 'attack'); setMode(null); return; }
    unlockCamera();
    S.miniDrag = true;
    S.renderer.cam.x = x;
    S.renderer.cam.y = y;
  });

  window.addEventListener('keydown', onKeyDown);
  window.addEventListener('keyup', (e) => {
    S.keys.delete(e.code);
    if (e.code === 'Tab') S.boardHeld = false;
  });
  window.addEventListener('blur', () => { S.keys.clear(); S.boardHeld = false; });
  window.addEventListener('resize', () => { if (S.renderer) { S.renderer.resize(); S.renderer.clampCam(); } });

  $('startBtn').addEventListener('click', startMatch);
  $('againBtn').addEventListener('click', startMatch);
  $('bPause').addEventListener('click', () => togglePause());
  $('bMenu').addEventListener('click', () => { if (S.over || confirm('Leave the current match?')) leaveToMenu(); });
  $('bAuto').addEventListener('click', () => setAuto(!S.auto));
  $('bView').addEventListener('click', () => toggleView());
  window.addEventListener('render3d-lost', () => {
    S.R3D = null;
    loading3D = Promise.resolve(false); // do not try the lost GPU device again
    if (S.renderer?.is3D) toggleView();
  });
  $('bSpeed').addEventListener('click', () => {
    S.speed = SPEEDS[(SPEEDS.indexOf(S.speed) + 1) % SPEEDS.length];
    setText($('bSpeed'), `${S.speed}×`);
  });
  $('shopBtn').addEventListener('click', () => togglePanel('shop'));
  $('boardBtn').addEventListener('click', () => { S.boardPinned = !S.boardPinned; if (!S.boardPinned) closePanel('board'); });
  $('pause').addEventListener('click', () => togglePause(false));
}

function onKeyDown(e) {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  S.keys.add(e.code);
  if (!S.running || menuOpen()) return;
  const code = e.code;
  if (code === 'Tab') { e.preventDefault(); S.boardHeld = true; return; }
  if (code === 'KeyP' || code === 'Escape') {
    e.preventDefault();
    if (code === 'Escape' && S.ui.mode) setMode(null);
    else if (code === 'Escape' && $('shop').classList.contains('open')) closePanel('shop');
    else if (code === 'Escape' && S.boardPinned) closePanel('board');
    else togglePause();
    return;
  }
  if (S.paused || S.over || e.repeat) return;
  const ab = ['KeyQ', 'KeyW', 'KeyE', 'KeyR'].indexOf(code);
  if (ab >= 0) { e.shiftKey ? learn(ab) : beginCast(ab); return; }
  const m = /^(?:Digit|Numpad)([1-6])$/.exec(code);
  if (m) { beginCast(10 + (+m[1] - 1)); return; }
  switch (code) {
    case 'KeyA': setMode('attackmove'); break;
    case 'KeyS': S.engine.stop(); setMode(null); break;
    case 'KeyH': S.engine.hold(); setMode(null); break;
    case 'KeyB': togglePanel('shop'); break;
    case 'KeyT': setAuto(!S.auto); break;
    case 'KeyV': toggleView(); break;
    case 'KeyY': S.lockCam = !S.lockCam; toast(S.lockCam ? 'Camera locked to hero' : 'Camera unlocked'); break;
    case 'Space': e.preventDefault(); centerOnHero(); break;
    default: break;
  }
}

// ------------------------------------------------------------------------------ per-snapshot work

function onSnapshot(f) {
  refreshSeen(f);
  S.renderer.onEvents(f, S.ui);
  const { E, ev, kind } = S.info, es = S.info.event_fields.length, e = f.events;
  for (let i = 0; i < f.events.length; i += es) {
    const t = e[i + E.type], a = e[i + E.a], b = e[i + E.b], vis = e[i + E.visible] > 0;
    if (t === ev.kill && vis && e[i + E.w] === kind.hero) {
      feed(`${label(b)} killed ${label(a)}`);
    } else if (t === ev.building) {
      const team = e[i + E.t], ancient = b === kind.ancient;
      feed(`<span class="${team === 0 ? 'r-c' : 'd-c'}">${esc(TEAM[team]?.name ?? '')}</span> ${ancient ? 'Ancient' : 'tower'} destroyed`);
      if (!ancient) announce(`${TEAM[team].name.toUpperCase()} TOWER DESTROYED`, TEAM[1 - team].main);
    } else if (t === ev.respawn && a === f.player) {
      toast('You have respawned');
    }
  }
  if (f.winner >= 0 && !S.over) {
    S.over = true;
    announce(f.winner === f.playerTeam ? 'VICTORY' : 'DEFEAT', TEAM[f.winner].main);
    setTimeout(() => { if (S.running && S.over) showEnd(S.cur); }, 2200);
  }
}

function feed(html) {
  const box = $('feed'), d = document.createElement('div');
  d.innerHTML = html;
  box.appendChild(d);
  while (box.children.length > 6) box.firstChild.remove();
  setTimeout(() => d.remove(), 9000);
}

function announce(text, color) {
  const box = $('announce'), d = document.createElement('div');
  d.textContent = text;
  d.style.color = color;
  box.appendChild(d);
  setTimeout(() => d.remove(), 3000);
}

// ------------------------------------------------------------------------------ view (interpolation, hover, camera)

function buildView(alpha, dt) {
  const f = S.cur, p = S.prev, v = S.view, { U } = S.info, us = S.info.unit_fields.length, n = f.nUnits;
  if (v.px.length < n) { v.px = new Float32Array(n * 2); v.py = new Float32Array(n * 2); }
  for (let i = 0; i < n; i++) {
    const o = i * us, x = f.units[o + U.x], y = f.units[o + U.y];
    let px = x, py = y;
    if (p !== f && alpha < 1) {
      const j = p.index.get(f.units[o + U.id]);
      if (j !== undefined) {
        const x0 = p.units[j * us + U.x], y0 = p.units[j * us + U.y];
        if ((x - x0) ** 2 + (y - y0) ** 2 < 300 * 300) { px = x0 + (x - x0) * alpha; py = y0 + (y - y0) * alpha; }
      }
    }
    v.px[i] = px;
    v.py[i] = py;
  }
  v.cur = f;
  v.time = performance.now() / 1000;

  const R = S.renderer, m = me();
  if (m && m.hasUnit) {
    const i = m.unitIndex;
    S.heroPos = [v.px[i], v.py[i]];
  }
  S.ui.mouseWorld = S.mouse.inWindow ? R.s2w(S.mouse.x, S.mouse.y) : null;
  v.hover = S.mouse.inCanvas && S.ui.mouseWorld ? pickUnit(S.ui.mouseWorld[0], S.ui.mouseWorld[1]) : -1;
  $('world').style.cursor = S.ui.mode ? 'crosshair' : v.hover >= 0 && isEnemy(v.hover) ? 'crosshair' : 'default';

  // camera: follow hero, or pan with arrow keys / screen edges
  const cam = R.cam, speed = 1100 / cam.zoom * dt;
  let dx = 0, dy = 0;
  if (S.keys.has('ArrowLeft')) dx -= 1;
  if (S.keys.has('ArrowRight')) dx += 1;
  if (S.keys.has('ArrowUp')) dy -= 1;
  if (S.keys.has('ArrowDown')) dy += 1;
  if (S.mouse.inWindow && !S.lockCam && !S.miniDrag) {
    const edge = 6, { x, y } = S.mouse;
    if (x <= edge) dx -= 1;
    if (x >= R.W - edge) dx += 1;
    if (y <= edge) dy -= 1;
    if (y >= R.H - edge) dy += 1;
  }
  if (dx || dy) {
    if (S.lockCam && (S.keys.has('ArrowLeft') || S.keys.has('ArrowRight') || S.keys.has('ArrowUp') || S.keys.has('ArrowDown'))) unlockCamera();
    if (!S.lockCam) { cam.x += dx * speed; cam.y += dy * speed; }
  }
  if (S.lockCam && S.heroPos) {
    const k = 1 - Math.exp(-dt * 9);
    cam.x += (S.heroPos[0] - cam.x) * k;
    cam.y += (S.heroPos[1] - cam.y) * k;
  }
  R.clampCam();
}

// ------------------------------------------------------------------------------ HUD update

function updateHud() {
  const f = S.cur, info = S.info, G = info.G, m = me();
  if (!m) return;

  // top bar
  let kills = [0, 0];
  const hs = info.hero_fields.length, H = info.H;
  for (let i = 0; i < f.nHeroes; i++) kills[f.heroes[i * hs + H.team]] += f.heroes[i * hs + H.kills];
  let towers = [0, 0];
  const us = info.unit_fields.length, U = info.U;
  for (let i = 0; i < f.nUnits; i++) if (f.units[i * us + U.kind] === info.kind.tower) towers[f.units[i * us + U.team]]++;
  setText($('sc0'), kills[0]); setText($('sc1'), kills[1]);
  setText($('tw0'), towers[0]); setText($('tw1'), towers[1]);
  setText($('time'), fmtTime(f.time));
  const wave = f.g[G.wave_no], next = Math.ceil(f.g[G.next_wave]);
  setText($('wave'), wave === 0 ? `First wave in ${next}s` : `Wave ${wave} · next in ${next}s`);

  // hero panel
  const hp = m.unit('hp'), maxHp = m.unit('max_hp'), mp = m.unit('mana'), maxMp = m.unit('max_mana');
  const dead = !m.alive;
  document.body.classList.toggle('dead', dead);
  setText($('respawn'), dead ? `Respawning in ${Math.ceil(m.get('respawn'))}s` : '');
  setText($('hlevel'), `Level ${m.get('level')} · ${info.heroes[m.def].title}`);
  setStyle($('hpFill'), 'width', `${dead || !maxHp ? 0 : clamp(hp / maxHp, 0, 1) * 100}%`);
  setStyle($('mpFill'), 'width', `${dead || !maxMp ? 0 : clamp(mp / maxMp, 0, 1) * 100}%`);
  setText($('hpText'), dead ? 'DEAD' : `${Math.ceil(hp)} / ${Math.round(maxHp)}  +${m.get('hp_regen').toFixed(1)}`);
  setText($('mpText'), dead ? '' : `${Math.floor(mp)} / ${Math.round(maxMp)}  +${m.get('mana_regen').toFixed(1)}`);
  const xpPrev = m.get('xp_prev'), xpNext = m.get('xp_next'), maxed = m.get('level') >= info.max_level;
  setStyle($('xpFill'), 'width', `${maxed ? 100 : clamp((m.get('xp') - xpPrev) / Math.max(1, xpNext - xpPrev), 0, 1) * 100}%`);
  setHtml($('stats'),
    [['STR', Math.round(m.get('str'))], ['AGI', Math.round(m.get('agi'))], ['INT', Math.round(m.get('int'))],
      ['DMG', `${Math.round(m.get('dmg_min'))}-${Math.round(m.get('dmg_max'))}`], ['ARM', m.get('armor').toFixed(1)], ['MS', Math.round(m.get('ms'))],
      ['RNG', Math.round(m.unit('range'))], ['AS', (1 / Math.max(0.05, m.get('atk_time'))).toFixed(2)], ['PTS', m.get('skill_pts')]]
      .map(([k, v]) => `<span>${k} <b>${v}</b></span>`).join(''));

  // abilities
  const slots = $('abilities').children, pts = m.get('skill_pts'), level = m.get('level');
  for (let s = 0; s < 4; s++) {
    const el = slots[s], a = info.abilities[m.def * 4 + s], lvl = m.get(`ab${s}`), cd = m.get(`cd${s}`);
    const cost = lvl > 0 ? a.mana[lvl - 1] : 0, learned = lvl > 0;
    setClass(el, 'locked', !learned);
    setClass(el, 'nomana', learned && m.unit('mana') < cost);
    setClass(el, 'ready', learned && cd <= 0.05 && m.unit('mana') >= cost && m.alive);
    setClass(el, 'active', S.ui.mode === 'cast' && S.ui.cast?.slot === s);
    const total = learned ? a.cd[lvl - 1] : 1, cdEl = el.querySelector('.cd');
    setStyle(cdEl, 'display', cd > 0.05 ? 'flex' : 'none');
    if (cd > 0.05) { cdEl.style.setProperty('--p', String(clamp(cd / total, 0, 1))); setText(cdEl, cd >= 10 ? Math.ceil(cd) : cd.toFixed(1)); }
    el.querySelectorAll('.pips i').forEach((p, i) => setClass(p, 'f', i < lvl));
    const canLearn = pts > 0 && lvl < a.max_level && level >= a.req_level[lvl];
    setStyle(el.querySelector('.plus'), 'display', canLearn ? 'block' : 'none');
  }

  // inventory
  const items = $('items').children;
  for (let i = 0; i < info.inv_slots; i++) {
    const el = items[i], id = m.get(`it${i}`), cd = m.get(`icd${i}`), ch = m.get(`ch${i}`), cv = el.querySelector('canvas');
    if (cv.__id !== id) {
      cv.__id = id;
      const c = cv.getContext('2d');
      c.clearRect(0, 0, cv.width, cv.height);
      if (id) c.drawImage(itemIcon(info, id, 50), 0, 0, cv.width, cv.height);
    }
    el.style.opacity = id ? '1' : '0.45';
    setText(el.querySelector('.ch'), ch > 0 ? ch : '');
    const cdEl = el.querySelector('.cd'), total = id ? Math.max(0.1, info.items[id].cd) : 1;
    setStyle(cdEl, 'display', id && cd > 0.05 ? 'flex' : 'none');
    if (id && cd > 0.05) { cdEl.style.setProperty('--p', String(clamp(cd / total, 0, 1))); setText(cdEl, cd >= 10 ? Math.ceil(cd) : cd.toFixed(1)); }
  }

  // gold, kda, shop
  const gold = Math.floor(m.get('gold'));
  setText($('gold'), gold);
  setHtml($('kda'), `<b>${m.get('kills')}</b> / <b>${m.get('deaths')}</b> / <b>${m.get('assists')}</b> &nbsp;·&nbsp; <b>${m.get('last_hits')}</b> LH <b>${m.get('denies')}</b> DN`);
  const near = f.g[G.near_shop] > 0;
  setClass($('shopBtn'), 'near', near);
  if ($('shop').classList.contains('open')) {
    setText($('shopGold'), `${gold} gold`);
    const sub = $('shopSub');
    setText(sub, near ? 'Click an item to buy it · right-click an inventory item to sell it for half price' : 'Return to your fountain to buy or sell items');
    setClass(sub, 'warn', !near);
    for (const el of $('shopGrid').children) setClass(el, 'poor', gold < info.items[+el.dataset.id].cost);
  }

  updateSelection();
  updateBoard();
  if (tipBuilder) renderTip();
}

function updateSelection() {
  const f = S.cur, info = S.info, el = $('sel'), id = S.ui.selectedId;
  const i = id ? f.index.get(id) : undefined;
  const us = info.unit_fields.length, U = info.U;
  if (i === undefined || f.units[i * us + U.visible] <= 0) { el.style.display = 'none'; return; }
  const o = i * us, u = f.units, k = u[o + U.kind];
  const hp = u[o + U.hp], max = u[o + U.max_hp], team = u[o + U.team];
  const cls = team === 0 ? 'r-c' : team === 1 ? 'd-c' : 'g-c';
  el.style.display = 'block';
  setHtml(el,
    `<b class="n ${cls}">${esc(unitName(u[o + U.def], k))}${k === info.kind.hero ? ` · Lv ${u[o + U.level]}` : ''}</b>` +
    `<div class="bar hp"><i style="width:${clamp(hp / max, 0, 1) * 100}%"></i><span>${Math.ceil(hp)} / ${Math.round(max)}</span></div>` +
    `Damage <b style="color:#e9e4d4">${Math.round(u[o + U.dmg])}</b> · Armor <b style="color:#e9e4d4">${u[o + U.armor].toFixed(1)}</b>` +
    (u[o + U.range] > 0 ? ` · Range <b style="color:#e9e4d4">${Math.round(u[o + U.range])}</b>` : ''));
}

let boardClock = 0;
function updateBoard() {
  const show = S.boardHeld || S.boardPinned, el = $('board');
  if (!show) { el.classList.remove('open'); return; }
  if (!el.classList.contains('open')) { closePanel('shop'); el.classList.add('open'); boardClock = 0; }
  const now = performance.now();
  if (now - boardClock < 250) return;
  boardClock = now;
  const f = S.cur, info = S.info, hs = info.hero_fields.length, H = info.H;
  const rows = [[], []];
  for (let i = 0; i < f.nHeroes; i++) {
    const o = i * hs, g = (n) => f.heroes[o + H[n]], isMe = g('unit') === f.player;
    rows[g('team')].push(
      `<tr class="${isMe ? 'me' : ''}"><td><canvas width="40" height="40" data-def="${g('def')}" data-team="${g('team')}"></canvas>` +
      `${esc(info.heroes[g('def')].name)}${isMe ? ' (you)' : ''}${g('bot') ? ' <span style="color:#9aa0ad">bot</span>' : ''}</td>` +
      `<td>${g('level')}</td><td>${g('kills')}</td><td>${g('deaths')}</td><td>${g('assists')}</td><td>${g('last_hits')}</td><td>${g('denies')}</td>` +
      `<td>${Math.round(g('networth'))}</td></tr>`);
  }
  const sec = (t, cls) => `<tr><td colspan="8" class="${cls}">${TEAM[t].name}</td></tr>${rows[t].join('')}`;
  setHtml(el,
    '<h3><span>Scoreboard</span></h3><table class="board"><tr><th>Hero</th><th>Lv</th><th>K</th><th>D</th><th>A</th><th>LH</th><th>DN</th><th>Net worth</th></tr>' +
    sec(0, 'sec-r') + sec(1, 'sec-d') + '</table>');
  el.querySelectorAll('canvas[data-def]').forEach((c) => drawHeroPortrait(c, +c.dataset.def, +c.dataset.team));
}

// ------------------------------------------------------------------------------ main loop

function frame(now) {
  requestAnimationFrame(frame);
  const dt = clamp((now - S.last) / 1000, 0, 0.1); // rAF time can trail S.last when a match starts mid-frame
  S.last = now;
  if (!S.running) return;

  const tick = 1 / S.info.tick_hz;
  if (!S.paused && !S.over) {
    S.acc += dt * S.speed;
    let n = Math.floor(S.acc / tick);
    if (n > 0) {
      n = Math.min(n, 24);
      S.acc = Math.min(S.acc - n * tick, tick);
      S.engine.step(n);
      S.prev = S.cur;
      S.cur = S.engine.snapshot();
      onSnapshot(S.cur);
    }
  }
  const alpha = S.paused || S.over ? 1 : clamp(S.acc / tick, 0, 1);
  buildView(alpha, dt);
  S.renderer.update(S.paused ? 0 : dt);
  S.renderer.draw(S.view, S.ui);
  updateHud();
}

// ------------------------------------------------------------------------------ go

bindInput();
requestAnimationFrame((t) => { S.last = t; requestAnimationFrame(frame); });
boot();

// Handy for the console and for automated tests.
window.__game = { S, startMatch, toggleView, beginCast, buy, applyMods, addModFile, switchEngine };
