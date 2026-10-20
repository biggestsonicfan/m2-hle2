/*
 * The page's tools drawer: the emulator's log, and a check for what is making the
 * game feel slow. Loaded before m2hle-page.js, which wires it into `Module`.
 *
 * CONSOLE. Every line m2-hle prints goes through Module.print / printErr, and the
 * emulator already says what each one is: log.h prefixes [INFO] [WARN] [ERR ]
 * [DBG ], and sokol prefixes [sg][error] and the like. Those become the three
 * filters -- General, Warnings, Errors -- and what Copy puts on the clipboard is
 * what the filters show, under a header that says which build, browser and GPU it
 * came from, because that is the first thing anyone reading a pasted log asks.
 *
 * LAG CHECK. "Lag" is one word for several different problems that need opposite
 * advice, so this measures instead of guessing blind: the emulator keeps running
 * totals of where each frame's time goes (main_web.c, g_web_perf), this reads
 * them twice a few seconds apart, and the findings are reasoned from the
 * difference. It can tell apart: the CPU not keeping up with the emulation; the
 * browser drawing too few frames (the GPU, a software renderer, a throttled or
 * 30 Hz display); single long frames; sound dropping out or a slow audio device;
 * and waiting on the other player. When the evidence points at the GPU it can
 * act on it, by drawing the game at a lower resolution (web_set_render_scale).
 * When the CPU is short it can offer the lighter sound driver (web_set_sound_driver).
 * When it finds nothing it says so, with the numbers, and says what is left.
 */
'use strict';

/* The lag check's words (Pinboard #944). What goes to the log stays English. */
m2hleLang.add({
  en: {
    'tools.copied': 'Copied',
    'tools.copyFailed': 'Copy failed',
    'tools.lag.notRunning': 'Start the game first: there is nothing to measure until it is running.',
    'tools.lag.measuring': 'Measuring. Keep playing... {s}',
    'tools.lag.checked': 'Checked {s} seconds of play.',
    'tools.lag.again': 'Check again',
    'tools.lag.done': 'Done. Run the check again to compare.',
    'tools.action.lowerRes': 'Draw the game at lower resolution',
    'tools.action.lighterSound': 'Use the lighter sound driver',
    'tools.tip.sound': 'The button below swaps the emulated sound CPU for a lighter driver: up to a tenth less work per frame, and the music sounds the same.',
    'tools.tip.closeCpu': 'Close other tabs and programs, especially anything else using the CPU.',
    'tools.tip.chargerCpu': 'On a laptop, plug in the charger and turn off battery saver: both slow the CPU down.',
    'tools.tip.chrome': 'Chrome and Edge run this kind of code fastest.',
    'tools.tip.close': 'Close other tabs and programs.',
    'tools.tip.charger': 'On a laptop, plug in the charger and turn off battery saver.',
    'tools.hidden.title': 'This tab was in the background during the check',
    'tools.hidden.evidence': 'Browsers all but stop a tab nobody is looking at, so these numbers describe that, not the game.',
    'tools.hidden.tip': 'Run the check again with the game in front, while playing.',
    'tools.cpuSlow.title': 'This computer is not keeping up with the emulation',
    'tools.cpuSlow.evidence': 'The game ran at {speed} of 60 frames a second. Emulating one frame took {slice} ms on average (worst {sliceMax}) and preparing the picture {render} ms; together they have to fit in 16.7.',
    'tools.cpuTight.title': 'This computer is only just keeping up',
    'tools.cpuTight.evidence': 'The game is at full speed ({speed}), but emulating a frame takes {slice} ms and the picture {render} ms of the 16.7 available, so anything else the computer does will cost frames.',
    'tools.turns.title': 'The browser is not giving the game enough turns to run',
    'tools.turns.evidence': 'The game ran at {speed} of 60 frames a second, but the emulator itself was only busy {busy}% of the time. The page was called {rate} times a second, and the game can catch up at most three frames per call.',
    'tools.turns.tip': 'Something outside the emulator is holding the page up: see the findings below.',
    'tools.soft.title': 'The browser is drawing in software: graphics acceleration is off',
    'tools.soft.evidence': 'It reports its renderer as "{renderer}", and drew {rate} frames a second.',
    'tools.soft.tip.accel': 'Turn on "Use graphics acceleration when available" in the browser\'s settings (System), then restart it.',
    'tools.soft.tip.driver': 'If it is already on, the graphics driver may be blocked or out of date: update it.',
    'tools.gpu.title': 'The graphics card is the bottleneck',
    'tools.gpu.evidence': 'Each frame took the GPU {gpu} ms, at {w}x{h} ({mp} million pixels). It has 16.7 ms for 60 frames a second.',
    'tools.gpu.tip': 'Make the browser window smaller, or use the button below.',
    'tools.half.title': 'The picture is being drawn at 30 frames a second',
    'tools.half.evidence.slow': 'The browser called the page {rate} times a second. Exactly half of 60 usually means something is rationing frames rather than struggling.',
    'tools.half.evidence': 'The browser called the page {rate} times a second. The game itself still ran at full speed ({speed}), so it plays correctly but looks choppy. Exactly half of 60 usually means something is rationing frames rather than struggling.',
    'tools.half.tip.power': 'Turn off battery saver / low power mode, and plug in the charger.',
    'tools.half.tip.hz': 'Check the display is set to 60 Hz or more, not 30.',
    'tools.half.tip.big': 'If the window is large or the screen is 4K, the GPU may be settling on every other frame: try the button below.',
    'tools.few.title': 'The browser is drawing too few frames',
    'tools.few.evidence': 'It drew {rate} frames a second while the emulator needed only {need} ms of each one. Canvas: {w}x{h} ({mp} million pixels), renderer "{renderer}".',
    'tools.few.evidence.noGpu': 'It drew {rate} frames a second while the emulator needed only {need} ms of each one; this browser does not let the page time the GPU, so this is an inference. Canvas: {w}x{h} ({mp} million pixels), renderer "{renderer}".',
    'tools.few.tip.smaller': 'Make the window smaller, or use the button below: if that fixes it, it was the GPU.',
    'tools.few.tip.video': 'Close other tabs showing video or animation.',
    'tools.softOk.title': 'The browser is drawing in software',
    'tools.softOk.evidence': 'Renderer: "{renderer}". It is keeping up right now, but it is using the CPU to do the GPU\'s job.',
    'tools.softOk.tip': 'Turn on graphics acceleration in the browser\'s settings and restart it.',
    'tools.stutter.title': 'Stutter: some frames arrived late',
    'tools.stutter.ours': '{n} frames in {s} s came more than 25 ms after the one before (worst gap {gap} ms). The emulator had slow frames of its own (worst emulation {slice} ms, worst picture {render} ms), which is normal while the game loads a new scene and not in the middle of a fight.',
    'tools.stutter.outside': '{n} frames in {s} s came more than 25 ms after the one before (worst gap {gap} ms). The emulator\'s own work never took longer than {worst} ms, so the pauses came from outside it: the browser, other tabs, or the system.',
    'tools.stutter.tip.fight': 'If it happens mid-fight, run the check again then and copy the report.',
    'tools.stutter.tip.ext': 'Browser extensions that touch every page can do this: try a private window.',
    'tools.dropout.one.title': 'The sound dropped out {n} time',
    'tools.dropout.many.title': 'The sound dropped out {n} times',
    'tools.dropout.cpu': 'The audio queue ran empty (it holds about {ms} ms). The sound is made a frame at a time, so this follows from the slow frames above rather than being a problem of its own.',
    'tools.dropout.late': 'The audio queue ran empty (it holds about {ms} ms). The sound is made a frame at a time, so this follows from late frames rather than being a problem of its own.',
    'tools.dropout.tip.cpu': 'It will stop when the game runs at full speed.',
    'tools.dropout.tip.late': 'Fix the late frames first. If the sound still breaks up, add ?audioms=80 to the address for a deeper queue (sound will be slightly later).',
    'tools.device.title': 'Your audio device adds {ms} ms of delay',
    'tools.device.evidence': 'That is the browser\'s own report for the output device, on top of the {queue} ms this page queues. Bluetooth headphones and speakers are the usual cause.',
    'tools.device.tip': 'Use wired headphones or the computer\'s speakers if sound behind the picture bothers you.',
    'tools.noisy.title': 'The emulator is complaining {n} times a second',
    'tools.noisy.evidence': 'It logged {n} warnings or errors in {s} s. Each one costs a little time; more to the point, it means the game is doing something the emulator does not handle yet, and what follows may be drawn or played wrong.',
    'tools.noisy.evidence.worst': 'It logged {n} warnings or errors in {s} s, most often: "{worst}". Each one costs a little time; more to the point, it means the game is doing something the emulator does not handle yet, and what follows may be drawn or played wrong.',
    'tools.noisy.tip': 'Open the Console tab, press Copy, and send it with a note of what was on screen.',
    'tools.wait.title': 'Waiting for the other player',
    'tools.wait.one': 'The game paused {n} time in {s} s because the other player\'s input had not arrived (frame delay {delay}).',
    'tools.wait.many': 'The game paused {n} times in {s} s because the other player\'s input had not arrived (frame delay {delay}).',
    'tools.wait.tip.delay': 'A higher frame delay absorbs a slower connection, at the cost of slightly later controls.',
    'tools.wait.tip.wifi': 'Wi-Fi on either side is the usual cause: a cable helps more than anything else.',
    'tools.ok.title': 'Nothing here is running slowly',
    'tools.ok.evidence': 'The game ran at {speed} of 60 frames a second and the browser drew {rate}. Emulation took {slice} ms a frame and the picture {render} ms, of 16.7 available. {late} late frames; sound queue {queue} ms.',
    'tools.ok.evidence.gpu': 'The game ran at {speed} of 60 frames a second and the browser drew {rate}. Emulation took {slice} ms a frame and the picture {render} ms, of 16.7 available; the GPU took {gpu} ms. {late} late frames; sound queue {queue} ms.',
    'tools.ok.tip.outside': 'If the controls still feel late, the delay is outside this page: a TV not in game mode, a wireless keyboard or controller, or the display itself.',
    'tools.ok.tip.again': 'If it only happens sometimes, run this check again while it is happening.',
    'tools.sound.nextMatch': 'The new sound driver starts with the next match.',
    'tools.sound.none': 'This game has no lighter sound driver: it keeps the sound CPU.',
    'tools.sound.changed': 'Sound driver changed. The music comes back with the game\'s next tune.',
  },
  ja: {
    'tools.copied': 'コピーしました',
    'tools.copyFailed': 'コピーできませんでした',
    'tools.lag.notRunning': '先にゲームを始めてください。動いていないと測れません。',
    'tools.lag.measuring': '計測中。そのまま遊んでください… {s}',
    'tools.lag.checked': '{s} 秒間のプレイを調べました。',
    'tools.lag.again': 'もう一度チェック',
    'tools.lag.done': '完了。もう一度チェックして比べてください。',
    'tools.action.lowerRes': 'ゲームを低い解像度で描く',
    'tools.action.lighterSound': '軽いサウンドドライバーを使う',
    'tools.tip.sound': '下のボタンで、エミュレートしたサウンドCPUを軽いドライバーに切り替えます。1フレームの処理が最大で1割ほど減り、音楽は変わりません。',
    'tools.tip.closeCpu': 'ほかのタブやプログラム、特にCPUを使っているものを閉じてください。',
    'tools.tip.chargerCpu': 'ノートパソコンなら充電器をつなぎ、省電力モードを切ってください。どちらもCPUを遅くします。',
    'tools.tip.chrome': 'この種の処理は Chrome と Edge がいちばん速く動きます。',
    'tools.tip.close': 'ほかのタブやプログラムを閉じてください。',
    'tools.tip.charger': 'ノートパソコンなら充電器をつなぎ、省電力モードを切ってください。',
    'tools.hidden.title': 'チェック中、このタブは裏に回っていました',
    'tools.hidden.evidence': 'ブラウザは見られていないタブをほぼ止めてしまうため、この数値はゲームではなくその状態を表しています。',
    'tools.hidden.tip': 'ゲームを前面に出し、遊びながらもう一度チェックしてください。',
    'tools.cpuSlow.title': 'このコンピューターはエミュレーションに追いついていません',
    'tools.cpuSlow.evidence': 'ゲームは秒間60フレーム中 {speed} フレームで動きました。1フレームのエミュレーションに平均 {slice} ms（最大 {sliceMax}）、画面の準備に {render} ms かかりました。合わせて 16.7 ms に収まる必要があります。',
    'tools.cpuTight.title': 'このコンピューターはぎりぎりで追いついています',
    'tools.cpuTight.evidence': 'ゲームは全速（{speed}）で動いていますが、使える 16.7 ms のうち、1フレームのエミュレーションに {slice} ms、画面に {render} ms かかっています。コンピューターがほかの処理をするとフレームを落とします。',
    'tools.turns.title': 'ブラウザがゲームに十分な実行の機会を与えていません',
    'tools.turns.evidence': 'ゲームは秒間60フレーム中 {speed} フレームで動きましたが、エミュレーター自体が働いていたのは {busy}% の時間だけでした。ページが呼ばれたのは秒間 {rate} 回で、1回につき追いつけるのは最大3フレームです。',
    'tools.turns.tip': 'エミュレーターの外の何かがページを止めています。下の項目を見てください。',
    'tools.soft.title': 'ブラウザがソフトウェアで描画しています（グラフィックアクセラレーションがオフ）',
    'tools.soft.evidence': 'レンダラーは「{renderer}」で、秒間 {rate} フレームを描きました。',
    'tools.soft.tip.accel': 'ブラウザの設定（システム）で「グラフィックアクセラレーションが使用可能な場合は使用する」をオンにし、ブラウザを再起動してください。',
    'tools.soft.tip.driver': 'すでにオンなら、グラフィックドライバーがブロックされているか古い可能性があります。更新してください。',
    'tools.gpu.title': 'グラフィックカードがボトルネックです',
    'tools.gpu.evidence': 'GPUは1フレームに {gpu} ms かかりました（{w}x{h}、{mp} 百万ピクセル）。秒間60フレームには 16.7 ms しかありません。',
    'tools.gpu.tip': 'ブラウザのウィンドウを小さくするか、下のボタンを使ってください。',
    'tools.half.title': '画面が秒間30フレームで描かれています',
    'tools.half.evidence.slow': 'ブラウザがページを呼んだのは秒間 {rate} 回です。60のちょうど半分なら、たいていは処理が追いつかないのではなく、何かがフレームを制限しています。',
    'tools.half.evidence': 'ブラウザがページを呼んだのは秒間 {rate} 回です。ゲーム自体は全速（{speed}）で動いているので正しく遊べますが、カクついて見えます。60のちょうど半分なら、たいていは処理が追いつかないのではなく、何かがフレームを制限しています。',
    'tools.half.tip.power': '省電力モードを切り、充電器をつないでください。',
    'tools.half.tip.hz': 'ディスプレイが30Hzではなく60Hz以上に設定されているか確認してください。',
    'tools.half.tip.big': 'ウィンドウが大きいか4K画面なら、GPUが1フレームおきの描画に落ち着いているのかもしれません。下のボタンを試してください。',
    'tools.few.title': 'ブラウザが描くフレームが少なすぎます',
    'tools.few.evidence': '秒間 {rate} フレームしか描かれませんでしたが、エミュレーターが1フレームに使ったのは {need} ms だけです。キャンバス：{w}x{h}（{mp} 百万ピクセル）、レンダラー「{renderer}」。',
    'tools.few.evidence.noGpu': '秒間 {rate} フレームしか描かれませんでしたが、エミュレーターが1フレームに使ったのは {need} ms だけです。このブラウザではページからGPUの時間を測れないため、これは推測です。キャンバス：{w}x{h}（{mp} 百万ピクセル）、レンダラー「{renderer}」。',
    'tools.few.tip.smaller': 'ウィンドウを小さくするか、下のボタンを使ってください。それで直ればGPUが原因です。',
    'tools.few.tip.video': '動画やアニメーションを表示しているほかのタブを閉じてください。',
    'tools.softOk.title': 'ブラウザがソフトウェアで描画しています',
    'tools.softOk.evidence': 'レンダラー：「{renderer}」。今は追いついていますが、GPUの仕事をCPUでしています。',
    'tools.softOk.tip': 'ブラウザの設定でグラフィックアクセラレーションをオンにし、再起動してください。',
    'tools.stutter.title': 'カクつき：遅れて届いたフレームがあります',
    'tools.stutter.ours': '{s} 秒間に {n} フレームが、前のフレームから 25 ms 以上遅れて届きました（最大の間隔 {gap} ms）。エミュレーター自体にも遅いフレームがありました（エミュレーション最大 {slice} ms、画面最大 {render} ms）。ゲームが新しい場面を読み込むときなら普通のことですが、対戦の最中なら普通ではありません。',
    'tools.stutter.outside': '{s} 秒間に {n} フレームが、前のフレームから 25 ms 以上遅れて届きました（最大の間隔 {gap} ms）。エミュレーター自体の処理は最大でも {worst} ms だったので、止まった原因はその外、ブラウザやほかのタブ、システムにあります。',
    'tools.stutter.tip.fight': '対戦中に起きるなら、そのときにもう一度チェックしてレポートをコピーしてください。',
    'tools.stutter.tip.ext': 'すべてのページに作用するブラウザ拡張機能が原因のこともあります。シークレットウィンドウで試してください。',
    'tools.dropout.one.title': '音が {n} 回途切れました',
    'tools.dropout.many.title': '音が {n} 回途切れました',
    'tools.dropout.cpu': '音声キュー（約 {ms} ms 分）が空になりました。音は1フレームずつ作られるので、これはそれ自体の問題ではなく、上の遅いフレームによるものです。',
    'tools.dropout.late': '音声キュー（約 {ms} ms 分）が空になりました。音は1フレームずつ作られるので、これはそれ自体の問題ではなく、遅れたフレームによるものです。',
    'tools.dropout.tip.cpu': 'ゲームが全速で動けば、音の途切れはなくなります。',
    'tools.dropout.tip.late': 'まず遅れるフレームを直してください。それでも音が途切れるなら、アドレスに ?audioms=80 を付けるとキューが深くなります（音は少し遅れます）。',
    'tools.device.title': 'オーディオ機器が {ms} ms の遅延を加えています',
    'tools.device.evidence': 'これはブラウザが出力機器について報告した値で、このページのキュー {queue} ms に上乗せされます。よくある原因は Bluetooth のヘッドホンやスピーカーです。',
    'tools.device.tip': '音が画面より遅れるのが気になるなら、有線のヘッドホンかコンピューターのスピーカーを使ってください。',
    'tools.noisy.title': 'エミュレーターが秒間 {n} 回警告を出しています',
    'tools.noisy.evidence': '{s} 秒間に警告やエラーが {n} 件記録されました。1件ごとに少し時間がかかりますが、それより問題なのは、ゲームがエミュレーター未対応のことをしているということで、この先の描画や音が正しくないかもしれません。',
    'tools.noisy.evidence.worst': '{s} 秒間に警告やエラーが {n} 件記録されました。いちばん多いのは「{worst}」です。1件ごとに少し時間がかかりますが、それより問題なのは、ゲームがエミュレーター未対応のことをしているということで、この先の描画や音が正しくないかもしれません。',
    'tools.noisy.tip': 'ログのタブを開いて「コピー」を押し、そのとき画面に何が出ていたかを添えて送ってください。',
    'tools.wait.title': '相手プレイヤーを待っています',
    'tools.wait.one': '相手プレイヤーの入力が届かず、{s} 秒間にゲームが {n} 回止まりました（入力遅延 {delay}）。',
    'tools.wait.many': '相手プレイヤーの入力が届かず、{s} 秒間にゲームが {n} 回止まりました（入力遅延 {delay}）。',
    'tools.wait.tip.delay': '入力遅延を大きくすると遅い回線を吸収できますが、操作が少し遅れます。',
    'tools.wait.tip.wifi': 'よくある原因はどちらかの Wi-Fi です。有線接続がいちばん効きます。',
    'tools.ok.title': '遅くなっているところはありません',
    'tools.ok.evidence': 'ゲームは秒間60フレーム中 {speed} フレームで動き、ブラウザは {rate} フレームを描きました。使える 16.7 ms のうち、エミュレーションは1フレーム {slice} ms、画面は {render} ms でした。遅れたフレーム {late}、音声キュー {queue} ms。',
    'tools.ok.evidence.gpu': 'ゲームは秒間60フレーム中 {speed} フレームで動き、ブラウザは {rate} フレームを描きました。使える 16.7 ms のうち、エミュレーションは1フレーム {slice} ms、画面は {render} ms、GPUは {gpu} ms でした。遅れたフレーム {late}、音声キュー {queue} ms。',
    'tools.ok.tip.outside': 'それでも操作が遅く感じるなら、遅延はこのページの外にあります。ゲームモードでないテレビ、ワイヤレスのキーボードやコントローラー、ディスプレイ自体などです。',
    'tools.ok.tip.again': 'ときどきしか起きないなら、起きているときにもう一度チェックしてください。',
    'tools.sound.nextMatch': '新しいサウンドドライバーは次の対戦から使われます。',
    'tools.sound.none': 'このゲームには軽いサウンドドライバーがありません。サウンドCPUのままです。',
    'tools.sound.changed': 'サウンドドライバーを変更しました。音楽はゲームの次の曲から戻ります。',
  },
});

const m2hleTools = (() => {
  const $ = (id) => document.getElementById(id);
  const t0 = performance.now();

  const tr = (key, vars) => m2hleLang.t(key, vars);
  /* A key's English, filled the same way: for the log, which is not translated. */
  const english = (key, vars) => m2hleLang.LANGS.en.strings[key]
    .replace(/\{(\w+)\}/g, (s, k) => (vars && k in vars ? vars[k] : s));
  let ready = false;            /* the emulator's exports exist, and its GL context */

  /* ---- Keys typed into the drawer stay in the drawer ---------------------------
   * sokol_app listens for keys on `window`, in the capture phase, and swallows
   * them -- so without this Ctrl+C in the log would reach the game and not the
   * clipboard. This listener is registered first (this script runs before
   * m2hle.js), so it runs first, and can keep the event from sokol entirely. */
  for (const type of ['keydown', 'keyup', 'keypress']) {
    window.addEventListener(type, (e) => {
      if (e.target instanceof Element && e.target.closest('#drawer, #online, #controls, input, textarea, select')) {
        e.stopImmediatePropagation();
      }
    }, true);
  }

  /* ---- The log ------------------------------------------------------------------ */

  const MAX_LINES = 3000, TRIM_CHUNK = 500;
  /* A line repeated within this long of its last appearance is folded into that
   * line as a count. The emulator warns every time it meets something it does not
   * handle, and when that is once a frame the log is four lines repeated two
   * hundred times -- unreadable, and slow to draw. Found the first time this
   * console was opened on a fight: an unimplemented COP command, 67 times. */
  const FOLD_MS = 5000;
  const recent = new Map();     /* text -> its line, while it can still be folded into */
  const events = { general: 0, warning: 0, error: 0 };   /* every occurrence, folded or not */
  const dirty = new Set();      /* drawn lines whose count changed */
  let redrawAll = false;
  const lines = [];             /* { t, level, text, n, row } */
  const counts = { general: 0, warning: 0, error: 0 };
  const shown = { general: true, warning: true, error: true };
  let rendered = 0;             /* lines[] index the DOM is drawn up to */
  let dropped = 0;

  function classify(text) {
    if (/\[ERR \]|\]\[(error|panic)\]|^(Uncaught|RuntimeError|Aborted|abort\()/.test(text)) return 'error';
    if (/\[WARN\]|\]\[warning\]/.test(text)) return 'warning';
    return 'general';
  }

  function add(text, level) {
    text = String(text);
    level = level || classify(text);
    events[level]++;
    const now = performance.now();
    const seen = recent.get(text);
    if (seen && now - seen.last < FOLD_MS) {
      seen.n++;
      seen.last = now;
      if (seen.row) dirty.add(seen);
      scheduleRender();
      return;
    }
    const line = { t: (now - t0) / 1000, level, text, n: 1, last: now, row: null };
    lines.push(line);
    recent.set(text, line);
    if (recent.size > 256) {
      for (const [k, v] of recent) if (now - v.last >= FOLD_MS) recent.delete(k);
      if (recent.size > 256) recent.clear();
    }
    counts[level]++;
    /* Trimmed in chunks, and the list redrawn whole when it is: cheaper than keeping
     * the DOM and the array in step one line at a time during a flood of warnings. */
    if (lines.length > MAX_LINES + TRIM_CHUNK) {
      for (const gone of lines.splice(0, TRIM_CHUNK)) counts[gone.level]--;
      dropped += TRIM_CHUNK;
      redrawAll = true;
    }
    scheduleRender();
  }

  /* The emulator's two streams. Mirrored to the browser's own console by level, so
   * devtools and tools/web-smoke.mjs see exactly what this shows. */
  function print(text) { add(text); console.log(text); }
  function printErr(text) {
    const level = classify(text);
    add(text, level);
    (level === 'error' ? console.error : level === 'warning' ? console.warn : console.log)(text);
  }

  window.addEventListener('error', (e) => add('Uncaught ' + (e.message || e.error), 'error'));
  window.addEventListener('unhandledrejection', (e) => add('Uncaught (in promise) ' + (e.reason && e.reason.message || e.reason), 'error'));

  const stamp = (l) => l.t.toFixed(1).padStart(7) + 's  ';
  const shownText = (l) => stamp(l) + l.text + (l.n > 1 ? '    (x' + l.n + ')' : '');
  let renderQueued = false;
  function scheduleRender() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }

  function render() {
    for (const k of ['general', 'warning', 'error']) {
      const el = $('count-' + k);
      if (el) el.textContent = counts[k];
    }
    const badge = $('console-badge');
    if (badge) {
      badge.textContent = counts.error || '';
      badge.hidden = counts.error === 0;
    }
    const log = $('log');
    if (!log || $('drawer').hidden || $('pane-console').hidden) return;   /* drawn on open */
    const stick = log.scrollHeight - log.scrollTop - log.clientHeight < 24;
    if (redrawAll) { log.textContent = ''; rendered = 0; redrawAll = false; for (const l of lines) l.row = null; }
    for (const l of dirty) if (l.row) l.row.textContent = shownText(l);
    dirty.clear();
    const frag = document.createDocumentFragment();
    for (; rendered < lines.length; rendered++) {
      const l = lines[rendered];
      if (!shown[l.level]) continue;
      const row = document.createElement('div');
      row.className = 'ln ln-' + l.level;
      row.textContent = shownText(l);
      l.row = row;
      frag.appendChild(row);
    }
    log.appendChild(frag);
    $('log-empty').hidden = log.childElementCount > 0;
    if (stick) log.scrollTop = log.scrollHeight;
  }

  function rerender() {
    const log = $('log');
    redrawAll = true;
    render();
    log.scrollTop = log.scrollHeight;
  }

  function systemInfo() {
    const g = gpuInfo();
    const c = $('canvas');
    return [
      'Build      ' + (document.documentElement.dataset.version || 'dev'),
      'When       ' + new Date().toISOString(),
      'Browser    ' + navigator.userAgent,
      'GPU        ' + (g.renderer || 'unknown') + (g.software ? '   (SOFTWARE RENDERER)' : ''),
      'Canvas     ' + c.width + 'x' + c.height + ' at devicePixelRatio ' + window.devicePixelRatio,
      'Cores      ' + (navigator.hardwareConcurrency || '?') + (navigator.deviceMemory ? ', ~' + navigator.deviceMemory + ' GB memory' : ''),
    ];
  }

  function consoleText() {
    const which = ['general', 'warning', 'error'].filter((k) => shown[k]);
    const out = [m2hleGame.title + ' (m2-hle web) -- console', ...systemInfo(),
                 'Showing    ' + which.join(', ') + (dropped ? '   (' + dropped + ' older lines were discarded)' : ''), ''];
    for (const l of lines) if (shown[l.level]) out.push(shownText(l));
    return out.join('\n');
  }

  async function copy(text, button) {
    let ok = false;
    try {
      await navigator.clipboard.writeText(text);
      ok = true;
    } catch (e) {
      /* No clipboard permission (or not a secure context): the old way. */
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try { ok = document.execCommand('copy'); } catch (e2) { ok = false; }
      ta.remove();
    }
    const was = button.textContent;
    button.textContent = tr(ok ? 'tools.copied' : 'tools.copyFailed');
    setTimeout(() => { button.textContent = was; }, 1500);
  }

  /* ---- The GPU ------------------------------------------------------------------- */

  let gpuCache = null;
  function gpuInfo() {
    if (gpuCache) return gpuCache;
    /* Only once sokol has made its context: asking the canvas for one before then
     * would CREATE it, with the wrong attributes, and sokol would inherit it. */
    if (!ready) return {};
    const gl = $('canvas').getContext('webgl2');
    if (!gl) return {};
    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    const renderer = String(dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER));
    gpuCache = {
      gl, renderer,
      software: /swiftshader|llvmpipe|softpipe|software|basic render/i.test(renderer),
      timer: gl.getExtension('EXT_disjoint_timer_query_webgl2'),
    };
    return gpuCache;
  }

  /* GPU time per frame, where the browser allows it (Chrome on desktop, mostly):
   * a timer query around the frame's GL work, asked for by web_perf_gpu_timing.
   * Results come back some frames later; a "disjoint" reading is thrown away. */
  const gpuTimer = { active: null, pending: [], ns: 0, n: 0 };
  function frameBegin() {
    const g = gpuInfo();
    if (!g.timer || gpuTimer.active || gpuTimer.pending.length > 8) return;
    gpuTimer.active = g.gl.createQuery();
    g.gl.beginQuery(g.timer.TIME_ELAPSED_EXT, gpuTimer.active);
  }
  function frameEnd() {
    const g = gpuInfo();
    if (!g.timer) return;
    if (gpuTimer.active) {
      g.gl.endQuery(g.timer.TIME_ELAPSED_EXT);
      gpuTimer.pending.push(gpuTimer.active);
      gpuTimer.active = null;
    }
    while (gpuTimer.pending.length && g.gl.getQueryParameter(gpuTimer.pending[0], g.gl.QUERY_RESULT_AVAILABLE)) {
      const q = gpuTimer.pending.shift();
      if (!g.gl.getParameter(g.timer.GPU_DISJOINT_EXT)) {
        gpuTimer.ns += g.gl.getQueryParameter(q, g.gl.QUERY_RESULT);
        gpuTimer.n++;
      }
      g.gl.deleteQuery(q);
    }
  }

  /* ---- The lag check --------------------------------------------------------------- */

  const readPerf = (reset) => JSON.parse(Module.UTF8ToString(Module._web_perf(reset ? 1 : 0)));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const f1 = (x) => x.toFixed(1);

  async function measure(seconds) {
    let hidden = document.hidden;
    const onVis = () => { if (document.hidden) hidden = true; };
    document.addEventListener('visibilitychange', onVis);
    gpuTimer.ns = 0; gpuTimer.n = 0;
    Module._web_perf_gpu_timing(1);
    readPerf(true);                                   /* clear the maxima */
    const a = readPerf(false), audioA = Module.m2hleAudioStats(), eventsA = { ...events };
    await sleep(seconds * 1000);
    const b = readPerf(true), audioB = Module.m2hleAudioStats();
    Module._web_perf_gpu_timing(0);
    document.removeEventListener('visibilitychange', onVis);

    const T = Math.max(0.001, (b.now_us - a.now_us) / 1e6);
    const d = (k) => b[k] - a[k];
    const g = gpuInfo();
    return {
      seconds: T, hidden,
      drawRate: d('callbacks') / T,                   /* frames the browser let us draw, per second */
      /* Of those, the ones actually drawn: on a display faster than the board, a
       * page short of time draws only the board's own frames rather than
       * interpolating between them (main_web.c, web_afford_picture). */
      pictureRate: (d('callbacks') - d('pictures_skipped')) / T,
      skipped: d('pictures_skipped'),
      speed: d('slices') / T,                         /* emulated frames per second: 60 is full speed */
      sliceMs: d('slices') ? d('slice_us') / d('slices') / 1000 : 0,
      sliceMaxMs: b.slice_us_max / 1000,
      renderMs: d('callbacks') ? d('render_us') / d('callbacks') / 1000 : 0,
      renderMaxMs: b.render_us_max / 1000,
      busy: (d('slice_us') + d('render_us')) / (T * 1e6),   /* share of the main thread we used */
      longFrames: d('long_callbacks'), worstGapMs: b.gap_us_max / 1000,
      forgivenMs: d('forgiven_us') / 1000,
      gpuMs: gpuTimer.n ? gpuTimer.ns / gpuTimer.n / 1e6 : null,
      canvasW: b.canvas_w, canvasH: b.canvas_h, renderScale: b.render_scale,
      soundDriver: Module._web_sound_driver(),
      renderer: g.renderer || 'unknown', software: !!g.software,
      audio: audioB, dropouts: (audioB.underruns || 0) - (audioA.underruns || 0),
      netplay: b.netplay, stalls: d('netplay_stalls'), netDelay: b.netplay_delay,
      complaints: (events.warning - eventsA.warning) + (events.error - eventsA.error),
    };
  }

  /* What the numbers mean. Each finding: how bad, a plain title, the evidence, what
   * to try, and sometimes something this page can do about it right now. */
  function reason(m) {
    const out = [];
    const find = (level, id, vars, evidence, tips, action) => out.push({
      level, title: tr('tools.' + id + '.title', vars), titleEn: english('tools.' + id + '.title', vars),
      evidence, tips: (tips || []).map((k) => tr(k)), action });

    if (m.hidden) {
      find('info', 'hidden', null, tr('tools.hidden.evidence'), ['tools.hidden.tip']);
      return out;                 /* nothing measured then means anything */
    }

    const c = {
      slow: m.speed < 57,
      cpuBound: m.busy > 0.7 || m.sliceMs + m.renderMs > 13,
      lowerRes: m.renderScale === 0 || m.renderScale > 2
        ? { label: tr('tools.action.lowerRes'), run: () => setRenderScale(2) } : null,
      lighterSound: m.soundDriver === 0
        ? { label: tr('tools.action.lighterSound'), run: () => setSoundDriver(1) } : null,
    };
    reasonCpu(m, c, find);
    const drawExplained = reasonDraw(m, c, find);
    reasonStutter(m, c, drawExplained, find);
    reasonSound(m, c, find);
    reasonOther(m, find);

    if (!out.some((f) => f.level === 'bad' || f.level === 'warn')) {
      find('ok', 'ok', null, tr(m.gpuMs !== null ? 'tools.ok.evidence.gpu' : 'tools.ok.evidence', {
        speed: f1(m.speed), rate: f1(m.drawRate), slice: f1(m.sliceMs), render: f1(m.renderMs),
        gpu: m.gpuMs !== null ? f1(m.gpuMs) : '', late: m.longFrames, queue: Math.round(m.audio.queueMs || 0) }),
      ['tools.ok.tip.outside', 'tools.ok.tip.again']);
    }
    return out;
  }

  function reasonCpu(m, c, find) {
    const soundTip = c.lighterSound ? ['tools.tip.sound'] : [];
    if (c.slow && c.cpuBound) {
      find('bad', 'cpuSlow', null, tr('tools.cpuSlow.evidence',
        { speed: f1(m.speed), slice: f1(m.sliceMs), sliceMax: f1(m.sliceMaxMs), render: f1(m.renderMs) }),
      ['tools.tip.closeCpu', 'tools.tip.chargerCpu', 'tools.tip.chrome', ...soundTip], c.lighterSound);
    } else if (c.cpuBound) {
      find('warn', 'cpuTight', null, tr('tools.cpuTight.evidence',
        { speed: f1(m.speed), slice: f1(m.sliceMs), render: f1(m.renderMs) }),
      ['tools.tip.close', 'tools.tip.charger', ...soundTip], c.lighterSound);
    } else if (c.slow) {
      find('bad', 'turns', null, tr('tools.turns.evidence',
        { speed: f1(m.speed), busy: Math.round(m.busy * 100), rate: f1(m.drawRate) }),
      ['tools.turns.tip']);
    }
  }

  /* Everything from here to the sound is also what a slow CPU looks like. When the
   * CPU is already the answer these are its symptoms, not more causes, and listing
   * them sends people after the wrong thing. Returns whether too few frames were
   * drawn and a finding already says why. */
  function reasonDraw(m, c, find) {
    if (m.drawRate < 50 && !c.cpuBound) {
      const near30 = m.drawRate > 27 && m.drawRate < 33;
      const canvas = { w: m.canvasW, h: m.canvasH, mp: f1(m.canvasW * m.canvasH / 1e6), renderer: m.renderer };
      const level = c.slow ? 'bad' : 'warn';
      if (m.software) {
        find('bad', 'soft', null, tr('tools.soft.evidence', { renderer: m.renderer, rate: f1(m.drawRate) }),
          ['tools.soft.tip.accel', 'tools.soft.tip.driver'], c.lowerRes);
      } else if (m.gpuMs !== null && m.gpuMs > 12) {
        find('bad', 'gpu', null, tr('tools.gpu.evidence', { gpu: f1(m.gpuMs), ...canvas }),
          ['tools.gpu.tip'], c.lowerRes);
      } else if (near30) {
        find(level, 'half', null, tr(c.slow ? 'tools.half.evidence.slow' : 'tools.half.evidence',
          { rate: f1(m.drawRate), speed: f1(m.speed) }),
        ['tools.half.tip.power', 'tools.half.tip.hz', 'tools.half.tip.big'], c.lowerRes);
      } else {
        find(level, 'few', null, tr(m.gpuMs === null ? 'tools.few.evidence.noGpu' : 'tools.few.evidence',
          { rate: f1(m.drawRate), need: f1(m.sliceMs + m.renderMs), ...canvas }),
        ['tools.few.tip.smaller', 'tools.few.tip.video'], c.lowerRes);
      }
      return true;
    }
    if (m.software) {
      find('warn', 'softOk', null, tr('tools.softOk.evidence', { renderer: m.renderer }), ['tools.softOk.tip']);
    }
    return false;
  }

  /* Late frames: the emulator's, or the browser's. */
  function reasonStutter(m, c, drawExplained, find) {
    if (!c.cpuBound && !drawExplained && (m.longFrames >= Math.max(2, m.seconds * 0.5) || m.worstGapMs > 70)) {
      const ours = m.sliceMaxMs > 20 || m.renderMaxMs > 20;
      find('warn', 'stutter', null, tr(ours ? 'tools.stutter.ours' : 'tools.stutter.outside', {
        n: m.longFrames, s: f1(m.seconds), gap: f1(m.worstGapMs), slice: f1(m.sliceMaxMs), render: f1(m.renderMaxMs),
        worst: f1(Math.max(m.sliceMaxMs, m.renderMaxMs)) }),
      ours ? ['tools.stutter.tip.fight'] : ['tools.tip.close', 'tools.stutter.tip.ext']);
    }
  }

  /* The sound: dropouts, which follow from late frames, and a slow device. */
  function reasonSound(m, c, find) {
    if (m.dropouts > 0) {
      find('warn', m.dropouts === 1 ? 'dropout.one' : 'dropout.many', { n: m.dropouts },
        tr(c.cpuBound ? 'tools.dropout.cpu' : 'tools.dropout.late', { ms: Math.round(m.audio.targetMs || 0) }),
        [c.cpuBound ? 'tools.dropout.tip.cpu' : 'tools.dropout.tip.late']);
    }
    const deviceMs = (m.audio.baseMs || 0) + (m.audio.outputMs || 0);
    if (deviceMs > 90) {
      find('warn', 'device', { ms: Math.round(deviceMs) },
        tr('tools.device.evidence', { queue: Math.round(m.audio.queueMs || 0) }), ['tools.device.tip']);
    }
  }

  /* The emulator's own complaints, and the other player. */
  function reasonOther(m, find) {
    if (m.complaints / m.seconds > 10) {
      const worst = lines.filter((l) => l.level !== 'general').sort((x, y) => y.n - x.n)[0];
      find('warn', 'noisy', { n: Math.round(m.complaints / m.seconds) },
        tr(worst ? 'tools.noisy.evidence.worst' : 'tools.noisy.evidence',
          { n: m.complaints, s: f1(m.seconds), worst: worst ? worst.text : '' }),
        ['tools.noisy.tip']);
    }

    if (m.netplay === 'playing' && m.stalls > 0) {
      find('warn', 'wait', null,
        tr(m.stalls === 1 ? 'tools.wait.one' : 'tools.wait.many', { n: m.stalls, s: f1(m.seconds), delay: m.netDelay }),
        ['tools.wait.tip.delay', 'tools.wait.tip.wifi']);
    }
  }

  function reportText(m, findings) {
    const out = [m2hleGame.title + ' (m2-hle web) -- lag check', ...systemInfo(), ''];
    for (const f of findings) {
      out.push('[' + f.level.toUpperCase() + '] ' + f.title, '    ' + f.evidence);
      for (const t of f.tips) out.push('    - ' + t);
      out.push('');
    }
    out.push('Measurements over ' + f1(m.seconds) + ' s:',
      '  game speed        ' + f1(m.speed) + ' / 60 fps',
      '  drawn             ' + f1(m.pictureRate) + ' fps' +
                              (m.skipped ? ' (of ' + f1(m.drawRate) + ' the display asked for; the rest would have been interpolated between board frames)' : ''),
      '  emulation         ' + f1(m.sliceMs) + ' ms avg, ' + f1(m.sliceMaxMs) + ' worst',
      '  picture (CPU)     ' + f1(m.renderMs) + ' ms avg, ' + f1(m.renderMaxMs) + ' worst',
      '  GPU               ' + (m.gpuMs === null ? 'not measurable in this browser' : f1(m.gpuMs) + ' ms avg'),
      '  main thread busy  ' + Math.round(m.busy * 100) + '%',
      '  late frames       ' + m.longFrames + ' (worst gap ' + f1(m.worstGapMs) + ' ms), time dropped ' + f1(m.forgivenMs) + ' ms',
      '  canvas            ' + m.canvasW + 'x' + m.canvasH + ', render scale ' + (m.renderScale || 'full'),
      '  sound driver      ' + (m.soundDriver ? 'lighter (C)' : 'sound CPU (68000)'),
      '  audio             ' + (m.audio.mode || '?') + ', queue ' + Math.round(m.audio.queueMs || 0) + ' ms, device ' +
                              Math.round((m.audio.baseMs || 0) + (m.audio.outputMs || 0)) + ' ms, dropouts ' + m.dropouts,
      '  netplay           ' + m.netplay + (m.netplay === 'playing' ? ', stalls ' + m.stalls : ''),
      '  warnings/errors   ' + m.complaints + ' during the check');
    const lately = lines.filter((l) => l.level !== 'general').slice(-15);
    if (lately.length) {
      out.push('', 'Recent warnings and errors:');
      for (const l of lately) out.push('  ' + shownText(l));
    }
    return out.join('\n');
  }

  let lastReport = '';
  async function diagnose(seconds) {
    seconds = seconds || 5;
    const status = $('lag-status'), list = $('lag-findings'), start = $('lag-start');
    if (!ready || Module._web_state() !== 1) {
      status.textContent = tr('tools.lag.notRunning');
      return null;
    }
    start.disabled = true;
    list.textContent = '';
    $('lag-copy').hidden = true;
    /* Hand the keyboard back: the check is only worth anything while playing. */
    $('canvas').focus();
    const pending = measure(seconds);
    for (let s = seconds; s > 0; s--) {
      status.textContent = tr('tools.lag.measuring', { s });
      await sleep(1000);
    }
    const m = await pending;
    const findings = reason(m);
    status.textContent = tr('tools.lag.checked', { s: f1(m.seconds) });
    for (const f of findings) {
      const card = document.createElement('article');
      card.className = 'finding finding-' + f.level;
      const h = document.createElement('h3'); h.textContent = f.title; card.appendChild(h);
      const p = document.createElement('p'); p.textContent = f.evidence; card.appendChild(p);
      if (f.tips.length) {
        const ul = document.createElement('ul');
        for (const t of f.tips) { const li = document.createElement('li'); li.textContent = t; ul.appendChild(li); }
        card.appendChild(ul);
      }
      if (f.action) {
        const b = document.createElement('button');
        b.className = 'tool';
        b.textContent = f.action.label;
        b.addEventListener('click', () => { f.action.run(); b.textContent = tr('tools.lag.done'); b.disabled = true; });
        card.appendChild(b);
      }
      list.appendChild(card);
    }
    lastReport = reportText(m, findings);
    add('lag check: ' + findings.map((f) => '[' + f.level + '] ' + f.titleEn).join('; '));
    $('lag-copy').hidden = false;
    start.disabled = false;
    start.textContent = tr('tools.lag.again');
    return { measurements: m, findings, text: lastReport };
  }

  /* ---- Picture resolution ---------------------------------------------------------- */

  function setRenderScale(n) {
    Module._web_set_render_scale(n);
    try { localStorage.setItem('m2hle.renderScale', String(n)); } catch (e) { /* private mode */ }
    const sel = $('render-scale');
    if (sel) sel.value = String(n);
  }

  /* ---- Sound driver -------------------------------------------------------------------
   * 0: the emulated 68000 runs the game's own sound program. 1: the same driver
   * ported to C (src/board/sound_hle.h): the sound board in about half the time,
   * 4-11% of a whole frame in the WASM build (det_digest under Node). Online
   * the change waits for the next match, since every match starts the board afresh. */

  function setSoundDriver(n) {
    try { localStorage.setItem('m2hle.soundDriver', String(n)); } catch (e) { /* private mode */ }
    const sel = $('sound-driver');
    if (sel) sel.value = String(n);
    if (!ready) return;
    const now = Module._web_set_sound_driver(n);
    if (Module._web_state() === 0) return;           /* no game yet: it starts with this one */
    $('lag-status').textContent = tr(!now ? 'tools.sound.nextMatch'
      : Module._web_sound_driver() !== n ? 'tools.sound.none' : 'tools.sound.changed');
  }

  /* ---- The drawer ---------------------------------------------------------------------- */

  function openDrawer(tab) {
    const drawer = $('drawer');
    const same = !drawer.hidden && !$('pane-' + tab).hidden;
    if (same) { closeDrawer(); return; }
    drawer.hidden = false;
    for (const t of ['lag', 'console', 'picture', 'replay', 'cheats']) {
      $('pane-' + t).hidden = t !== tab;
      $('tab-' + t).setAttribute('aria-selected', String(t === tab));
      $('btn-' + t).setAttribute('aria-expanded', String(t === tab));
    }
    if (tab === 'console') rerender();
    /* Let go of anything held: the key-up will land in here, not in the game. */
    if (ready && Module._web_release_keys) Module._web_release_keys();
  }

  function closeDrawer() {
    $('drawer').hidden = true;
    for (const t of ['lag', 'console', 'picture', 'replay', 'cheats']) $('btn-' + t).setAttribute('aria-expanded', 'false');
    $('canvas').focus();
  }

  function init() {
    $('btn-lag').addEventListener('click', () => openDrawer('lag'));
    $('btn-console').addEventListener('click', () => openDrawer('console'));
    $('tab-lag').addEventListener('click', () => openDrawer('lag'));
    $('tab-console').addEventListener('click', () => openDrawer('console'));
    $('btn-picture').addEventListener('click', () => openDrawer('picture'));
    $('tab-picture').addEventListener('click', () => openDrawer('picture'));
    $('btn-replay').addEventListener('click', () => openDrawer('replay'));
    $('tab-replay').addEventListener('click', () => openDrawer('replay'));
    $('btn-cheats').addEventListener('click', () => openDrawer('cheats'));
    $('tab-cheats').addEventListener('click', () => openDrawer('cheats'));
    $('drawer-close').addEventListener('click', closeDrawer);
    $('lag-start').addEventListener('click', () => diagnose(5));
    $('lag-copy').addEventListener('click', (e) => copy(lastReport, e.currentTarget));
    $('log-copy').addEventListener('click', (e) => copy(consoleText(), e.currentTarget));
    $('log-clear').addEventListener('click', () => {
      lines.length = 0; rendered = 0; dropped = 0;
      recent.clear(); dirty.clear();
      counts.general = counts.warning = counts.error = 0;
      rerender();
    });
    for (const k of ['general', 'warning', 'error']) {
      $('filter-' + k).addEventListener('click', (e) => {
        shown[k] = !shown[k];
        e.currentTarget.setAttribute('aria-pressed', String(shown[k]));
        rerender();
      });
    }
    $('render-scale').addEventListener('change', (e) => setRenderScale(Number(e.target.value)));
    $('sound-driver').addEventListener('change', (e) => setSoundDriver(Number(e.target.value)));
    /* sokol_app re-measures the canvas when the WINDOW resizes, and only then. The
     * drawer changes the canvas's size without that, and the old
     * drawing buffer gets stretched into the new box: a squashed picture. Tell it. */
    if (window.ResizeObserver) {
      new ResizeObserver(() => window.dispatchEvent(new Event('resize'))).observe($('canvas'));
    }
    render();
  }

  /* Called from Module.onM2hleReady: the exports and the GL context exist now. */
  function onReady() {
    ready = true;
    let stored = 0;
    try { stored = Number(localStorage.getItem('m2hle.renderScale')) || 0; } catch (e) { /* private mode */ }
    const asked = new URLSearchParams(location.search).get('scale');
    const scale = asked !== null ? Number(asked) || 0 : stored;
    if (scale) Module._web_set_render_scale(scale);
    $('render-scale').value = String(scale);
    /* Before the game is loaded, so the board boots with it. ?sounddriver=c|68000 for tests. */
    let driver = 0;
    try { driver = Number(localStorage.getItem('m2hle.soundDriver')) === 1 ? 1 : 0; } catch (e) { /* private mode */ }
    const askedDriver = new URLSearchParams(location.search).get('sounddriver');
    if (askedDriver !== null) driver = askedDriver === 'c' ? 1 : 0;
    Module._web_set_sound_driver(driver);
    $('sound-driver').value = String(driver);
    const g = gpuInfo();
    add('web: renderer "' + (g.renderer || 'unknown') + '"' + (g.software ? ' (software)' : '') +
        ', canvas ' + $('canvas').width + 'x' + $('canvas').height + ', devicePixelRatio ' + window.devicePixelRatio);
  }

  document.addEventListener('DOMContentLoaded', init);

  return { add, print, printErr, frameBegin, frameEnd, diagnose, reason, onReady, consoleText, openDrawer, systemInfo, copy };
})();
