// ZyDeck Big Wave: one renderer for the phone's Big Wave and the T4·4 tablet display.
// The host supplies the deck object and an environment: { remote, LAT, endWarn, seconds, spanUntil }.
// zydeck-wave.js supplies drawBeats/drawSectionTint/drawSectionTags/barsBetween/sectionAt/fmtBarsLeft/SECTION_COLORS;
// zydeck-position.js supplies ZydeckPos.posAt/slipModel.
(() => {
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const h = (tag, cls, html) => { const el = document.createElement(tag); if (cls) el.className = cls; if (html != null) el.innerHTML = html; return el; };
  const hexColor = v => `#${v.toString(16).padStart(6, "0")}`;
  const inkOn = v => { const n = parseInt(v.slice(1), 16), r = n >> 16 & 255, g = n >> 8 & 255, b = n & 255; return (r * .299 + g * .587 + b * .114) / 255 > .55 ? "#101010" : "#fff"; };

  function sizeCanvas(c) {
    const dpr = devicePixelRatio || 1, W = Math.round(c.clientWidth * dpr), H = Math.round(c.clientHeight * dpr);
    if (c.width !== W || c.height !== H) { c.width = W; c.height = H; return true; }
    return false;
  }
  function colorFor(a, i) {   // like Mixxx's RGB waveform: red lows, green mids, blue highs
    const lo = a[i + 1], mi = a[i + 2], hi = a[i + 3], m = Math.max(lo, mi, hi, 1);
    return `rgb(${Math.round(255 * lo / m)},${Math.round(230 * mi / m)},${Math.round(255 * hi / m)})`;
  }
  const WAVE_FULL = .8;

  function cueMarks(d) {
    const sec = v => v >= 0 && d.trackSamples > 0 && d.dur > 0 ? v / d.trackSamples * d.dur : null;
    const marks = [], main = sec(d.mainCue);
    if (main != null) marks.push({ t: main, color: "#ff0000", label: "CUE", top: true });
    for (let k = 0; k < 8; k++) {
      const t = sec(d.cuePos?.[k]);
      if (t != null) marks.push({ t, color: d.cueColors?.[k] != null ? hexColor(d.cueColors[k]) : "#00d9ff", label: String(k + 1) });
    }
    return marks;
  }
  const cueSig = d => cueMarks(d).map(m => m.t.toFixed(3) + m.color).join();

  function drawCueMarks(g, marks, pxPerSec, left, W, H) {
    const dpr = devicePixelRatio || 1, tag = Math.max(Math.round(11 * dpr), Math.round(H * .17));
    g.font = `bold ${Math.round(tag * .74)}px sans-serif`; g.textBaseline = "middle"; g.textAlign = "left";
    for (const m of marks) {
      const x = Math.round(m.t * pxPerSec) - left;
      if (x < -tag * 4 || x > W + dpr) continue;
      g.fillStyle = m.color;
      g.fillRect(x - dpr, 0, 2 * dpr, H);
      const w = Math.ceil(g.measureText(m.label).width + tag * .6), y = 0;
      g.fillRect(x, y, w, tag);
      g.fillStyle = inkOn(m.color);
      g.fillText(m.label, x + tag * .3, y + tag / 2);
    }
  }

  const TILE = 1024, ZOOM_SECONDS = 4;
  function drawZoom(p, d, env) {
    const c = p.zCanvas;
    sizeCanvas(c);
    const seconds = p.seconds || ZOOM_SECONDS;
    const W = c.width, H = c.height, wf = d.wf, pxPerSec = W / seconds;
    const left = Math.round((d.pos - seconds / 2) * pxPerSec), now = performance.now();
    const slipping = d.slipAt !== undefined && (env.remote?.on || now - d.slipAt < 1000);
    const slipLeft = slipping ? Math.round((ZydeckPos.slipModel(d, now) - (env.LAT || 0) * (d.playing ? d.ratio || 1 : 0) - seconds / 2) * pxPerSec) : null;
    const blink = slipping ? Math.round((Math.cos(now / 800 * Math.PI) + 1) * 4) : 0;
    const key = `${d.n}|${d.wfVer}|${d.beatsVer || 0}|${d.strVer || 0}|${cueSig(d)}|${left}|${W}|${H}|${seconds}|${slipLeft}|${blink}`;
    if (p.zKey === key) return;
    p.zKey = key;
    const g = c.getContext("2d");
    g.clearRect(0, 0, W, H);
    if (!wf) { drawBeats(g, d, pxPerSec, left, W, H); drawCueMarks(g, cueMarks(d), pxPerSec, left, W, H); return; }
    drawSectionTint(g, d, pxPerSec, left, W, H);
    drawBeats(g, d, pxPerSec, left, W, H);
    drawZoomWave(p, d, g, left, W, H, pxPerSec);
    drawCueMarks(g, cueMarks(d), pxPerSec, left, W, H);
    drawSectionTags(g, d, pxPerSec, left, W, H, true, true, p.tagOpts);
    if (slipLeft == null) return;
    const half = Math.ceil(H / 2), edge = Math.max(2, Math.round(devicePixelRatio));
    g.save();
    g.beginPath(); g.rect(0, 0, W, half); g.clip();
    g.fillStyle = "#000"; g.fillRect(0, 0, W, half);
    drawBeats(g, d, pxPerSec, slipLeft, W, H);
    drawZoomWave(p, d, g, slipLeft, W, H, pxPerSec);
    drawCueMarks(g, cueMarks(d), pxPerSec, slipLeft, W, H);
    g.restore();
    g.fillStyle = `rgba(224,224,224,${(.25 + blink / 8 * .5).toFixed(3)})`;
    g.fillRect(0, 0, W, edge); g.fillRect(0, half, W, edge); g.fillRect(0, 0, edge, half); g.fillRect(W - edge, 0, edge, half);
  }
  function drawZoomWave(p, d, g, left, W, H, pxPerSec) {
    const wf = d.wf, tkey = `${d.n}|${d.wfVer}|${W}|${H}|${pxPerSec}`;
    if (p.tileKey !== tkey) { p.tileKey = tkey; p.tiles = new Map(); }
    for (let t = Math.floor(left / TILE); t * TILE < left + W; t++) {
      if (t < 0 || t * TILE / pxPerSec > wf.n / wf.rate) continue;
      let tile = p.tiles.get(t);
      if (!tile) {
        tile = document.createElement("canvas"); tile.width = TILE; tile.height = H;
        const tg = tile.getContext("2d"), a = wf.data, perPx = wf.rate / pxPerSec;
        for (let x = 0; x < TILE; x++) {
          const i0 = Math.floor((t * TILE + x) * perPx), i1 = Math.max(i0 + 1, Math.floor((t * TILE + x + 1) * perPx));
          if (i0 >= wf.n || i1 <= 0) continue;
          let all = 0, best = i0;
          for (let i = Math.max(0, i0); i < Math.min(wf.n, i1); i++) if (a[i * 4] > all) { all = a[i * 4]; best = i; }
          tg.fillStyle = colorFor(a, best * 4);
          const hh = Math.max(1, all / 255 * H * WAVE_FULL / 2);
          tg.fillRect(x, H / 2 - hh, 1, hh * 2);
        }
        p.tiles.set(t, tile);
        if (p.tiles.size > 12) p.tiles.delete(p.tiles.keys().next().value);
      }
      g.drawImage(tile, t * TILE - left, 0);
    }
  }

  function createPanel(k, opts = {}) {
    const root = h("div", "wv", `<canvas></canvas><div class="played"></div><div class="cur"></div><div class="endwarn"></div>` +
      `<div class="name"><b></b><span></span></div><div class="bars"></div><div class="cursec"></div><div class="span"></div>` +
      (opts.timer === false ? "" : `<div class="tm7"><span class="s7"></span><span class="silk">:</span><span class="s7"></span></div>`));
    return { k, root, zCanvas: root.querySelector("canvas"), num: root.querySelector("b"), title: root.querySelector(".name span"),
      bars: root.querySelector(".bars"), cursec: root.querySelector(".cursec"), span: root.querySelector(".span"), time7: root.querySelectorAll(".s7"),
      tiles: new Map(), tagOpts: opts.tagOpts || { maxFs: 13 * (devicePixelRatio || 1), sticky: false } };
  }

  function drawFrame(p, d, now, env = {}) {
    const loaded = d.loaded !== false;
    if (!env.keepPos) d.pos = loaded ? clamp(ZydeckPos.posAt(d, now, env.LAT || 0), 0, d.dur) : 0;
    const ending = env.endWarn !== false && loaded && d.playing && d.dur > 0 && d.pos < d.dur && (d.dur - d.pos) / (d.ratio || 1) < 30;
    p.root.className = `wv${d.n > 2 ? " d34" : ""}${loaded ? "" : " empty"}${d.playing ? " playing" : ""}${ending ? " ending" : ""}`;
    p.num.textContent = d.n;
    const title = loaded ? (d.title || "Loaded") + (d.artist ? " · " + d.artist : "") : "Empty";
    if (p.title.textContent !== title) p.title.textContent = title;
    const left = loaded ? Math.max(0, Math.round((d.dur - d.pos) / (d.ratio || 1))) : null;
    if (p.time7?.length) {
      seg7(p.time7[0], left != null ? "-" + Math.floor(left / 60) : "", 2);
      seg7(p.time7[1], left != null ? String(left % 60).padStart(2, "0") : "", 2);
    }
    const bars = loaded && d.dur > 0 ? barsBetween(d, d.pos, d.dur) : NaN, text = bars >= 0 ? (env.barsShort ? `-${Math.floor(bars)}` : fmtBarsLeft(bars)) : "";
    if (p.bars.textContent !== text) p.bars.textContent = text;
    const sc = loaded ? d.sections?.[sectionAt(d.sections, d.pos)] : null, inSec = sc ? barsBetween(d, d.pos, sc.e) : NaN;
    const cursec = sc ? `<i>${esc(sc.letter + " " + sc.label)}</i>${inSec >= 0 ? (env.barsShort ? Math.floor(inSec) : fmtBarsLeft(inSec)) + " bars" : ""}` : "";
    if (p.cursecText !== cursec) {
      p.cursecText = cursec;
      p.cursec.innerHTML = cursec;
      p.cursec.style.setProperty("--c", sc ? SECTION_COLORS[sc.label] || "#888" : "");
    }
    p.seconds = env.seconds || ZOOM_SECONDS;
    if (loaded) drawZoom(p, d, env);
    const spanOn = now < (env.spanUntil || 0);
    p.root.classList.toggle("spanon", spanOn);
    if (spanOn) {
      const seen = loaded ? barsBetween(d, d.pos - p.seconds / 2, d.pos + p.seconds / 2) : NaN;
      const span = seen > 0 ? `${+seen.toFixed(1)} bars` : `${p.seconds} s`;
      if (p.span.textContent !== span) p.span.textContent = span;
    }
  }

  window.BigWave = { createPanel, drawFrame, drawZoom, sizeCanvas, colorFor, cueMarks, cueSig };
})();
