// ShadeTube site: language switching, icons, small interactions. No dependencies, no tracking.
(function () {
  "use strict";
  var LANGS = [
    ["en", "English"], ["tr", "Türkçe"], ["de", "Deutsch"], ["es", "Español"], ["fr", "Français"],
    ["pt", "Português (BR)"], ["ru", "Русский"], ["uk", "Українська"], ["id", "Bahasa Indonesia"],
    ["ja", "日本語"], ["ko", "한국어"]
  ];
  var REPO = "shadesofdeath/ShadeTube";
  var doc = document, root = doc.documentElement;
  var store = { get: function (k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
                set: function (k, v) { try { localStorage.setItem(k, v); } catch (e) {} } };

  // ---- icons (the app's own set) ----
  function paintIcons() {
    var set = window.ST_ICONS || {};
    doc.querySelectorAll("[data-icon]").forEach(function (el) {
      var d = set[el.getAttribute("data-icon")];
      if (d) el.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true">' + d + "</svg>";
    });
  }

  // ---- languages ----
  var nodes = Array.prototype.slice.call(doc.querySelectorAll("[data-i18n]"));
  var english = {};   // the page's own text is the English copy
  nodes.forEach(function (n) { english[n.getAttribute("data-i18n")] = n.textContent; });
  english["_title"] = doc.title;
  english["_desc"] = doc.querySelector('meta[name="description"]').getAttribute("content");
  english["i.copied"] = "Copied";
  english["c.search"] = "Search the changelog";
  english["c.latest"] = "Latest";
  english["c.count"] = "{n} changes";
  english["c.release"] = "Download";
  var isChangelog = doc.body.classList.contains("page-changelog");
  var langHooks = [];
  function tr(k) { var t = window.ST_I18N[current] || english; return t[k] || english[k] || ""; }
  window.ST_I18N = window.ST_I18N || {};
  window.ST_I18N.en = english;
  var current = "en";

  function apply(code) {
    var t = window.ST_I18N[code] || english;
    nodes.forEach(function (n) {
      var k = n.getAttribute("data-i18n");
      n.textContent = t[k] || english[k];
    });
    if (isChangelog) doc.title = (t["nav.changelog"] || english["nav.changelog"]) + " · ShadeTube";
    else {
      doc.title = t._title || english._title;
      doc.querySelector('meta[name="description"]').setAttribute("content", t._desc || english._desc);
    }
    doc.querySelectorAll("[data-i18n-ph]").forEach(function (n) {
      var k = n.getAttribute("data-i18n-ph");
      n.placeholder = t[k] || english[k] || n.placeholder;
    });
    root.lang = code === "pt" ? "pt-BR" : code;
    current = code;
    langHooks.forEach(function (f) { f(); });
    doc.getElementById("langCode").textContent = code.toUpperCase();
    doc.querySelectorAll("#langMenu li").forEach(function (li) {
      li.setAttribute("aria-selected", li.dataset.code === code ? "true" : "false");
    });
  }

  function setLang(code, remember) {
    if (!LANGS.some(function (l) { return l[0] === code; })) code = "en";
    if (remember) store.set("st-lang", code);
    if (window.ST_I18N[code]) return apply(code);
    var s = doc.createElement("script");   // a script, not fetch(): works from file:// too
    s.src = "i18n/" + code + ".js";
    s.onload = function () { apply(code); };
    s.onerror = function () { apply("en"); };
    doc.head.appendChild(s);
  }

  function initialLang() {
    var q = /[?&]lang=([a-z]{2})/i.exec(location.search);
    if (q) return q[1].toLowerCase();
    var saved = store.get("st-lang");
    if (saved) return saved;
    var prefs = navigator.languages || [navigator.language || "en"];
    for (var i = 0; i < prefs.length; i++) {
      var two = String(prefs[i]).slice(0, 2).toLowerCase();
      if (LANGS.some(function (l) { return l[0] === two; })) return two;
    }
    return "en";
  }

  var btn = doc.getElementById("langBtn"), menu = doc.getElementById("langMenu");
  LANGS.forEach(function (l) {
    var li = doc.createElement("li");
    li.setAttribute("role", "option");
    li.tabIndex = 0;
    li.dataset.code = l[0];
    li.innerHTML = l[1] + "<span>" + l[0].toUpperCase() + "</span>";
    li.lang = l[0];
    menu.appendChild(li);
  });
  function toggleMenu(open) {
    menu.hidden = !open;
    btn.setAttribute("aria-expanded", open ? "true" : "false");
  }
  btn.addEventListener("click", function (e) { e.stopPropagation(); toggleMenu(menu.hidden); });
  menu.addEventListener("click", function (e) {
    var li = e.target.closest("li");
    if (li) { setLang(li.dataset.code, true); toggleMenu(false); btn.focus(); }
  });
  menu.addEventListener("keydown", function (e) {
    var li = e.target.closest("li");
    if (!li) return;
    if (e.key === "Enter" || e.key === " ") { e.preventDefault(); li.click(); }
    else if (e.key === "ArrowDown" && li.nextElementSibling) { e.preventDefault(); li.nextElementSibling.focus(); }
    else if (e.key === "ArrowUp" && li.previousElementSibling) { e.preventDefault(); li.previousElementSibling.focus(); }
  });
  doc.addEventListener("click", function () { toggleMenu(false); });
  doc.addEventListener("keydown", function (e) { if (e.key === "Escape") toggleMenu(false); });

  // ---- sticky nav ----
  var nav = doc.getElementById("nav");
  function onScroll() { nav.classList.toggle("stuck", window.scrollY > 8); }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  // ---- reveal on scroll ----
  var reveals = doc.querySelectorAll(".reveal");
  if ("IntersectionObserver" in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (en.isIntersecting) { en.target.classList.add("in"); io.unobserve(en.target); }
      });
    }, { rootMargin: "0px 0px -8% 0px", threshold: 0.05 });
    reveals.forEach(function (el, i) {
      if (el.closest(".f-grid, .tg-grid, .facts-in")) el.style.transitionDelay = (i % 4) * 60 + "ms";
      io.observe(el);
    });
  } else {
    reveals.forEach(function (el) { el.classList.add("in"); });
  }

  // ---- accent swatches: the app's accent follows the cover; here it follows you ----
  function setAccent(c) {
    root.style.setProperty("--accent", c);
    doc.querySelectorAll(".sw").forEach(function (b) { b.classList.toggle("on", b.dataset.accent === c); });
  }
  doc.querySelectorAll(".sw").forEach(function (b) {
    b.addEventListener("click", function () { setAccent(b.dataset.accent); store.set("st-accent", b.dataset.accent); });
  });
  var savedAccent = store.get("st-accent");
  if (savedAccent && doc.querySelector('.sw[data-accent="' + savedAccent + '"]')) setAccent(savedAccent);

  // ---- copy buttons ----
  doc.querySelectorAll(".copy").forEach(function (b) {
    b.addEventListener("click", function () {
      var text = b.parentNode.querySelector("code").textContent;
      var done = function () {
        var t = window.ST_I18N[current] || english;
        b.textContent = t["i.copied"] || english["i.copied"];
        b.classList.add("done");
        setTimeout(function () { b.classList.remove("done"); b.textContent = t["i.copy"] || english["i.copy"]; }, 1600);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, function () {});
      else {
        var r = doc.createRange(); r.selectNodeContents(b.parentNode.querySelector("code"));
        var s = getSelection(); s.removeAllRanges(); s.addRange(r);
        try { doc.execCommand("copy"); done(); } catch (e) {}
      }
    });
  });

  // ---- latest release: a direct download link and the version, when GitHub answers ----
  if (window.fetch && location.protocol !== "file:") {
    fetch("https://api.github.com/repos/" + REPO + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (rel) {
        if (!rel) return;
        var pick = function (re) { return (rel.assets || []).filter(function (a) { return re.test(a.name); })[0]; };
        var zip = pick(/win64\.zip$/i), setup = pick(/setup\.exe$/i) || zip;
        if (setup) doc.querySelectorAll(".js-download").forEach(function (a) { a.href = setup.browser_download_url; });
        if (zip) doc.querySelectorAll(".js-portable").forEach(function (a) { a.href = zip.browser_download_url; });
        if (rel.tag_name) doc.querySelectorAll(".js-version").forEach(function (v) { v.textContent = rel.tag_name; });
      })
      .catch(function () {});
  }

  // ---- mobile menu ----
  var burger = doc.getElementById("burger"), links = doc.querySelector(".nav-links");
  if (burger && links) {
    var setMenu = function (open) {
      nav.classList.toggle("open", open);
      burger.setAttribute("aria-expanded", open ? "true" : "false");
    };
    burger.addEventListener("click", function (e) { e.stopPropagation(); setMenu(!nav.classList.contains("open")); });
    links.addEventListener("click", function () { setMenu(false); });
    doc.addEventListener("click", function () { setMenu(false); });
  }

  // ---- gallery: a sideways strip of full-size screenshots, chips to jump, a lightbox to enlarge ----
  var track = doc.getElementById("gTrack");
  if (track) {
    var slides = Array.prototype.slice.call(track.querySelectorAll(".g-slide"));
    var chips = Array.prototype.slice.call(doc.querySelectorAll("#gChips .chip"));
    var active = 0;
    var centerOf = function (el) { return el.offsetLeft + el.offsetWidth / 2; };
    var go = function (i, smooth) {
      i = Math.max(0, Math.min(slides.length - 1, i));
      track.scrollTo({ left: centerOf(slides[i]) - track.clientWidth / 2, behavior: smooth === false ? "auto" : "smooth" });
    };
    var mark = function () {
      var mid = track.scrollLeft + track.clientWidth / 2, best = 0, bestD = Infinity;
      slides.forEach(function (s, i) { var d = Math.abs(centerOf(s) - mid); if (d < bestD) { bestD = d; best = i; } });
      if (best === active && chips[best].classList.contains("on")) return;
      active = best;
      slides.forEach(function (s, i) { s.classList.toggle("on", i === best); });
      chips.forEach(function (c, i) { c.classList.toggle("on", i === best); });
      var chip = chips[best], box = chip.parentNode;   // keep the active chip in view without moving the page
      box.scrollTo({ left: chip.offsetLeft - box.clientWidth / 2 + chip.offsetWidth / 2, behavior: "smooth" });
    };
    var ticking = false;
    track.addEventListener("scroll", function () {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(function () { ticking = false; mark(); });
    }, { passive: true });
    chips.forEach(function (c, i) { c.addEventListener("click", function () { go(i); }); });
    doc.getElementById("gPrev").addEventListener("click", function () { go(active - 1); });
    doc.getElementById("gNext").addEventListener("click", function () { go(active + 1); });
    track.addEventListener("keydown", function (e) {
      if (e.key === "ArrowRight") { e.preventDefault(); go(active + 1); }
      else if (e.key === "ArrowLeft") { e.preventDefault(); go(active - 1); }
    });
    // Drag to scroll with a mouse; a drag does not count as a click on the picture.
    var drag = null, dragged = false;
    track.addEventListener("pointerdown", function (e) {
      if (e.pointerType !== "mouse" || e.button !== 0) return;
      drag = { x: e.clientX, left: track.scrollLeft }; dragged = false;
    });
    window.addEventListener("pointermove", function (e) {
      if (!drag) return;
      var dx = e.clientX - drag.x;
      if (Math.abs(dx) > 6) { dragged = true; track.classList.add("dragging"); }
      if (dragged) track.scrollLeft = drag.left - dx;
    });
    window.addEventListener("pointerup", function () {
      if (!drag) return;
      drag = null;
      track.classList.remove("dragging");
      if (dragged) go(active);
    });
    mark();
    window.addEventListener("resize", function () { go(active, false); });

    var lb = doc.getElementById("lightbox"), lbImg = doc.getElementById("lbImg"), lbAt = 0, lbFrom = null;
    var lbShow = function (i) {
      lbAt = (i + slides.length) % slides.length;
      var s = slides[lbAt], img = s.querySelector("img");
      lbImg.src = img.getAttribute("src");
      lbImg.width = img.width; lbImg.height = img.height;
      doc.getElementById("lbT").textContent = s.querySelector("b").textContent;
      doc.getElementById("lbD").textContent = s.querySelector("span").textContent;
      doc.getElementById("lbN").textContent = (lbAt + 1) + " / " + slides.length;
      lbImg.alt = s.querySelector("b").textContent;
    };
    var lbOpen = function (i) {
      lbFrom = doc.activeElement;
      lbShow(i);
      lb.hidden = false;
      doc.body.classList.add("locked");
      doc.getElementById("lbClose").focus();
    };
    var lbCloseFn = function () {
      lb.hidden = true;
      doc.body.classList.remove("locked");
      go(lbAt, false);
      if (lbFrom) lbFrom.focus();
    };
    track.addEventListener("click", function (e) {
      var b = e.target.closest(".g-img");
      if (!b) return;
      if (dragged) { dragged = false; return; }
      lbOpen(+b.dataset.zoom);
    });
    doc.getElementById("lbClose").addEventListener("click", lbCloseFn);
    doc.getElementById("lbPrev").addEventListener("click", function (e) { e.stopPropagation(); lbShow(lbAt - 1); });
    doc.getElementById("lbNext").addEventListener("click", function (e) { e.stopPropagation(); lbShow(lbAt + 1); });
    lb.addEventListener("click", function (e) { if (e.target === lb || e.target.tagName === "FIGURE") lbCloseFn(); });
    doc.addEventListener("keydown", function (e) {
      if (lb.hidden) return;
      if (e.key === "Escape") lbCloseFn();
      else if (e.key === "ArrowRight") lbShow(lbAt + 1);
      else if (e.key === "ArrowLeft") lbShow(lbAt - 1);
    });
    langHooks.push(function () { if (!lb.hidden) lbShow(lbAt); });
  }

  // ---- changelog page: rendered from changelog.js (generated from CHANGELOG.md) ----
  var clList = doc.getElementById("clList");
  if (clList && window.ST_CHANGELOG) {
    var versions = window.ST_CHANGELOG, toc = doc.getElementById("clToc"), q = doc.getElementById("clQ");
    var fmtDate = function (d) {
      var m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(d);
      if (!m) return d;
      try {
        return new Date(Date.UTC(+m[1], +m[2] - 1, +m[3])).toLocaleDateString(root.lang || "en",
          { year: "numeric", month: "long", day: "numeric", timeZone: "UTC" });
      } catch (e) { return d; }
    };
    var plain = function (h) { return h.replace(/<[^>]+>/g, "").toLowerCase(); };
    var render = function () {
      var needle = (q.value || "").trim().toLowerCase(), html = "", tocHtml = "", shown = 0;
      versions.forEach(function (v, vi) {
        var items = v.items.filter(function (it) { return !needle || plain(it.t + " " + it.d).indexOf(needle) >= 0; });
        if (!items.length) return;
        shown += items.length;
        var id = "v" + v.v.replace(/[^0-9a-z]+/gi, "-");
        var open = needle || vi < 2 || location.hash === "#" + id;
        tocHtml += '<li><a href="#' + id + '"><b>' + v.v + "</b><span>" + items.length + "</span></a></li>";
        html += '<details class="cl-v" id="' + id + '"' + (open ? " open" : "") + "><summary><span class=\"cl-num\">" + v.v + "</span>" +
          (vi === 0 ? '<span class="badge">' + tr("c.latest") + "</span>" : "") +
          '<span class="cl-date">' + fmtDate(v.date) + '</span><span class="cl-count">' +
          tr("c.count").replace("{n}", items.length) + "</span></summary><ul>" +
          items.map(function (it) {
            return "<li>" + (it.t ? "<h3>" + it.t + "</h3>" : "") + "<p>" + it.d + "</p></li>";
          }).join("") + "</ul>" +
          (/^\d+\.\d+\.\d+$/.test(v.v) ? '<a class="cl-rel" href="https://github.com/' + REPO + "/releases/tag/v" + v.v +
            '" rel="noopener">' + tr("c.release") + " v" + v.v + " →</a>" : "") + "</details>";
      });
      clList.innerHTML = html;
      toc.innerHTML = tocHtml;
      doc.getElementById("clNone").hidden = shown > 0;
    };
    q.addEventListener("input", render);
    toc.addEventListener("click", function (e) {
      var a = e.target.closest("a");
      if (!a) return;
      var d = doc.getElementById(a.getAttribute("href").slice(1));
      if (d) d.open = true;
    });
    langHooks.push(render);
    render();
    if (location.hash) { var target = doc.getElementById(location.hash.slice(1)); if (target) target.scrollIntoView(); }
  }

  paintIcons();
  setLang(initialLang(), false);
})();
