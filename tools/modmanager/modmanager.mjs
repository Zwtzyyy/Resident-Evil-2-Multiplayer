#!/usr/bin/env node
// re2r_mp mod manager - a minimal, open replacement for Fluffy Mod Manager.
//
// Why: Fluffy is closed-source, so we do not use it. This manages mod payloads (REFramework
// scripts/plugins and PAK "natives/" trees) by copying them into the game directory and
// recording every file it writes, so a mod can be disabled/removed cleanly.
//
// Layout (inside the manager's data dir, default ./data):
//   config.json          { gamePath, dataDir }
//   registry.json        { mods: { <slug>: { name, enabled, files:[], backups:{} } } }
//   mods/<slug>/payload/ the extracted mod files
//   backups/<slug>/      originals overwritten by this mod
//
// Usage:
//   node modmanager.mjs config [--game <path>]      show / set game path
//   node modmanager.mjs list
//   node modmanager.mjs install <dir|zip|tar.gz> [--name X]
//   node modmanager.mjs enable <slug>
//   node modmanager.mjs disable <slug>
//   node modmanager.mjs uninstall <slug>

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DATA = process.env.RE2R_MP_DATA || path.join(HERE, 'data');
const CONFIG = path.join(DATA, 'config.json');
const REGISTRY = path.join(DATA, 'registry.json');

const log = (...a) => console.log(...a);
const die = (m) => { console.error('error: ' + m); process.exit(1); };

function readJSON(p, dflt) {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch { return dflt; }
}
function writeJSON(p, v) {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, JSON.stringify(v, null, 2));
}
const config = () => readJSON(CONFIG, {});
const registry = () => readJSON(REGISTRY, { mods: {} });

function gamePath() {
  const c = config();
  if (c.gamePath && fs.existsSync(c.gamePath)) return c.gamePath;
  const auto = detectGame();
  if (auto) { c.gamePath = auto; writeJSON(CONFIG, c); log('detected game: ' + auto); return auto; }
  die('game path not set. Run: node modmanager.mjs config --game "C:\\path\\to\\RESIDENT EVIL 2  BIOHAZARD RE2"');
}

function detectGame() {
  // Try Steam libraries, best effort.
  const roots = [];
  try {
    const vdf = fs.readFileSync('C:\\Program Files (x86)\\Steam\\steamapps\\libraryfolders.vdf', 'utf8');
    for (const m of vdf.matchAll(/"path"\s+"([^"]+)"/g)) roots.push(m[1].replace(/\\\\/g, '\\'));
  } catch {}
  const names = ['RESIDENT EVIL 2  BIOHAZARD RE2', 'RESIDENT EVIL 2 BIOHAZARD RE2', 'RESIDENT EVIL 2'];
  for (const r of roots) {
    for (const n of names) {
      const p = path.join(r, 'steamapps', 'common', n);
      if (fs.existsSync(path.join(p, 're2.exe'))) return p;
    }
  }
  return null;
}

function slugify(s) {
  return s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 60) || 'mod';
}

function extract(archive, dest) {
  fs.mkdirSync(dest, { recursive: true });
  if (fs.statSync(archive).isDirectory()) {
    fs.cpSync(archive, dest, { recursive: true });
    return;
  }
  const lower = archive.toLowerCase();
  if (lower.endsWith('.zip')) {
    const r = spawnSync('powershell', ['-NoProfile', '-Command',
      `Expand-Archive -LiteralPath '${archive}' -DestinationPath '${dest}' -Force`], { stdio: 'inherit' });
    if (r.status !== 0) die('Expand-Archive failed');
  } else if (lower.endsWith('.tar') || lower.endsWith('.tar.gz') || lower.endsWith('.tgz')) {
    const r = spawnSync('tar', ['-xf', archive, '-C', dest], { stdio: 'inherit' });
    if (r.status !== 0) die('tar failed');
  } else if (lower.endsWith('.rar')) {
    die('.rar not supported by this manager; extract first and pass the folder (WinRAR/7-Zip).');
  } else {
    die('unsupported archive type: ' + archive);
  }
}

// collapse a single wrapping directory if the archive has one, and locate the mod root.
function normalizePayload(dir) {
  let entries = fs.readdirSync(dir);
  while (entries.length === 1 && fs.statSync(path.join(dir, entries[0])).isDirectory()) {
    const inner = path.join(dir, entries[0]);
    for (const e of fs.readdirSync(inner)) fs.renameSync(path.join(inner, e), path.join(dir, e));
    fs.rmdirSync(inner);
    entries = fs.readdirSync(dir);
  }
  return dir;
}

function modRootOf(payload) {
  // Prefer a nested reframework/ or natives/ tree; otherwise the payload root itself.
  for (const sub of ['reframework', 'natives']) {
    if (fs.existsSync(path.join(payload, sub))) return payload;
  }
  // maybe payload/SomeMod/reframework
  for (const e of fs.readdirSync(payload)) {
    const p = path.join(payload, e);
    if (fs.statSync(p).isDirectory() &&
        (fs.existsSync(path.join(p, 'reframework')) || fs.existsSync(path.join(p, 'natives')))) return p;
  }
  return payload;
}

function walk(root) {
  const out = [];
  const stack = [''];
  while (stack.length) {
    const rel = stack.pop();
    for (const e of fs.readdirSync(path.join(root, rel))) {
      const r = path.join(rel, e);
      const abs = path.join(root, r);
      const st = fs.lstatSync(abs);
      if (st.isDirectory()) stack.push(r);
      else out.push(r);
    }
  }
  return out;
}

const cmd = process.argv[2];
const args = process.argv.slice(3);
const opt = (n) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : undefined; };
const pos = args.filter((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')));

if (cmd === 'config') {
  const c = config();
  const g = opt('--game');
  if (g) { c.gamePath = g; writeJSON(CONFIG, c); log('game path set: ' + g); }
  else log('game path: ' + (c.gamePath || '(unset)') + (detectGame() ? '\ndetected: ' + detectGame() : ''));
} else if (cmd === 'list') {
  const reg = registry();
  const mods = Object.entries(reg.mods);
  if (!mods.length) log('(no mods installed)');
  for (const [slug, m] of mods) log(`${m.enabled ? '[x]' : '[ ]'} ${slug}  (${m.name})  files=${m.files.length}`);
} else if (cmd === 'install') {
  const src = pos[0];
  if (!src || !fs.existsSync(src)) die('usage: install <dir|zip|tar.gz>');
  const name = opt('--name') || path.basename(src).replace(/\.(zip|tar\.gz|tgz|tar)$/i, '');
  const slug = slugify(name);
  const reg = registry();
  if (reg.mods[slug]) die('mod already installed: ' + slug);
  const payload = path.join(DATA, 'mods', slug, 'payload');
  fs.rmSync(path.join(DATA, 'mods', slug), { recursive: true, force: true });
  extract(src, payload);
  normalizePayload(payload);
  const root = modRootOf(payload);
  reg.mods[slug] = { name, enabled: false, root: path.relative(path.join(DATA, 'mods', slug), root), files: [], backups: {} };
  writeJSON(REGISTRY, reg);
  log(`installed '${name}' as '${slug}'. enable with: node modmanager.mjs enable ${slug}`);
} else if (cmd === 'enable') {
  const slug = pos[0];
  const reg = registry();
  const m = reg.mods[slug];
  if (!m) die('no such mod: ' + slug);
  if (m.enabled) { log('already enabled'); process.exit(0); }
  const game = gamePath();
  const root = path.join(DATA, 'mods', slug, m.root || 'payload');
  const files = walk(root);
  for (const rel of files) {
    const from = path.join(root, rel);
    const to = path.join(game, rel);
    const bak = path.join(DATA, 'backups', slug, rel);
    if (fs.existsSync(to)) { fs.mkdirSync(path.dirname(bak), { recursive: true }); fs.copyFileSync(to, bak); m.backups[rel] = true; }
    fs.mkdirSync(path.dirname(to), { recursive: true });
    fs.copyFileSync(from, to);
  }
  m.files = files; m.enabled = true;
  writeJSON(REGISTRY, reg);
  log(`enabled '${slug}' (${files.length} files)`);
} else if (cmd === 'disable') {
  const slug = pos[0];
  const reg = registry();
  const m = reg.mods[slug];
  if (!m) die('no such mod: ' + slug);
  if (!m.enabled) { log('not enabled'); process.exit(0); }
  const game = gamePath();
  for (const rel of m.files) {
    const to = path.join(game, rel);
    const bak = path.join(DATA, 'backups', slug, rel);
    if (m.backups[rel] && fs.existsSync(bak)) fs.copyFileSync(bak, to);
    else fs.rmSync(to, { force: true });
  }
  fs.rmSync(path.join(DATA, 'backups', slug), { recursive: true, force: true });
  m.files = []; m.backups = {}; m.enabled = false;
  writeJSON(REGISTRY, reg);
  log(`disabled '${slug}'`);
} else if (cmd === 'uninstall') {
  const slug = pos[0];
  const reg = registry();
  const m = reg.mods[slug];
  if (!m) die('no such mod: ' + slug);
  if (m.enabled) { spawnSync(process.execPath, [fileURLToPath(import.meta.url), 'disable', slug], { stdio: 'inherit' }); }
  fs.rmSync(path.join(DATA, 'mods', slug), { recursive: true, force: true });
  const reg2 = registry(); delete reg2.mods[slug]; writeJSON(REGISTRY, reg2);
  log(`uninstalled '${slug}'`);
} else {
  log(`re2r_mp mod manager
usage:
  node modmanager.mjs config [--game <path>]
  node modmanager.mjs list
  node modmanager.mjs install <dir|zip|tar.gz> [--name X]
  node modmanager.mjs enable <slug>
  node modmanager.mjs disable <slug>
  node modmanager.mjs uninstall <slug>`);
}
