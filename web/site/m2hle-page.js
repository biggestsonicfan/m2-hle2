/*
 * The page's half of the web build. Loaded BEFORE m2hle.js, because Emscripten
 * reads the global `Module` as it starts.
 *
 * What is here today is step 1 of the wizard (WEB-PORT.md section 5): hand the
 * emulator the player's own game file. The file is read in this page and passed
 * to the wasm heap; it is never sent anywhere.
 */
'use strict';

const $ = (id) => document.getElementById(id);
const steps = ['step-loading', 'step-rom', 'step-busy', 'step-fatal'];

function show(step) {
  for (const s of steps) $(s).hidden = s !== step;
  $('panel').hidden = false;
}

function fatal(text) {
  $('fatal-text').textContent = text;
  show('step-fatal');
}

/* What a failed load means, in words a player can act on. The emulator matches
 * files by checksum, so it knows exactly which ones the zip does not hold. */
function explainMissing(missing) {
  const names = missing.split(' ').filter(Boolean);
  if (names.length === 0) {
    return 'That file could not be read as a zip. Choose your merged sfight.zip or schamp.zip.';
  }
  const first = names[0];
  if (/^epr-1900[12]\./.test(first)) {
    return 'This zip does not have the Sonic the Fighters program files (' + first + '). ' +
           'It looks like a split "schamp" set. You need a merged set: one zip that holds ' +
           'both sfight and schamp.';
  }
  return 'This zip is missing ' + first + ', which the game needs. It looks like a split ' +
         '"sfight" set, which keeps those files in a separate schamp.zip. You need a merged ' +
         'set: one zip that holds both.';
}

function loadZip(bytes) {
  show('step-busy');
  /* Let the "reading" text paint before the (synchronous) extraction runs. */
  requestAnimationFrame(() => setTimeout(() => {
    const ptr = Module._malloc(bytes.length);
    if (!ptr) { romError('Not enough memory to read that file.'); return; }
    Module.HEAPU8.set(bytes, ptr);           /* HEAPU8 read AFTER malloc: growth replaces the view */
    const rc = Module._web_rom_load(ptr, bytes.length);   /* takes ownership of ptr */
    if (rc !== 0) {
      romError(explainMissing(Module.UTF8ToString(Module._web_rom_missing())));
      return;
    }
    $('panel').hidden = true;
    $('keys').hidden = false;
    $('canvas').focus();
  }, 0));
}

function romError(text) {
  $('rom-error').textContent = text;
  $('rom-error').hidden = false;
  show('step-rom');
}

function readFile(file) {
  if (!file) return;
  $('rom-error').hidden = true;
  file.arrayBuffer().then((buf) => loadZip(new Uint8Array(buf)),
                          () => romError('That file could not be read.'));
}

/* ---- Emscripten ------------------------------------------------------------ */

var Module = {
  canvas: $('canvas'),
  print: (t) => console.log(t),
  printErr: (t) => console.warn(t),
  onAbort: (what) => fatal(String(what)),

  /* Called from main_web.c's init(), once the exports can be used. */
  onM2hleReady() {
    show('step-rom');
    /* Development only: ?rom=<path> loads a zip from THIS site, so a headless
     * browser can boot the game with no file dialog. A production site hosts no
     * ROMs, so there is nothing for it to find; other origins are refused. */
    const dev = new URLSearchParams(location.search).get('rom');
    if (dev) {
      const url = new URL(dev, location.href);
      if (url.origin === location.origin) {
        fetch(url).then((r) => r.ok ? r.arrayBuffer() : Promise.reject(new Error(r.status)))
                  .then((buf) => loadZip(new Uint8Array(buf)))
                  .catch((e) => romError('Could not fetch ' + url.pathname + ' (' + e.message + ').'));
      }
    }
  },
};

if (typeof WebAssembly !== 'object') {
  fatal('It does not support WebAssembly.');
}

/* ---- The file picker and the drop zone ------------------------------------- */

$('file').addEventListener('change', (e) => readFile(e.target.files[0]));

const drop = $('drop');
for (const type of ['dragenter', 'dragover']) {
  window.addEventListener(type, (e) => { e.preventDefault(); drop.classList.add('over'); });
}
for (const type of ['dragleave', 'drop']) {
  window.addEventListener(type, (e) => { e.preventDefault(); drop.classList.remove('over'); });
}
window.addEventListener('drop', (e) => {
  if ($('step-rom').hidden) return;
  readFile(e.dataTransfer && e.dataTransfer.files[0]);
});
