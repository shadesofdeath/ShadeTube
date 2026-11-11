// ShadeTube phone remote. Served by the app itself (app/RemoteControl); nothing to install.
//
// Pairing: the QR code opens /#p=<secret>; the page trades the secret once for its own device token (POST /api/pair)
// and keeps that in localStorage. Every API call carries it. State comes as Server-Sent Events over fetch (so the
// token stays in a header): "state" (the player), "queue" (what plays next), "pos" (position + lyrics line, once a
// second). Commands are JSON POSTs to /api/cmd. Every text comes from /api/i18n (the app's own translations).
'use strict';
(() => {
  const TOKEN_KEY = 'shadetube.remote.token';
  const root = document.getElementById('root');

  // ---- small helpers ----------------------------------------------------------------------------------------------
  let S = {};
  const t = (k) => S[k] || '';
  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function icon(name) {
    const s = el('span', 'ic i-' + name);
    s.setAttribute('aria-hidden', 'true');
    return s;
  }
  function iconButton(name, label, cls) {
    const b = el('button', 'icon-btn' + (cls ? ' ' + cls : ''));
    b.type = 'button';
    b.setAttribute('aria-label', label);
    b.title = label;
    b.appendChild(icon(name));
    return b;
  }
  function setIcon(button, name) {
    const i = button.querySelector('.ic');
    if (i) i.className = 'ic i-' + name;
  }
  function storageGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function storageSet(k, v) { try { if (v == null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) { /* private mode */ } }
  function fmt(ms) {
    if (!(ms > 0)) ms = 0;
    const s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
    const two = (n) => (n < 10 ? '0' : '') + n;
    return h > 0 ? h + ':' + two(m) + ':' + two(r) : two(m) + ':' + two(r);
  }
  // Rows and other non-button elements that act like buttons (keyboard: Enter / Space).
  function activate(node, fn) {
    node.setAttribute('role', 'button');
    node.tabIndex = 0;
    node.addEventListener('click', fn);
    node.addEventListener('keydown', (e) => {
      if (e.target === node && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); fn(e); }
    });
  }

  // ---- covers: http(s) straight from the image host, the app's own local covers with the token -----------------
  const blobs = new Map();
  function setCover(box, url, placeholder) {
    box.textContent = '';
    box.classList.remove('ph');
    if (!url) {
      box.classList.add('ph');
      box.appendChild(icon(placeholder || 'music-note'));
      return;
    }
    const img = el('img');
    img.alt = '';
    img.decoding = 'async';
    img.loading = 'lazy';
    img.referrerPolicy = 'no-referrer';
    img.onerror = () => { box.textContent = ''; box.classList.add('ph'); box.appendChild(icon(placeholder || 'music-note')); };
    box.appendChild(img);
    if (url.startsWith('/api/')) {
      if (blobs.has(url)) { img.src = blobs.get(url); return; }
      fetch(url, { headers: auth() })
        .then((r) => (r.ok ? r.blob() : Promise.reject()))
        .then((b) => { const u = URL.createObjectURL(b); blobs.set(url, u); img.src = u; })
        .catch(() => img.onerror());
    } else {
      img.src = url;
    }
  }

  // ---- connection -------------------------------------------------------------------------------------------------
  let token = storageGet(TOKEN_KEY);
  const auth = () => ({ Authorization: 'Bearer ' + token });

  async function api(path, opts) {
    opts = opts || {};
    const headers = auth();
    if (opts.body !== undefined) headers['Content-Type'] = 'application/json';
    const r = await fetch(path, {
      method: opts.method || 'GET', headers, cache: 'no-store', signal: opts.signal,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
    });
    if (r.status === 401) { revoked('revoked'); throw Object.assign(new Error('401'), { status: 401 }); }
    let j = null;
    try { j = await r.json(); } catch (e) { /* empty */ }
    if (!r.ok) throw Object.assign(new Error(String(r.status)), { status: r.status, data: j });
    return j;
  }

  async function cmd(name, extra) {
    try {
      const j = await api('/api/cmd', { method: 'POST', body: Object.assign({ cmd: name }, extra || {}) });
      if (j && j.toast) toast(j.toast);
      return true;
    } catch (e) {
      if (e.status === 401) return false;
      toast((e.data && e.data.toast) || (e.status === 429 ? t('throttled') : t('error')));
      return false;
    }
  }

  // The event stream (fetch + a small SSE reader), reconnecting with a backoff; closed while the page is hidden so
  // the PC stops its position ticks.
  let stream = null, retryMs = 1000, retryTimer = 0, lastData = 0, polling = 0;
  function connect() {
    clearTimeout(retryTimer);
    if (stream || !token || document.hidden) return;
    const ctrl = new AbortController();
    stream = ctrl;
    (async () => {
      try {
        const r = await fetch('/api/events', { headers: auth(), signal: ctrl.signal, cache: 'no-store' });
        if (r.status === 401) { revoked('revoked'); return; }
        if (r.status === 429) throw new Error('429');
        if (!r.ok) throw new Error(String(r.status));
        if (!r.body || !r.body.getReader) { stream = null; startPolling(); return; }
        const reader = r.body.getReader();
        const dec = new TextDecoder();
        let buf = '';
        setOnline(true);
        retryMs = 1000;
        lastData = Date.now();
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          lastData = Date.now();
          buf += dec.decode(value, { stream: true }).replace(/\r\n?/g, '\n');
          let i;
          while ((i = buf.indexOf('\n\n')) >= 0) {
            const block = buf.slice(0, i);
            buf = buf.slice(i + 2);
            let event = 'message';
            const data = [];
            for (const line of block.split('\n')) {
              if (line.startsWith(':')) continue;
              const c = line.indexOf(':');
              const field = c < 0 ? line : line.slice(0, c);
              let v = c < 0 ? '' : line.slice(c + 1);
              if (v.startsWith(' ')) v = v.slice(1);
              if (field === 'event') event = v;
              else if (field === 'data') data.push(v);
            }
            if (data.length) onEvent(event, data.join('\n'));
          }
        }
      } catch (e) { /* network error or aborted */ }
      if (stream !== ctrl) return;   // replaced / closed on purpose
      stream = null;
      if (!token) return;
      setOnline(false);
      retryTimer = setTimeout(connect, retryMs);
      retryMs = Math.min(retryMs * 2, 10000);
    })();
  }
  function disconnect() {
    clearTimeout(retryTimer);
    if (stream) { const s = stream; stream = null; s.abort(); }
    stopPolling();
  }
  // Browsers without streaming fetch bodies: poll the state.
  function startPolling() {
    if (polling) return;
    const poll = async () => {
      try {
        const j = await api('/api/state');
        setOnline(true);
        if (j && j.state) onEvent('state', JSON.stringify(j.state));
        if (j && j.queue) onEvent('queue', JSON.stringify(j.queue));
      } catch (e) { if (e.status !== 401) setOnline(false); }
    };
    poll();
    polling = setInterval(poll, 2000);
  }
  function stopPolling() { if (polling) clearInterval(polling); polling = 0; }
  // A stream that went silent (the PC sends something every second while connected) is a dead one.
  setInterval(() => {
    if (stream && lastData && Date.now() - lastData > 12000) { const s = stream; stream = null; s.abort(); setOnline(false); connect(); }
  }, 4000);
  document.addEventListener('visibilitychange', () => {
    if (!token) return;
    if (document.hidden) disconnect();
    else { retryMs = 1000; connect(); }
  });

  function revoked(key) {
    token = null;
    storageSet(TOKEN_KEY, null);
    disconnect();
    showPairing(key);
  }

  // ---- app state --------------------------------------------------------------------------------------------------
  let state = null, queue = null;
  let pos = { p: 0, at: 0, playing: false, line: '' };
  let ui = null;           // the shell's elements, built once
  let tab = 'now';
  let detail = null;       // the open album / playlist / artist ({kind, id}), over the current tab

  function onEvent(name, data) {
    let j;
    try { j = JSON.parse(data); } catch (e) { return; }
    if (name === 'state') {
      state = j;
      pos = { p: j.positionMs || 0, at: performance.now(), playing: !!j.playing, line: pos.line };
      applyAccent();
      renderNow();
    } else if (name === 'queue') {
      queue = j;
      renderQueue();
    } else if (name === 'pos') {
      pos = { p: j.p || 0, at: performance.now(), playing: !!j.playing, line: j.line || '' };
      if (state) state.playing = pos.playing;
      renderLyric();
      renderTransport();
    }
  }

  // The app's accent (from the album art) as this phone's theme shows it.
  const lightQuery = window.matchMedia ? window.matchMedia('(prefers-color-scheme: light)') : null;
  function applyAccent() {
    const a = state && state.accent;
    const style = document.documentElement.style;
    if (!a) { style.removeProperty('--accent-base'); style.removeProperty('--accent-on-accent'); return; }
    const pick = lightQuery && lightQuery.matches ? a.light : a.dark;
    style.setProperty('--accent-base', pick.base);
    style.setProperty('--accent-on-accent', pick.on);
  }
  if (lightQuery && lightQuery.addEventListener) lightQuery.addEventListener('change', applyAccent);

  function currentPosition() {
    if (!state) return 0;
    let p = pos.p;
    if (pos.playing && state.status === 'playing') p += (performance.now() - pos.at) * (state.speed || 1);
    return state.durationMs > 0 ? Math.min(p, state.durationMs) : p;
  }

  // ---- toast / sheet ----------------------------------------------------------------------------------------------
  let toastTimer = 0;
  function toast(text) {
    if (!text) return;
    let n = document.querySelector('.toast');
    if (!n) { n = el('div', 'toast'); n.setAttribute('role', 'status'); document.body.appendChild(n); }
    n.textContent = text;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => n.remove(), 3000);
  }

  let sheetReturn = null;
  function openSheet(head, items) {
    closeSheet();
    sheetReturn = document.activeElement;
    const scrim = el('div', 'scrim');
    const sheet = el('div', 'sheet');
    sheet.setAttribute('role', 'dialog');
    sheet.setAttribute('aria-modal', 'true');
    if (head) {
      const h = el('div', 'sheet-head');
      if (head.cover !== undefined) {
        const th = el('div', 'thumb');
        setCover(th, head.cover, head.placeholder);
        h.appendChild(th);
      }
      const tx = el('div', 'row-text');
      tx.appendChild(el('div', 'row-title clip', head.title || ''));
      if (head.sub) tx.appendChild(el('div', 'row-sub clip', head.sub));
      h.appendChild(tx);
      sheet.appendChild(h);
    }
    for (const it of items) {
      const b = el('button', 'sheet-item' + (it.danger ? ' danger' : ''));
      b.type = 'button';
      b.appendChild(icon(it.icon));
      b.appendChild(el('span', 'clip', it.label));
      b.addEventListener('click', () => { closeSheet(); it.run(); });
      sheet.appendChild(b);
    }
    const cancel = el('button', 'sheet-item cancel', t('cancel'));
    cancel.type = 'button';
    cancel.addEventListener('click', closeSheet);
    sheet.appendChild(cancel);
    scrim.appendChild(sheet);
    scrim.addEventListener('click', (e) => { if (e.target === scrim) closeSheet(); });
    scrim.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeSheet(); });
    document.body.appendChild(scrim);
    const first = sheet.querySelector('button');
    if (first) first.focus();
  }
  function closeSheet() {
    const s = document.querySelector('.scrim');
    if (!s) return;
    s.remove();
    if (sheetReturn && sheetReturn.focus) sheetReturn.focus();
    sheetReturn = null;
  }

  // A song's menu: play it (in its list), next, at the end, its album / artist.
  function songSheet(track, list, index) {
    const items = [
      { icon: 'play', label: t('playSong'), run: () => cmd('playList', { list, index }) },
      { icon: 'queue', label: t('playNext'), run: () => cmd('playNext', { list, index }) },
      { icon: 'plus', label: t('addToQueue'), run: () => cmd('enqueue', { list, index }) },
    ];
    if (track.album && track.album.id) items.push({ icon: 'album', label: t('goAlbum'), run: () => openDetail('album', track.album.id) });
    const artist = (track.artists || []).find((a) => a.id);
    if (artist) items.push({ icon: 'artist', label: t('goArtist'), run: () => openDetail('artist', artist.id) });
    openSheet({ title: track.title, sub: track.artist, cover: track.cover }, items);
  }

  // ---- pairing ----------------------------------------------------------------------------------------------------
  function showPairing(messageKey) {
    ui = null;
    root.textContent = '';
    const box = el('div', 'pair');
    const logo = el('img', 'logo');
    logo.src = '/logo.svg';
    logo.alt = '';
    box.appendChild(logo);
    box.appendChild(el('h1', null, t('title')));
    box.appendChild(el('p', null, t('pairHint')));
    if (messageKey) {
      const n = el('div', 'notice' + (messageKey === 'pairing' ? '' : ' error'), t(messageKey));
      n.setAttribute('role', 'status');
      box.appendChild(n);
    }
    if (messageKey !== 'pairing') {
      const label = el('label', 'mono-label', t('codeLabel'));
      label.htmlFor = 'code';
      box.appendChild(label);
      const field = el('div', 'field');
      const input = el('input');
      input.id = 'code';
      input.autocomplete = 'off';
      input.autocapitalize = 'off';
      input.spellcheck = false;
      const go = el('button', 'pill primary', t('connect'));
      go.type = 'button';
      const submit = () => {
        const code = input.value.replace(/[^A-Za-z0-9_-]/g, '');
        if (code.length >= 16) pair(code);
      };
      go.addEventListener('click', submit);
      input.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });
      field.appendChild(input);
      field.appendChild(go);
      box.appendChild(field);
      box.appendChild(el('div', 'hint', t('codeHint')));
    }
    root.appendChild(box);
  }

  async function pair(secret) {
    showPairing('pairing');
    try {
      const r = await fetch('/api/pair', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ secret }), cache: 'no-store',
      });
      if (r.status === 429) return showPairing('throttled');
      if (!r.ok) return showPairing('pairFailed');
      const j = await r.json();
      token = j.token;
      storageSet(TOKEN_KEY, token);
      start();
    } catch (e) {
      showPairing('offline');
    }
  }

  // ---- the shell --------------------------------------------------------------------------------------------------
  function start() {
    buildShell();
    connect();
    route();
  }

  // Navigation lives in the address: "#queue" / "#search" / "#library" / "#now" (tabs, replaced) and
  // "#album=<id>" / "#playlist=<id>" / "#artist=<id>" (pushed), so the phone's back gesture and a reload work.
  let pushed = 0;
  function go(hash, push) {
    if (location.hash === hash) return route();
    if (push) { history.pushState(null, '', hash); pushed++; } else history.replaceState(null, '', hash);
    route();
  }
  function route() {
    if (!ui) return;
    const h = location.hash.slice(1);
    const m = /^(album|playlist|artist)=(.+)$/.exec(h);
    if (m) {
      let id = '';
      try { id = decodeURIComponent(m[2]); } catch (e) { /* malformed */ }
      if (id) { detail = { kind: m[1], id }; showDetail(); return; }
    }
    detail = null;
    ui.detail.hidden = true;
    ui.detail.textContent = '';
    showTab(['now', 'queue', 'search', 'library'].includes(h) ? h : tab);
  }
  window.addEventListener('popstate', () => { if (pushed > 0) pushed--; route(); });
  function switchTab(id) { go('#' + id, false); }
  function openDetail(kind, id) { if (id) go('#' + kind + '=' + encodeURIComponent(id), true); }
  function goBack() {
    if (pushed > 0) history.back();
    else go('#' + tab, false);   // opened straight from the address: back to its tab
  }

  function buildShell() {
    root.textContent = '';
    const app = el('div', 'app');
    const banner = el('div', 'banner');
    banner.hidden = true;
    banner.setAttribute('role', 'status');
    banner.appendChild(icon('offline'));
    banner.appendChild(el('span', null, t('offline')));
    app.appendChild(banner);

    const views = el('main', 'views');
    const now = buildNow(), q = buildQueue(), search = buildSearch(), library = buildLibrary();
    const detail = el('section', 'view detail');
    detail.hidden = true;
    for (const v of [now.view, q.view, search.view, library.view, detail]) views.appendChild(v);
    app.appendChild(views);

    // Mini player (on every tab but Now playing).
    const mini = el('div', 'mini');
    mini.hidden = true;
    const progress = el('div', 'mini-progress');
    const inner = el('div', 'mini-inner');
    const mthumb = el('div', 'thumb');
    const mtext = el('div', 'row-text');
    const mtitle = el('div', 'row-title clip'), msub = el('div', 'row-sub clip');
    mtext.appendChild(mtitle);
    mtext.appendChild(msub);
    const mplay = iconButton('play', t('play'));
    mplay.addEventListener('click', (e) => { e.stopPropagation(); togglePlay(); });
    inner.appendChild(mthumb);
    inner.appendChild(mtext);
    inner.appendChild(mplay);
    activate(inner, (e) => { if (!mplay.contains(e.target)) switchTab('now'); });
    mini.appendChild(progress);
    mini.appendChild(inner);
    app.appendChild(mini);

    const tabs = el('nav', 'tabs');
    const tabsInner = el('div', 'tabs-inner');
    const tabButtons = {};
    for (const [id, ic, key] of [['now', 'music-note', 'tabNow'], ['queue', 'queue', 'tabQueue'], ['search', 'search', 'tabSearch'], ['library', 'library', 'tabLibrary']]) {
      const b = el('button', 'tab');
      b.type = 'button';
      b.appendChild(icon(ic));
      b.appendChild(el('span', 'clip', t(key)));
      b.addEventListener('click', () => switchTab(id));
      tabButtons[id] = b;
      tabsInner.appendChild(b);
    }
    tabs.appendChild(tabsInner);
    app.appendChild(tabs);
    root.appendChild(app);
    ui = { app, banner, now, queue: q, search, library, detail, mini, progress, mthumb, mtitle, msub, mplay, tabButtons };
  }

  function setOnline(on) {
    if (ui) ui.banner.hidden = on;
  }

  function showTab(id) {
    tab = id;
    for (const k of Object.keys(ui.tabButtons)) {
      if (k === id) ui.tabButtons[k].setAttribute('aria-current', 'page');
      else ui.tabButtons[k].removeAttribute('aria-current');
    }
    ui.now.view.hidden = id !== 'now';
    ui.queue.view.hidden = id !== 'queue';
    ui.search.view.hidden = id !== 'search';
    ui.library.view.hidden = id !== 'library';
    if (id === 'library' && !ui.library.loaded) loadLibrary();
    window.scrollTo(0, 0);
    renderMini();
  }

  function togglePlay() {
    if (!state || !state.track) return;
    state.playing = !state.playing;   // at once; the next state event confirms
    pos = { p: currentPosition(), at: performance.now(), playing: state.playing, line: pos.line };
    renderTransport();
    cmd('toggle');
  }

  // ---- Now playing ------------------------------------------------------------------------------------------------
  function buildNow() {
    const view = el('section', 'view now');
    const empty = el('div', 'empty-state');
    empty.appendChild(icon('music-note'));
    empty.appendChild(el('h2', null, t('nothing')));
    empty.appendChild(el('p', null, t('nothingHint')));
    const np = el('div', 'np');
    const label = el('div', 'mono-label np-label clip');
    const art = el('div', 'art');
    const meta = el('div', 'np-meta');
    const text = el('div', 'np-text');
    const title = el('div', 'np-title');
    const sub = el('div', 'np-sub clip');
    text.appendChild(title);
    text.appendChild(sub);
    const like = iconButton('heart', t('like'));
    like.addEventListener('click', () => {
      if (!state || !state.canLike) return;
      state.liked = !state.liked;
      renderLike();
      cmd('like');
    });
    meta.appendChild(text);
    meta.appendChild(like);
    const seek = el('input', 'range');
    seek.type = 'range';
    seek.min = '0';
    seek.max = '1000';
    seek.step = '1';
    seek.value = '0';
    seek.setAttribute('aria-label', t('nowLabel'));
    const times = el('div', 'times mono');
    const cur = el('span', null, '00:00'), dur = el('span', null, '00:00');
    times.appendChild(cur);
    times.appendChild(dur);
    const lyric = el('div', 'lyric');
    lyric.setAttribute('aria-live', 'polite');
    const transport = el('div', 'transport');
    const shuffle = iconButton('shuffle', t('shuffle'), 'mode');
    const prev = iconButton('prev', t('prev'));
    const play = el('button', 'play-btn');
    play.type = 'button';
    play.appendChild(icon('play'));
    const next = iconButton('next', t('next'));
    const repeat = iconButton('repeat', t('repeat'), 'mode');
    for (const b of [shuffle, prev, play, next, repeat]) transport.appendChild(b);
    shuffle.addEventListener('click', () => cmd('shuffle'));
    prev.addEventListener('click', () => cmd('prev'));
    next.addEventListener('click', () => cmd('next'));
    repeat.addEventListener('click', () => cmd('repeat'));
    play.addEventListener('click', togglePlay);
    const volume = el('div', 'volume');
    const volIcon = icon('volume-2');
    const vol = el('input', 'range');
    vol.type = 'range';
    vol.min = '0';
    vol.max = '100';
    vol.step = '1';
    vol.setAttribute('aria-label', t('volume'));
    volume.appendChild(volIcon);
    volume.appendChild(vol);
    for (const n of [label, art, meta, seek, times, lyric, transport, volume]) np.appendChild(n);
    view.appendChild(empty);
    view.appendChild(np);

    // Seeking: the bar follows the finger; the command goes when it is let go.
    let seeking = false;
    seek.addEventListener('input', () => {
      seeking = true;
      const ms = (seek.value / 1000) * (state ? state.durationMs : 0);
      cur.textContent = fmt(ms);
      seek.style.setProperty('--p', seek.value / 10 + '%');
    });
    seek.addEventListener('change', () => {
      seeking = false;
      if (!state || !state.canSeek) return;
      const ms = Math.round((seek.value / 1000) * state.durationMs);
      pos = { p: ms, at: performance.now(), playing: pos.playing, line: '' };
      cmd('seek', { ms });
    });
    // Volume: sent while dragging (throttled); incoming states don't move the slider meanwhile.
    let volTimer = 0, volUntil = 0;
    const sendVolume = () => { volTimer = 0; cmd('volume', { value: vol.value / 100 }); };
    vol.addEventListener('input', () => {
      volUntil = performance.now() + 1500;
      vol.style.setProperty('--p', vol.value + '%');
      setIcon(volume, volumeIcon(vol.value / 100));
      if (!volTimer) volTimer = setTimeout(sendVolume, 150);
    });
    const nowParts = { view, empty, np, label, art, title, sub, like, seek, cur, dur, lyric, shuffle, prev, play, next, repeat, vol, volume };
    nowParts.isSeeking = () => seeking;
    nowParts.volumeBusy = () => volTimer || performance.now() < volUntil;
    return nowParts;
  }

  function volumeIcon(v) { return v <= 0.001 ? 'volume-0' : v < 0.34 ? 'volume-1' : v < 0.67 ? 'volume-2' : 'volume-3'; }

  function renderNow() {
    if (!ui) return;
    const n = ui.now;
    const tr = state && state.track;
    n.empty.hidden = !!tr;
    n.np.hidden = !tr;
    renderMini();
    if (!tr) return;
    n.label.textContent = t('nowLabel') + (state.context ? ' · ' + state.context : '');
    if (n.art.dataset.url !== (tr.cover || '')) {
      n.art.dataset.url = tr.cover || '';
      setCover(n.art, tr.cover, state.live ? 'radio' : state.podcast ? 'podcast' : 'music-note');
    }
    n.title.textContent = tr.title;
    n.sub.textContent = '';
    if (state.live) {
      const b = el('span', 'badge', t('live'));
      n.sub.appendChild(b);
      n.sub.appendChild(document.createTextNode(' ' + (state.liveTitle || tr.artist || '')));
    } else {
      const artist = (tr.artists || []).find((a) => a.id);
      const a = el('button', null, tr.artist || '');
      a.type = 'button';
      if (artist) a.addEventListener('click', () => openDetail('artist', artist.id));
      else a.disabled = true;
      n.sub.appendChild(a);
      if (tr.album && tr.album.name) {
        n.sub.appendChild(document.createTextNode(' · '));
        const al = el('button', null, tr.album.name);
        al.type = 'button';
        if (tr.album.id) al.addEventListener('click', () => openDetail('album', tr.album.id));
        else al.disabled = true;
        n.sub.appendChild(al);
      }
    }
    n.like.hidden = !state.canLike;
    renderLike();
    n.seek.disabled = !state.canSeek;
    n.seek.hidden = state.live;
    n.cur.parentNode.hidden = state.live;
    n.dur.textContent = fmt(state.durationMs);
    setIcon(n.shuffle, state.shuffle === 'smart' ? 'shuffle-smart' : 'shuffle');
    n.shuffle.classList.toggle('on', state.shuffle !== 'off');
    n.shuffle.setAttribute('aria-pressed', String(state.shuffle !== 'off'));
    n.shuffle.title = state.shuffle === 'smart' ? t('smartShuffle') : t('shuffle');
    setIcon(n.repeat, state.repeat === 'one' ? 'repeat-one' : 'repeat');
    n.repeat.classList.toggle('on', state.repeat !== 'off');
    n.repeat.setAttribute('aria-pressed', String(state.repeat !== 'off'));
    n.next.disabled = !state.hasNext;
    if (!n.volumeBusy()) {
      n.vol.value = String(Math.round((state.volume || 0) * 100));
      n.vol.style.setProperty('--p', n.vol.value + '%');
      setIcon(n.volume, volumeIcon(state.volume || 0));
    }
    renderTransport();
    renderLyric();
  }

  function renderLike() {
    const n = ui && ui.now;
    if (!n || !state) return;
    setIcon(n.like, state.liked ? 'heart-filled' : 'heart');
    n.like.classList.toggle('on', !!state.liked);
    n.like.setAttribute('aria-pressed', String(!!state.liked));
  }

  function renderTransport() {
    if (!ui || !state) return;
    const playing = !!state.playing;
    const label = playing ? t('pause') : t('play');
    for (const b of [ui.now.play, ui.mplay]) {
      setIcon(b, playing ? 'pause' : 'play');
      b.setAttribute('aria-label', label);
      b.title = label;
    }
  }

  function renderLyric() {
    if (!ui) return;
    const text = state && state.track && !state.live ? pos.line : '';
    if (ui.now.lyric.textContent !== text) ui.now.lyric.textContent = text;
  }

  function renderMini() {
    if (!ui) return;
    const tr = state && state.track;
    const show = !!tr && (tab !== 'now' || !!detail);
    ui.mini.hidden = !show;
    ui.app.classList.toggle('has-mini', show);
    if (!tr) return;
    if (ui.mthumb.dataset.url !== (tr.thumb || '')) {
      ui.mthumb.dataset.url = tr.thumb || '';
      setCover(ui.mthumb, tr.thumb, state.live ? 'radio' : 'music-note');
    }
    ui.mtitle.textContent = tr.title;
    ui.msub.textContent = state.live ? state.liveTitle || tr.artist : tr.artist;
  }

  // Position: drawn every frame while something plays and the bar is on screen.
  function frame() {
    requestAnimationFrame(frame);
    if (!ui || !state || !state.track || document.hidden) return;
    const p = currentPosition();
    const d = state.durationMs || 0;
    const frac = d > 0 ? Math.min(1, p / d) : 0;
    if (!ui.now.view.hidden && !ui.now.isSeeking()) {
      const v = String(Math.round(frac * 1000));
      if (ui.now.seek.value !== v) {
        ui.now.seek.value = v;
        ui.now.seek.style.setProperty('--p', frac * 100 + '%');
      }
      const c = fmt(p);
      if (ui.now.cur.textContent !== c) ui.now.cur.textContent = c;
    }
    if (!ui.mini.hidden) ui.progress.style.width = (state.live ? 0 : frac * 100) + '%';
  }
  requestAnimationFrame(frame);

  // ---- Queue ------------------------------------------------------------------------------------------------------
  function buildQueue() {
    const view = el('section', 'view queue');
    view.hidden = true;
    view.appendChild(el('h1', 'page-title', t('tabQueue')));
    const list = el('div');
    view.appendChild(list);
    return { view, list };
  }

  function trackRow(tr, opts) {
    const row = el('div', 'row' + (tr.playable === false ? ' unplayable' : '') + (opts.current ? ' current' : ''));
    const th = el('div', 'thumb');
    setCover(th, tr.cover);
    const text = el('div', 'row-text');
    const title = el('div', 'row-title clip');
    if (tr.rec) title.appendChild(el('span', 'rec', '✦'));
    title.appendChild(document.createTextNode(tr.title || ''));
    text.appendChild(title);
    text.appendChild(el('div', 'row-sub clip', tr.artist || ''));
    row.appendChild(th);
    row.appendChild(text);
    if (tr.durationMs > 0) row.appendChild(el('span', 'dur mono', fmt(tr.durationMs)));
    if (opts.action) {
      const b = iconButton(opts.action.icon, opts.action.label);
      b.addEventListener('click', (e) => { e.stopPropagation(); opts.action.run(); });
      row.appendChild(b);
    }
    if (opts.onTap) activate(row, (e) => { if (!e.target.closest('.icon-btn')) opts.onTap(); });
    return row;
  }

  function renderQueue() {
    if (!ui || !queue) return;
    const list = ui.queue.list;
    list.textContent = '';
    const items = queue.items || [];
    if (!items.length) {
      const m = el('div', 'empty-state');
      m.appendChild(icon('queue'));
      m.appendChild(el('p', null, t('queueEmpty')));
      list.appendChild(m);
      return;
    }
    let shownNext = false;
    for (const it of items) {
      const isCurrent = it.i === queue.current;
      if (isCurrent) list.appendChild(el('span', 'mono-label section-label accent', t('queueNow')));
      else if (!shownNext) { list.appendChild(el('span', 'mono-label section-label', t('queueNext'))); shownNext = true; }
      list.appendChild(trackRow(it, {
        current: isCurrent,
        onTap: () => { if (!isCurrent) cmd('jump', { index: it.i, id: it.id }); },
        action: isCurrent ? null : { icon: 'close', label: t('remove'), run: () => cmd('remove', { index: it.i, id: it.id }) },
      }));
    }
    if (queue.more > 0 && queue.moreText) list.appendChild(el('div', 'mono-label more', queue.moreText));
  }

  // ---- Search -----------------------------------------------------------------------------------------------------
  const KINDS = [['tracks', 'songs'], ['albums', 'albums'], ['playlists', 'playlists'], ['artists', 'artists']];

  function chips(kinds, selected, onPick) {
    const box = el('div', 'chips');
    box.setAttribute('role', 'group');
    const buttons = [];
    for (const [id, key] of kinds) {
      const b = el('button', 'chip', t(key));
      b.type = 'button';
      b.setAttribute('aria-pressed', String(id === selected));
      b.addEventListener('click', () => {
        for (const x of buttons) x.setAttribute('aria-pressed', String(x === b));
        onPick(id);
      });
      buttons.push(b);
      box.appendChild(b);
    }
    return box;
  }

  function buildSearch() {
    const view = el('section', 'view search');
    view.hidden = true;
    view.appendChild(el('h1', 'page-title', t('tabSearch')));
    const box = el('div', 'searchbox');
    box.appendChild(icon('search'));
    const input = el('input');
    input.type = 'search';
    input.placeholder = t('searchPlaceholder').replace(/\s{2,}/g, ' ');
    input.setAttribute('aria-label', t('tabSearch'));
    input.enterKeyHint = 'search';
    const clear = iconButton('close', t('cancel'));
    clear.hidden = true;
    box.appendChild(input);
    box.appendChild(clear);
    view.appendChild(box);
    const s = { view, input, kind: 'tracks', data: null, query: '', ctrl: null, timer: 0 };
    view.appendChild(chips(KINDS, s.kind, (k) => { s.kind = k; renderResults(); }));
    s.results = el('div');
    view.appendChild(s.results);
    input.addEventListener('input', () => {
      clear.hidden = !input.value;
      clearTimeout(s.timer);
      s.timer = setTimeout(runSearch, 350);
    });
    input.addEventListener('keydown', (e) => { if (e.key === 'Enter') { clearTimeout(s.timer); runSearch(); input.blur(); } });
    clear.addEventListener('click', () => { input.value = ''; clear.hidden = true; s.data = null; s.query = ''; renderResults(); input.focus(); });

    async function runSearch() {
      const q = input.value.trim();
      if (q === s.query) return;
      s.query = q;
      if (s.ctrl) s.ctrl.abort();
      if (!q) { s.data = null; renderResults(); return; }
      const ctrl = new AbortController();
      s.ctrl = ctrl;
      s.results.textContent = '';
      for (let i = 0; i < 6; i++) s.results.appendChild(el('div', 'skeleton'));
      try {
        const j = await api('/api/search?q=' + encodeURIComponent(q), { signal: ctrl.signal });
        if (s.ctrl !== ctrl) return;
        s.data = j;
        renderResults();
      } catch (e) {
        if (ctrl.signal.aborted || e.status === 401) return;
        message(s.results, (e.data && e.data.toast) || t('searchFailed'), () => { s.query = ''; runSearch(); });
      }
    }
    function renderResults() {
      s.results.textContent = '';
      if (!s.data) return;
      renderList(s.results, s.data, s.kind);
    }
    return s;
  }

  function message(box, text, retry) {
    box.textContent = '';
    const m = el('div', 'message', text);
    if (retry) {
      const b = el('button', 'pill', t('retry'));
      b.type = 'button';
      b.addEventListener('click', retry);
      m.appendChild(el('br'));
      m.appendChild(b);
    }
    box.appendChild(m);
  }

  // One kind of result (songs / albums / playlists / artists) as rows.
  function renderList(box, data, kind) {
    const items = (data && data[kind]) || [];
    if (!items.length) { message(box, t('noResults')); return; }
    if (kind === 'tracks') {
      items.forEach((tr, index) => box.appendChild(trackRow(tr, {
        onTap: () => cmd('playList', { list: data.list, index }),
        action: { icon: 'more-vertical', label: t('playNext'), run: () => songSheet(tr, data.list, index) },
      })));
      return;
    }
    for (const it of items) box.appendChild(itemRow(kind, it));
  }

  function itemRow(kind, it) {
    const row = el('div', 'row');
    const th = el('div', 'thumb' + (kind === 'artists' ? ' round' : '') + (kind === 'playlists' && it.id === 'liked' ? ' liked ph' : ''));
    if (kind === 'playlists' && it.id === 'liked') th.appendChild(icon('heart-filled'));
    else setCover(th, it.cover, kind === 'artists' ? 'artist' : kind === 'albums' ? 'album' : 'playlist');
    const text = el('div', 'row-text');
    const title = it.title || it.name || '';
    text.appendChild(el('div', 'row-title clip', title));
    let sub = '';
    if (kind === 'albums') sub = [it.type === 'Single' ? t('single') : t('album'), it.year, it.artist].filter(Boolean).join(' · ');
    else if (kind === 'playlists') sub = [it.owner, it.count].filter(Boolean).join(' · ') || t('playlist');
    else sub = t('artist');
    text.appendChild(el('div', 'row-sub clip', sub));
    row.appendChild(th);
    row.appendChild(text);
    activate(row, () => openDetail(kind === 'albums' ? 'album' : kind === 'playlists' ? 'playlist' : 'artist', it.id));
    return row;
  }

  // ---- Library ----------------------------------------------------------------------------------------------------
  function buildLibrary() {
    const view = el('section', 'view library');
    view.hidden = true;
    view.appendChild(el('h1', 'page-title', t('tabLibrary')));
    const s = { view, kind: 'playlists', data: null, loaded: false };
    view.appendChild(chips([['playlists', 'playlists'], ['albums', 'albums'], ['artists', 'artists']], s.kind, (k) => { s.kind = k; renderLibrary(); }));
    s.results = el('div');
    view.appendChild(s.results);
    // This phone: forget it here (the PC's Ayarlar can do the same).
    const forget = el('button', 'pill forget');
    forget.type = 'button';
    forget.appendChild(icon('logout'));
    forget.appendChild(el('span', null, t('disconnectPhone')));
    forget.addEventListener('click', phoneMenu);
    view.appendChild(forget);
    return s;
  }

  async function loadLibrary() {
    const s = ui.library;
    s.loaded = true;
    s.results.textContent = '';
    for (let i = 0; i < 6; i++) s.results.appendChild(el('div', 'skeleton'));
    try {
      s.data = await api('/api/library');
      renderLibrary();
    } catch (e) {
      s.loaded = false;
      if (e.status !== 401) message(s.results, t('error'), loadLibrary);
    }
  }

  function renderLibrary() {
    const s = ui.library;
    if (!s.data) return;
    s.results.textContent = '';
    const total = (s.data.playlists || []).length + (s.data.albums || []).length + (s.data.artists || []).length;
    if (!total) { message(s.results, t('libraryEmpty')); return; }
    renderList(s.results, s.data, s.kind);
  }

  // ---- Album / playlist / artist (over the current tab) ----------------------------------------------------------
  async function showDetail() {
    const d = ui.detail;
    const top = detail;
    d.textContent = '';
    for (const v of [ui.now.view, ui.queue.view, ui.search.view, ui.library.view]) v.hidden = true;
    d.hidden = false;
    renderMini();
    window.scrollTo(0, 0);
    const head = el('div', 'detail-top');
    const back = iconButton('arrow-back', t('back'));
    back.addEventListener('click', goBack);
    const label = el('span', 'mono-label clip');
    head.appendChild(back);
    head.appendChild(label);
    d.appendChild(head);
    const body = el('div');
    d.appendChild(body);
    for (let i = 0; i < 8; i++) body.appendChild(el('div', 'skeleton'));
    let j;
    try {
      j = await api('/api/' + top.kind + '?id=' + encodeURIComponent(top.id));
    } catch (e) {
      if (e.status === 401 || detail !== top) return;
      message(body, (e.data && e.data.toast) || t('error'), showDetail);
      return;
    }
    if (detail !== top) return;   // the user went on meanwhile
    body.textContent = '';
    const hero = el('div', 'hero');
    const art = el('div', 'hero-art');
    let title = '', sub = '';
    if (top.kind === 'album') {
      const a = j.album || {};
      label.textContent = [a.type === 'Single' ? t('single') : t('album'), a.year].filter(Boolean).join(' · ');
      setCover(art, a.cover, 'album');
      title = a.title;
      sub = [a.artist, j.count].filter(Boolean).join(' · ');
    } else if (top.kind === 'playlist') {
      const p = j.playlist || {};
      label.textContent = t('playlist');
      if (top.id === 'liked') { art.classList.add('liked', 'ph'); art.appendChild(icon('heart-filled')); }
      else setCover(art, p.cover, 'playlist');
      title = p.title;
      sub = [p.owner, p.count].filter(Boolean).join(' · ');
    } else {
      const a = j.artist || {};
      label.textContent = t('artist');
      art.classList.add('round');
      setCover(art, a.cover, 'artist');
      title = a.name;
    }
    hero.appendChild(art);
    hero.appendChild(el('h1', 'hero-title', title || ''));
    if (sub) hero.appendChild(el('div', 'hero-sub', sub));
    const tracks = j.tracks || [];
    if (tracks.length) {
      const actions = el('div', 'actions');
      const play = el('button', 'play-btn sm');
      play.type = 'button';
      play.setAttribute('aria-label', t('play'));
      play.appendChild(icon('play'));
      play.addEventListener('click', () => cmd('playList', { list: j.list, index: -1 }));
      const shuffle = el('button', 'pill');
      shuffle.type = 'button';
      shuffle.appendChild(icon('shuffle'));
      shuffle.appendChild(el('span', null, t('shuffle')));
      shuffle.addEventListener('click', () => cmd('playList', { list: j.list, index: -1, shuffle: true }));
      const add = iconButton('plus', t('addToQueue'), 'outline');
      add.addEventListener('click', () => cmd('enqueue', { list: j.list, index: -1 }));
      actions.appendChild(play);
      actions.appendChild(shuffle);
      actions.appendChild(add);
      hero.appendChild(actions);
    }
    body.appendChild(hero);
    if (top.kind === 'artist' && tracks.length) body.appendChild(el('span', 'mono-label section-label', t('popular')));
    tracks.forEach((tr, index) => body.appendChild(trackRow(tr, {
      onTap: () => cmd('playList', { list: j.list, index }),
      action: { icon: 'more-vertical', label: t('playNext'), run: () => songSheet(tr, j.list, index) },
    })));
    if (top.kind === 'artist' && (j.albums || []).length) {
      body.appendChild(el('span', 'mono-label section-label', t('albums')));
      for (const a of j.albums) body.appendChild(itemRow('albums', a));
    }
  }

  // ---- disconnect this phone ----------------------------------------------------------------------------------------
  function phoneMenu() {
    openSheet({ title: t('title') }, [{
      icon: 'logout', label: t('disconnectPhone'), danger: true,
      run: async () => {
        try { await api('/api/unpair', { method: 'POST', body: {} }); } catch (e) { /* gone anyway */ }
        revoked('revoked');
      },
    }]);
  }

  // ---- boot -------------------------------------------------------------------------------------------------------
  async function boot() {
    try {
      const r = await fetch('/api/i18n', { cache: 'no-store' });
      const j = await r.json();
      S = j.s || {};
      if (j.lang) document.documentElement.lang = j.lang;
      document.title = 'ShadeTube · ' + t('title');
    } catch (e) { /* the page still works with empty texts */ }
    const m = /[#&]p=([A-Za-z0-9_-]{16,64})/.exec(location.hash);
    if (m) {
      history.replaceState(null, '', location.pathname);   // the secret leaves the address bar and the history
      pair(m[1]);
      return;
    }
    if (!token) { showPairing(); return; }
    start();
  }
  boot();
})();
