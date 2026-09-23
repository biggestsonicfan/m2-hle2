/*
 * m2hle-shader.js -- the Picture tab: a filter over the game's 4:3 picture
 * (src/ui/post_shader.h). Either the built-in CRT (Lost Judgment's, as YAMP
 * ports it) or a libretro GLSL shader the player supplies.
 *
 * The emulator has no file system, so a custom shader arrives as files: the
 * player picks a preset with its shaders and textures (or a whole folder), this
 * hands the ones the preset names to web_shader_file_add, and web_shader_load
 * resolves the preset against them. After a load the emulator reports which
 * files it read (web_shader_used), and only those are kept -- in IndexedDB,
 * because a preset's textures are too big for localStorage -- so the shader is
 * back on the next visit without the whole folder being stored.
 *
 * Stored, per browser: the filter (localStorage m2hle.shaderMode), the custom
 * shader's files (IndexedDB m2hle-shader), its parameter values
 * (localStorage m2hle.shaderParams, by preset name).
 */
'use strict';

const m2hleShader = (() => {
  const $ = (id) => document.getElementById(id);
  const MODE_OFF = 0, MODE_CRT = 1, MODE_CUSTOM = 2;
  const KEY_MODE = 'm2hle.shaderMode', KEY_PARAMS = 'm2hle.shaderParams';
  const DB_NAME = 'm2hle-shader', DB_STORE = 'preset', DB_KEY = 'current';
  const TEXT = /\.(glslp|glsl|inc|h)$/i;
  const IMAGE = /\.(png|jpe?g|bmp|tga)$/i;
  const PRESET = /\.(glslp|glsl)$/i;
  const SLANG = /\.(slangp|slang)$/i;

  let ready = false;
  let picked = [];          /* [{ path, file }] from the last pick */
  let loadedName = '';
  let saveTimer = 0;

  /* ---- Small helpers ------------------------------------------------------------ */

  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } },
  };

  function withString(s, fn) {
    const n = Module.lengthBytesUTF8(s) + 1, p = Module._malloc(n);
    try {
      Module.stringToUTF8(s, p, n);
      return fn(p);
    } finally {
      Module._free(p);
    }
  }

  function addFile(path, bytes) {
    const d = Module._malloc(Math.max(1, bytes.length));
    try {
      Module.HEAPU8.set(bytes, d);           /* HEAPU8 after malloc: growth replaces the view */
      return withString(path, (p) => Module._web_shader_file_add(p, d, bytes.length));
    } finally {
      Module._free(d);
    }
  }

  function status(text, kind) {
    const s = $('shader-status');
    s.textContent = text || '';
    s.className = 'status' + (kind ? ' shader-st-' + kind : '');
  }

  function base(path) { return path.slice(path.lastIndexOf('/') + 1); }

  /* ---- IndexedDB, for the files of the shader in use ------------------------------ */

  function idb() {
    return new Promise((resolve, reject) => {
      if (!window.indexedDB) { reject(new Error('no IndexedDB')); return; }
      const r = indexedDB.open(DB_NAME, 1);
      r.onupgradeneeded = () => r.result.createObjectStore(DB_STORE);
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
  }

  async function idbDo(mode, fn) {
    const db = await idb();
    try {
      return await new Promise((resolve, reject) => {
        const tx = db.transaction(DB_STORE, mode);
        const req = fn(tx.objectStore(DB_STORE));
        tx.oncomplete = () => resolve(req && req.result);
        tx.onerror = () => reject(tx.error);
        tx.onabort = () => reject(tx.error);
      });
    } finally {
      db.close();
    }
  }

  const idbGet = () => idbDo('readonly', (s) => s.get(DB_KEY));
  const idbPut = (v) => idbDo('readwrite', (s) => s.put(v, DB_KEY));
  const idbDelete = () => idbDo('readwrite', (s) => s.delete(DB_KEY));

  /* ---- The filter ------------------------------------------------------------------ */

  function setMode(mode, remember = true) {
    if (mode === MODE_CUSTOM && !loadedName) mode = MODE_OFF;
    Module._web_shader_set_mode(mode);
    $('shader-mode').value = String(mode);
    if (remember) store.set(KEY_MODE, String(mode));
    $('shader-params-box').hidden = !(mode === MODE_CUSTOM && $('shader-params').childElementCount > 0);
  }

  function refreshCustom() {
    const opt = $('shader-mode-custom');
    opt.disabled = !loadedName;
    opt.textContent = loadedName ? 'Your shader: ' + loadedName : 'Your shader (load one below)';
    $('shader-forget').hidden = !loadedName;
  }

  /* ---- Parameters ---------------------------------------------------------------- */

  function savedParams() {
    try { return JSON.parse(store.get(KEY_PARAMS) || '{}') || {}; } catch (e) { return {}; }
  }

  function saveParamsSoon() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
      const all = savedParams(), mine = {};
      for (const p of readParams()) if (p.value !== p.initial) mine[p.name] = p.value;
      all[loadedName] = mine;
      store.set(KEY_PARAMS, JSON.stringify(all));
    }, 400);
  }

  function readParams() {
    try { return JSON.parse(Module.UTF8ToString(Module._web_shader_params())); } catch (e) { return []; }
  }

  function setParam(name, value) {
    withString(name, (p) => Module._web_shader_set_param(p, value));
  }

  const fmt = (v) => String(Math.round(v * 1000) / 1000);

  function buildParams() {
    const list = $('shader-params');
    list.textContent = '';
    for (const p of readParams()) {
      const row = document.createElement('label');
      row.className = 'shader-param';
      const name = document.createElement('span');
      name.textContent = p.desc;
      const input = document.createElement('input');
      input.type = 'range';
      input.min = String(p.min);
      input.max = String(p.max);
      input.step = String(p.step > 0 ? p.step : (p.max - p.min) / 100 || 'any');
      input.value = String(p.value);
      input.disabled = !(p.max > p.min);
      const out = document.createElement('output');
      out.textContent = fmt(p.value);
      input.addEventListener('input', () => {
        const v = Number(input.value);
        setParam(p.name, v);
        out.textContent = fmt(v);
        saveParamsSoon();
      });
      input.dataset.name = p.name;
      input.dataset.initial = String(p.initial);
      row.append(name, input, out);
      list.appendChild(row);
    }
    $('shader-params-box').hidden = !(Number($('shader-mode').value) === MODE_CUSTOM && list.childElementCount > 0);
  }

  function resetParams() {
    for (const input of $('shader-params').querySelectorAll('input[type=range]')) {
      input.value = input.dataset.initial;
      input.dispatchEvent(new Event('input'));
    }
  }

  /* ---- Loading ---------------------------------------------------------------------- */

  /* `files` is [{ path, bytes }]. Returns true when the shader is in use. */
  function loadInto(presetPath, files) {
    Module._web_shader_files_clear();
    for (const f of files) addFile(f.path, f.bytes);
    const ok = withString(presetPath, (p) => Module._web_shader_load(p));
    if (!ok) {
      status(Module.UTF8ToString(Module._web_shader_error()), 'bad');
      return false;
    }
    loadedName = Module.UTF8ToString(Module._web_shader_name());
    const mine = savedParams()[loadedName] || {};
    for (const [k, v] of Object.entries(mine)) setParam(k, Number(v));
    refreshCustom();
    buildParams();
    return true;
  }

  /* The paths a preset names, found by regex rather than a second parser: every
   * value that ends like a shader or an image, and every #reference. The C side
   * does the real resolving; this only decides which picked files to hand over. */
  function referencedNames(text) {
    const names = new Set();
    const re = /^\s*(?:#reference\s+|[\w.-]+\s*=\s*)"?([^"\r\n#;]+?\.(?:glslp|glsl|png|jpe?g|bmp|tga))"?\s*$/gim;
    let m;
    while ((m = re.exec(text)) !== null) names.add(base(m[1].trim().replace(/\\/g, '/')).toLowerCase());
    return names;
  }

  async function filesFor(presetPath) {
    const byName = new Map();
    for (const f of picked) {
      const k = base(f.path).toLowerCase();
      if (!byName.has(k)) byName.set(k, []);
      byName.get(k).push(f);
    }
    const chosen = new Map();     /* path -> picked file */
    const queue = [presetPath];
    const seen = new Set();
    while (queue.length) {
      const path = queue.shift();
      if (seen.has(path)) continue;
      seen.add(path);
      const entry = picked.find((f) => f.path === path);
      if (!entry) continue;
      chosen.set(entry.path, entry);
      if (!/\.glslp$/i.test(entry.path)) continue;
      const text = await entry.file.text();
      for (const name of referencedNames(text)) {
        for (const f of byName.get(name) || []) {
          chosen.set(f.path, f);
          if (/\.glslp$/i.test(f.path)) queue.push(f.path);
        }
      }
    }
    const out = [];
    for (const f of chosen.values()) out.push({ path: f.path, bytes: new Uint8Array(await f.file.arrayBuffer()) });
    return out;
  }

  async function usePreset(presetPath) {
    if (!presetPath) return;
    status('Loading ' + base(presetPath) + '…');
    let files;
    try {
      files = await filesFor(presetPath);
    } catch (e) {
      status('Could not read the files: ' + e.message, 'bad');
      return;
    }
    if (!loadInto(presetPath, files)) return;
    setMode(MODE_CUSTOM);
    status(loadedName + ' is on.', 'good');
    /* Keep what it read, and nothing else the pick carried. */
    const used = new Set(Module.UTF8ToString(Module._web_shader_used()).split('\n').filter(Boolean));
    const keep = files.filter((f) => used.has(f.path.replace(/\\/g, '/')) || used.has(normalize(f.path)));
    try {
      await idbPut({ preset: presetPath, name: loadedName, files: keep });
    } catch (e) {
      status(loadedName + ' is on, but this browser would not keep it for next time (' + e.message + ').', 'warn');
    }
  }

  /* The emulator's normalisation (rs_path_normalize), for matching its report. */
  function normalize(p) {
    const out = [];
    for (const part of p.replace(/\\/g, '/').split('/')) {
      if (part === '' || part === '.') continue;
      if (part === '..' && out.length && out[out.length - 1] !== '..') out.pop();
      else out.push(part);
    }
    return (p.startsWith('/') ? '/' : '') + out.join('/');
  }

  async function onPick(fileList) {
    picked = [];
    let slang = false;
    for (const file of fileList) {
      const path = (file.webkitRelativePath || file.name).replace(/\\/g, '/');
      if (SLANG.test(path)) slang = true;
      if (TEXT.test(path) || IMAGE.test(path)) picked.push({ path, file });
    }
    const presets = picked.filter((f) => PRESET.test(f.path))
      .sort((a, b) => (/\.glslp$/i.test(b.path) - /\.glslp$/i.test(a.path)) || a.path.localeCompare(b.path));
    const sel = $('shader-preset');
    sel.textContent = '';
    if (!presets.length) {
      sel.hidden = true;
      status(slang ? 'Those are slang shaders (.slangp), which need a compiler a browser does not have. '
                     + 'Pick the GLSL version (.glslp or .glsl) from libretro\'s glsl-shaders.'
                   : 'No .glslp or .glsl file in what you picked.', 'bad');
      return;
    }
    /* One preset, or one .glsl and nothing else to choose between: use it. */
    const glslp = presets.filter((f) => /\.glslp$/i.test(f.path));
    if (presets.length === 1 || glslp.length === 1) {
      sel.hidden = true;
      await usePreset((glslp[0] || presets[0]).path);
      return;
    }
    const hint = document.createElement('option');
    hint.value = '';
    hint.textContent = 'Choose one of ' + presets.length + ' shaders…';
    sel.appendChild(hint);
    for (const f of presets) {
      const o = document.createElement('option');
      o.value = f.path;
      o.textContent = f.path;
      sel.appendChild(o);
    }
    sel.hidden = false;
    sel.focus();
    status('Pick which shader to use.');
  }

  async function forget() {
    Module._web_shader_unload();
    Module._web_shader_files_clear();
    loadedName = '';
    refreshCustom();
    buildParams();
    if (Number($('shader-mode').value) === MODE_CUSTOM) setMode(MODE_OFF);
    try { await idbDelete(); } catch (e) { /* nothing kept */ }
    status('Forgotten.');
  }

  /* On the next visit: the kept shader, then the filter that was on. */
  async function restore() {
    const mode = Number(store.get(KEY_MODE)) || MODE_OFF;
    let rec = null;
    try { rec = await idbGet(); } catch (e) { /* no IndexedDB here */ }
    if (rec && rec.preset && Array.isArray(rec.files)) {
      if (!loadInto(rec.preset, rec.files)) status('Your saved shader did not load: ' + $('shader-status').textContent, 'bad');
      else status(loadedName + ' is loaded.');
    }
    setMode(mode === MODE_CUSTOM && !loadedName ? MODE_OFF : mode, false);
  }

  /* ---- Wiring ---------------------------------------------------------------------------- */

  function init() {
    $('shader-mode').addEventListener('change', (e) => setMode(Number(e.target.value)));
    $('shader-files').addEventListener('click', () => $('shader-file-input').click());
    $('shader-folder').addEventListener('click', () => $('shader-folder-input').click());
    for (const id of ['shader-file-input', 'shader-folder-input']) {
      $(id).addEventListener('change', (e) => {
        if (!ready) return;
        onPick(e.target.files);
        e.target.value = '';
      });
    }
    $('shader-preset').addEventListener('change', (e) => usePreset(e.target.value));
    $('shader-forget').addEventListener('click', forget);
    $('shader-reset').addEventListener('click', resetParams);
    /* A folder pick needs webkitdirectory; where it is missing, only the files button shows. */
    if (!('webkitdirectory' in document.createElement('input'))) $('shader-folder').hidden = true;
    refreshCustom();
  }

  /* Called from Module.onM2hleReady: the exports and the GL context exist now. */
  function onReady() {
    ready = true;
    restore();
  }

  document.addEventListener('DOMContentLoaded', init);

  return { onReady, setMode };
})();
