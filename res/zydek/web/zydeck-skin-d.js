// ZyDeck's "Hardware" look (skin D) over the deck page, without changing any size or position: Element stays as it
// is underneath. The deck page turns it on and off with zydeckSkin.apply({ theme: "d" | "element", lamp, text, ptl }):
//   lamp: "left" | "right" | "bottom"  where the long, flat keys have their lamp
//   text: "adaptive" | "element"        each key's text sized to the key, or Element's sizes
//   ptl:  "top"                          how a pad's text sits (top-down)
(() => {
  const body = document.body;
  let on = false, side = "l", ADAPT = true, PHONE = false;
  window.zydeckSkin = { apply(cfg = {}) {
    on = cfg.theme === "d";
    side = { left: "l", right: "r", bottom: "b" }[cfg.lamp || "left"] || "l";
    ADAPT = (cfg.text || "adaptive") === "adaptive";
    PHONE = !!document.getElementById("app")?.classList.contains("phone");
    body.classList.toggle("skin-d", on);
    for (const c of [...body.classList]) if (c.startsWith("ptl-")) body.classList.remove(c);
    body.classList.add("ptl-" + (cfg.ptl || "top"));
    body.classList.toggle("sk-phone", PHONE);
    body.classList.toggle("adaptive", on && ADAPT);
    all = true; later();
  } };
  const app = document.getElementById("app");
  const mm = () => parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--mm")) || 5.93;

  // keys that can be on (a lamp); the rest are actions (no lamp). Selectors get the strip.
  const LAMPED = ".play, .cuebtn, .sync, .shift, .ol, .sshift, .mute, .pfl, #rec, .flags button, .smpbtn, .loopbox .loop, .modes button, .prow.modes button, .fxstrip button";
  const SELECTOR = ".modes button, .prow.modes button";
  const SKIP = ".psw, .strip button";

  // How much of a key its text fills, by the key's size against a thumb: 15 mm and up about 20 %, 8 mm and under
  // about 70 %, in between in between; then shrunk until it fits.
  const BIG = .7;   // the main text at 70 % of what the fill rule gives
  const fill = (w, h) => { const m = Math.sqrt(w * h) / mm(); return m >= 15 ? .2 : m <= 8 ? .7 : .7 - (m - 8) / 7 * .5; };
  const range = document.createRange();
  // the box the text itself takes (its glyphs, not the spans around them, which may be as wide as the key)
  const textBox = el => {
    let l = 1e9, t = 1e9, r = -1e9, b = -1e9;
    const w = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
    for (let n; (n = w.nextNode());) {
      if (!n.textContent.trim()) continue;
      range.selectNodeContents(n);
      for (const q of range.getClientRects()) { l = Math.min(l, q.left); t = Math.min(t, q.top); r = Math.max(r, q.right); b = Math.max(b, q.bottom); }
    }
    return r < l ? { width: 0, height: 0 } : { width: r - l, height: b - t };
  };

  function key(b) {
    if (!on || b.matches(SKIP)) return;
    const r = b.getBoundingClientRect(); if (!r.width || !r.height) return;
    if (getComputedStyle(b).position === "static") b.style.position = "relative";
    if (b.classList.contains("on")) b.dataset.seenOn = "1";
    let ls = "";
    if (b.matches(LAMPED) || b.dataset.seenOn) {
      if (b.matches(SELECTOR)) ls = "s";
      else if (r.width / r.height >= 1.8) ls = side === "b" ? (r.height / mm() < 8.5 || PHONE ? "bh" : "b") : side;
      else ls = PHONE || r.height / mm() < 8.5 ? "bh" : "b";
    }
    if (b.dataset.ls !== ls) b.dataset.ls = ls;
    const lw = (ls === "b" || ls === "bh") && Math.abs(Math.log(r.width / r.height)) > Math.log(1.15) ? "2" : "";   // not square: a lamp twice as wide
    if ((b.dataset.lw || "") !== lw) b.dataset.lw = lw;
    // below the waveforms (SHIFT, TEMPO BOOST, headphones, MUTE, FX…), a wide lamp is 80 % as tall
    const low = lw && r.top >= stripsBottom() - 1 ? "1" : "";
    if ((b.dataset.low || "") !== low) b.dataset.low = low;
    if (ADAPT) fitKey(b, r);
  }
  // where the waveforms end (the lowest strip's bottom edge), once per layout
  let _sb = null;
  const stripsBottom = () => _sb ?? (_sb = Math.max(0, ...[...app.querySelectorAll(".strip")].map(e => e.getBoundingClientRect().bottom)));
  const norm = t => (t || "").replace(/\d/g, "8").trim();
  function fitKey(b, r) {
    for (const x of b.querySelectorAll(".two > b, .two > small")) x.style.removeProperty("font-size");   // Element's own squeeze
    // the same shape and words (SHIFT only swaps which of the two is big): the same size as last time
    const sig = `${Math.round(r.width)}x${Math.round(r.height)}|${b.dataset.ls}|${norm(b.querySelector(".two > b")?.textContent)}|${norm(b.querySelector(".two > small")?.textContent)}|${b.querySelector(".two") ? "" : norm(b.textContent)}`;
    if (b._sig === sig && b._afs) { b.style.setProperty("--afs", (b._row || b._afs).toFixed(1) + "px"); return; }
    b._sig = sig; b._row = 0;
    const cs = getComputedStyle(b);
    const W = b.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight) - 2;
    const H = b.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom) - 1;
    const text = (b.textContent || "").trim(); if (!text || W < 6 || H < 6) return;
    const lines = b.querySelector("small") ? 1.6 : 1, longest = Math.max(...[...b.querySelectorAll("b, small")].map(x => x.textContent.length), (b.firstChild?.nodeType === 3 ? b.firstChild.textContent.trim().length : 0), 1);
    let s = Math.sqrt(fill(r.width, r.height) * r.width * r.height / (longest * .58 * lines));
    s = Math.max(8, Math.min(s, H / lines));
    b.dataset.afs = "1";
    for (let i = 0; i < 14; i++) {
      b.style.setProperty("--afs", s.toFixed(1) + "px");
      const t = textBox(b); if ((t.width <= W && t.height <= H) || s <= 8) break;
      s *= .92;
    }
    s = Math.max(8, s * BIG); b.style.setProperty("--afs", s.toFixed(1) + "px");
    b._afs = s;
  }
  // keys side by side in one row share one size: the largest that fits all of them
  function evenRows(keys) {
    const rows = new Set(keys.map(k => k.parentElement).filter(Boolean));
    for (const row of rows) {
      const sib = [...row.children].filter(k => k.tagName === "BUTTON" && k._afs);
      if (sib.length < 2) continue;
      const m = Math.min(...sib.map(k => k._afs));
      for (const k of sib) { k._row = m; k.style.setProperty("--afs", m.toFixed(1) + "px"); }
    }
  }

  function pad(p) {
    if (!on || p.classList.contains("asknob")) return;
    // the leading bare text (the pad's number, or its word) gets a span, so it can be placed
    const first = p.firstChild;
    if (first && first.nodeType === 3 && first.textContent.trim()) { const s = document.createElement("span"); s.className = "pn"; first.replaceWith(s); s.append(first); }
    const lbl = p.querySelector(".lbl"), pn = p.querySelector(".pn"), tm = p.querySelector(".tm");
    const numText = pn?.textContent.trim() || "";
    p.classList.toggle("hasname", !!lbl);
    p.classList.toggle("numonly", !lbl && /^\d+$/.test(numText));   // a cue without a name: just its number
    const r = p.getBoundingClientRect(); if (!r.width) return;
    const sig = `${Math.round(r.width)}x${Math.round(r.height)}|${norm(lbl?.textContent)}|${norm(numText)}|${norm(tm?.textContent)}|${norm(p.querySelector("small")?.textContent)}`;
    if (p._sig === sig) return;
    p._sig = sig;
    const W = r.width - 14, H = r.height - 12;
    // the number: one size for every pad of this shape: 2.5 times the old small number (half a name's size)
    const nominal = Math.min(Math.sqrt(fill(r.width, r.height) * r.width * r.height / (5 * .56 * 1.6)), .93 * W / (5 * .56), H / 1.7);
    let small = Math.max(9, nominal * .5), num = 2.5 * small;
    const timeW = tm ? tm.textContent.length * .6 * small + 6 : 0;
    num = Math.min(num, (W - timeW) / (Math.max(1, numText.length) * .62), H * .55);
    // the name: what's left under the number; two lines only for a name of more than one word, on a tall pad.
    // Chinese and Japanese characters are a full em wide, Latin letters about half
    const name = (lbl || pn)?.textContent || "";
    const lines = lbl && r.height / r.width > 1.05 && /\s/.test(name.trim()) ? 2 : 1;
    const ems = [...name].reduce((n, ch) => n + (/[\u2E80-\u9FFF\uAC00-\uD7AF\uF900-\uFAFF\uFF00-\uFFEF]/.test(ch) ? 1 : .56), 0);
    const per = Math.max(.56, ems / lines);
    const room = lbl ? H - num * 1.02 - 3 : H - small * 1.5 * 1.3;   // other modes: their second line half as big again
    let s = Math.sqrt(fill(r.width, r.height) * r.width * r.height / (per * (lines + .6)));
    s = Math.max(10, Math.min(s, .93 * W / per, room / (lines * 1.08 + (lbl ? .3 : 0))));   // a cue's name starts 0.3 of a line lower
    p.style.setProperty("--pfs", Math.max(10, s * BIG).toFixed(1) + "px");
    p.style.setProperty("--pfs2", small.toFixed(1) + "px");
    p.style.setProperty("--pfn", Math.max(10, num * BIG).toFixed(1) + "px");
    p.style.setProperty("--pfs3", (small * 1.5).toFixed(1) + "px");
  }

  // ---- the nudge bars: a CDJ jog wheel's edge, seen from the side and turned by Element's --off (the finger's
  // travel). Drawn in device pixels; teeth that would come out thinner than 3 device pixels near the curved
  // edges become plain shade, so nothing finer than the screen's pixels moves (no moire, no shimmer). ----
  const snap = Math.round;
  function jog(n) {
    if (!on) return;
    let c = n.querySelector(":scope > canvas.sk-jog");
    if (!c) { c = document.createElement("canvas"); c.className = "sk-jog"; n.prepend(c); }
    const dpr = devicePixelRatio || 1, W = snap(n.clientWidth * dpr), H = snap(n.clientHeight * dpr);
    if (!W || !H) return;
    if (c.width !== W || c.height !== H) { c.width = W; c.height = H; }
    const g = c.getContext("2d"); g.clearRect(0, 0, W, H);
    const off = parseFloat(n.style.getPropertyValue("--off")) || 0, held = n.classList.contains("held");
    const acc = getComputedStyle(n).getPropertyValue("--acc").trim() || "#ff8a1e";
    const R = W / 2, teeth = 44, step = Math.PI * 2 / teeth, angle = off * dpr / R;
    const top = Math.max(2, snap(3 * dpr)), bot = H - snap(H * .14);   // a 3 px lit lip along the top
    g.fillStyle = "#141416"; g.fillRect(0, top, W, bot - top);
    for (let i = 0; i < teeth; i++) {
      const a0 = (((i * step + angle) % (Math.PI * 2)) + Math.PI * 2) % (Math.PI * 2) - Math.PI, a1 = a0 + step;
      if (a1 < -Math.PI / 2 || a0 > Math.PI / 2) continue;
      const x0 = R + R * Math.sin(Math.max(-Math.PI / 2, a0)), x1 = R + R * Math.sin(Math.min(Math.PI / 2, a1)), w = x1 - x0;
      const light = .3 + .7 * Math.max(0, Math.cos((a0 + a1) / 2 + .4));
      if (w < 3) continue;
      // a round dot: a circle where it faces you, narrower only as it turns away (the projection)
      const c2 = 10 + 36 * light | 0, rd = Math.min((bot - top) * .3, R * step * .3), ry = rd, rx = Math.max(1.5, rd * w / (R * step));
      g.fillStyle = `rgb(${c2},${c2},${c2 + 2})`; g.beginPath();
      g.ellipse(x0 + w / 2, (top + bot) / 2, rx, ry, 0, 0, Math.PI * 2); g.fill();
    }
    // the lip: light at its top edge, darker the lower it goes (light falling off round the rim)
    const lip = g.createLinearGradient(0, 0, 0, top); lip.addColorStop(0, "#e2e3e6"); lip.addColorStop(1, "#4a4c50");
    g.fillStyle = lip; g.fillRect(0, 0, W, top);
    g.globalAlpha = held ? 1 : .4; g.fillStyle = acc; g.fillRect(0, bot, W, H - bot); g.globalAlpha = 1;
    const edge = g.createLinearGradient(0, 0, W, 0);
    edge.addColorStop(0, "rgba(0,0,0,.85)"); edge.addColorStop(.16, "rgba(0,0,0,.08)"); edge.addColorStop(.84, "rgba(0,0,0,.15)"); edge.addColorStop(1, "rgba(0,0,0,.88)");
    g.fillStyle = edge; g.fillRect(0, 0, W, H);
  }
  const jogs = () => app.querySelectorAll(".nudge").forEach(jog);
  new MutationObserver(ms => { for (const m of ms) if (m.target.classList?.contains("nudge")) jog(m.target); })
    .observe(app, { subtree: true, attributes: true, attributeFilter: ["style", "class"] });
  setTimeout(() => body.classList.add("learned"), 20000);

  // ---- knobs: the flat top (a circle over Element's cap) and the gradients both use, once for the page ----
  if (!document.getElementById("sk-defs")) {
    const d = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    d.id = "sk-defs"; d.setAttribute("width", "0"); d.setAttribute("height", "0"); d.style.position = "absolute";
    d.innerHTML = `<defs><linearGradient id="sk-knob-skirt" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3c3c3e"/><stop offset=".5" stop-color="#1a1a1b"/><stop offset="1" stop-color="#0d0d0e"/></linearGradient>` +
      `<linearGradient id="sk-knob-face" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#252527"/><stop offset="1" stop-color="#1b1b1d"/></linearGradient></defs>`;
    body.appendChild(d);
  }
  function knobs() {
    for (const svg of app.querySelectorAll(".knob svg")) {
      if (svg.querySelector(".sk-face")) continue;
      const bg = svg.querySelector(".kbg"); if (!bg) continue;
      const f = document.createElementNS("http://www.w3.org/2000/svg", "circle");
      f.setAttribute("class", "sk-face"); f.setAttribute("cx", "20"); f.setAttribute("cy", "20"); f.setAttribute("r", "9.6");
      bg.after(f);   // over the cap, under the arc and the pointer
    }
  }

  // ---- the tempo lanes: the ribs run under the finger (only the look: Element does the tempo) ----
  const laneY = new WeakMap();
  app.addEventListener("pointerdown", e => { const l = e.target.closest?.(".lane"); if (l) laneY.set(l, { y: e.clientY, off: parseFloat(l.style.getPropertyValue("--sk-ty")) || 0 }); }, true);
  addEventListener("pointermove", e => {
    for (const l of app.querySelectorAll(".lane.held")) { const st = laneY.get(l); if (!st) continue; st.off += e.clientY - st.y; st.y = e.clientY; l.style.setProperty("--sk-ty", st.off.toFixed(1) + "px"); }
  }, true);

  // Element repaints keys and pads as things change: redo the ones that changed, once a frame
  var queued = new Set(), all = true;
  const run = () => {
    PHONE = app.classList.contains("phone"); body.classList.toggle("sk-phone", PHONE);
    if (all) _sb = null;
    const whole = all, els = all ? [...app.querySelectorAll("button"), ...app.querySelectorAll(".pad")] : [...queued];
    all = false; queued = new Set();
    if (whole && on) { jogs(); knobs(); }
    for (const el of els) { if (!el.isConnected) continue; if (el.classList.contains("pad")) pad(el); else key(el); }
    if (ADAPT) evenRows(els.filter(el => el.tagName === "BUTTON"));
  };
  var raf = 0;
  function later() { if (!raf) raf = requestAnimationFrame(() => { raf = 0; run(); }); }
  new MutationObserver(ms => {
    for (const m of ms) {
      const el = (m.target.nodeType === 1 ? m.target : m.target.parentElement)?.closest?.("button, .pad");
      if (el && app.contains(el)) queued.add(el);
      if (m.type === "childList" && [...m.addedNodes].some(n => n.nodeType === 1 && (n.matches("button, .pad, .knob") || n.querySelector?.("button, .pad, .knob")))) all = true;
    }
    if (on) run();   // a microtask: before this frame is drawn
  }).observe(app, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ["class"] });
  addEventListener("resize", () => { all = true; later(); });
  document.fonts?.ready.then(() => { all = true; later(); });
  later();
  requestAnimationFrame(jogs); addEventListener("resize", () => requestAnimationFrame(jogs));
  new MutationObserver(() => requestAnimationFrame(jogs)).observe(app, { childList: true });
})();
