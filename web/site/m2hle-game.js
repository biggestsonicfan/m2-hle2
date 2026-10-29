/*
 * Which game this page plays. Loaded after m2hle-games.js, which picked it
 * from the address (?game=ID&profile=PROFILE, Pinboard #881); the key and pad
 * panels read their row labels from this, and the page script its arguments
 * and ROM text. The profile reaches main_web.c as --profile=ID; ?wheel=range,speed
 * (or ?wheel=range) adjusts Sega Rally's steering pot (src/board/io_5649.h; the
 * default is the operator manual's spec). The page's language picks the board's
 * country, --region= (Pinboard #942): English USA, Japanese Japan, any other
 * language Export, unless the site profile fixes one (m2hle-games.js `region`,
 * Fighting Vipers' Japan on the English page, Pinboard #1350). Every game takes
 * it; a profile says what it does with it (game_quirks_t.region,
 * game_region_use_t in src/core/game_profile.h).
 */
'use strict';

const m2hleGame = (() => {
  const params = new URLSearchParams(location.search);
  /* index.html sends a visit with no game to the front page before this runs;
   * Sonic the Fighters only if that did not happen yet. */
  const picked = m2hleGames.picked || { id: 'sfight', profile: 'sfight_console' };
  const id = picked.id;
  const game = Object.assign({ id, profile: picked.profile }, m2hleGames.text(id),
                             { args: ['--profile=' + picked.profile] });
  game.args.push('--region=' + (picked.region || { en: 'usa', ja: 'japan' }[m2hleLang.asked] || 'export'));

  if (id === 'srallyc') {
    const w = (params.get('wheel') || '').split(',').map((v) => v.trim());
    if (/^(0x[0-9a-f]+|\d+)$/i.test(w[0] || '')) game.args.push('--wheel-range=' + w[0]);
    if (/^(0x[0-9a-f]+|\d+)$/i.test(w[1] || '')) game.args.push('--wheel-speed=' + w[1]);
  }

  /* The page's own words for the game (index.html's are the whole arcade's),
   * in the visit's language (m2hle-lang.js). */
  document.title = game.title;
  const head = document.querySelector('#panel .ps-title');
  if (head) head.textContent = game.title;
  const hint = document.querySelector('#drop .hint');
  if (hint) hint.innerHTML = game.romHint;
  const desc = document.querySelector('meta[name="description"]');
  if (desc) desc.content = m2hleLang.t('game.desc', { title: game.title });
  const all = document.getElementById('all-games');
  if (all) all.href = 'home.html' + m2hleLang.query();

  /* The arcade (arcade/stf-arcade.mjs): a shared board per game, and whether this
   * browser keeps the game's own. ?game= tells it which board and which words: Sega
   * Rally's own, not the Fly's, which are Sonic the Fighters'. Named with this
   * script's own ?v=, as index.html names everything. A game the arcade keeps no
   * board for (arcade: false) runs without it, on the browser's own save. */
  if (document.currentScript && game.arcade !== false) {
    const s = document.createElement('script');
    const q = new URLSearchParams(new URL(document.currentScript.src).search);
    q.set('game', id);
    q.set('profile', picked.profile);    /* a game's board may fit only some of its sets */
    s.type = 'module';
    s.src = 'arcade/stf-arcade.mjs?' + q;
    document.head.append(s);
  }

  return game;
})();
