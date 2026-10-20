/*
 * The front page's boxes (home.html), one per game in m2hle-games.js. The box
 * plays the game's first profile; under it, every variant, each its own link,
 * or greyed when the emulator has no profile for it yet.
 */
'use strict';

(() => {
  /* The address is the site's root, not home.html: index.html sent the visit
   * here only because it named no game, and the root is the link to share. */
  if (/\/home\.html$/.test(location.pathname)) history.replaceState(null, '', './' + m2hleLang.query());

  const make = (tag, props, ...kids) => {
    const el = Object.assign(document.createElement(tag), props);
    el.append(...kids);
    return el;
  };

  function box(id) {
    const g = m2hleGames.text(id);
    const main = make('a', { href: m2hleGames.href(id) },
      make('div', { className: 'marquee' }, g.title),
      make('div', { className: 'about' },
        make('div', { className: 'meta' }, g.year + ' · ' + g.board),
        make('p', {}, g.blurb)));
    main.querySelector('.marquee').style.cssText = `--c0:${g.colors[0]};--c1:${g.colors[1]}`;
    const profiles = make('div', { className: 'profiles' },
      ...g.profiles.map((p) => p.soon
        ? make('span', { className: 'soon', title: m2hleLang.t('home.soon', { about: p.about }) }, p.name)
        : make('a', { href: m2hleGames.href(id, p.id), title: p.about }, p.name)));
    return make('li', { className: 'game' }, main, profiles);
  }

  document.title = m2hleLang.t('home.title');
  const desc = document.querySelector('meta[name="description"]');
  if (desc) desc.content = m2hleLang.t('home.desc');

  const list = document.getElementById('games');
  for (const id of Object.keys(m2hleGames.GAMES)) list.append(box(id));

  /* The language switch: this page in each of the site's languages. */
  const langs = document.getElementById('langs');
  if (langs) {
    for (const [code, l] of Object.entries(m2hleLang.LANGS)) {
      const q = new URLSearchParams(code === m2hleLang.DEFAULT ? {} : { lang: code });
      const s = q.toString();
      langs.append(' ', code === m2hleLang.lang
        ? make('strong', { lang: code }, l.name)
        : make('a', { href: './' + (s ? '?' + s : ''), lang: code, hreflang: code }, l.name));
    }
  }
})();
