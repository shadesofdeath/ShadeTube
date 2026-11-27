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
  window.ST_I18N = window.ST_I18N || {};
  window.ST_I18N.en = english;
  var current = "en";

  function apply(code) {
    var t = window.ST_I18N[code] || english;
    nodes.forEach(function (n) {
      var k = n.getAttribute("data-i18n");
      n.textContent = t[k] || english[k];
    });
    doc.title = t._title || english._title;
    doc.querySelector('meta[name="description"]').setAttribute("content", t._desc || english._desc);
    root.lang = code === "pt" ? "pt-BR" : code;
    current = code;
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
        var zip = (rel.assets || []).filter(function (a) { return /win64\.zip$/i.test(a.name); })[0];
        if (zip) doc.querySelectorAll(".js-download").forEach(function (a) { a.href = zip.browser_download_url; });
        if (rel.tag_name) doc.querySelectorAll(".js-version").forEach(function (v) { v.textContent = rel.tag_name; });
      })
      .catch(function () {});
  }

  paintIcons();
  setLang(initialLang(), false);
})();
