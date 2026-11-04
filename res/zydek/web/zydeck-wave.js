// ZyDeck: drawing a track's waveform: one copy for every page that draws one (the deck page, the phone page's
// decks), so they can't disagree. Beside zydeck-position.js, which says where.

// The beat grid in the strip from pixel left to left + W: every 4th beat (from the bar's first, d.bar) a full line,
// the rest ticks at the top and bottom. A line just past either edge is drawn too (its width reaches in).
function drawBeats(g, d, pxPerSec, left, W, H) {
  const b = d.beats;
  if (!b || !b.length) return;
  const dpr = devicePixelRatio || 1, start = (left - 2 * dpr) / pxPerSec, end = (left + W + 2 * dpr) / pxPerSec;
  let lo = 0, hi = b.length;
  while (lo < hi) { const m = (lo + hi) >> 1; if (b[m] < start) lo = m + 1; else hi = m; }
  const tick = Math.round(H * .2);
  for (let i = lo; i < b.length && b[i] <= end; i++) {
    const x = Math.round(b[i] * pxPerSec) - left;
    if ((i + d.bar) % 4 === 0) { g.fillStyle = "rgba(255,255,255,.55)"; g.fillRect(x - dpr, 0, 2 * dpr, H); }
    else { g.fillStyle = "rgba(255,255,255,.5)"; g.fillRect(x, 0, dpr, tick); g.fillRect(x, H - tick, dpr, tick); }
  }
}

// A track's sections (the hub's /api/structure, ZyDeck Connect's /web/structure: d.sections, [{s, e (seconds), letter,
// label}]), in their colours: a tint behind the waveform, a strip along its bottom and a tag where each starts
const SECTION_COLORS = { Intro: "#3fbf6f", Verse: "#4a90e2", "Pre-Chorus": "#e0b020", Chorus: "#e5484d",
  Interlude: "#9b6bd6", Bridge: "#2fb7b0", Outro: "#3fbf6f" };
// A section's letter where it starts (zoomed in: with its part, "B Pre-Chorus"); a run of the same one: once
const sectionTag = (secs, i) => i === 0 || secs[i].letter !== secs[i - 1].letter || secs[i].label !== secs[i - 1].label;
function drawSectionTint(g, d, pxPerSec, left, W, H, alpha = .2) {
  if (!d.sections) return;
  g.globalAlpha = alpha;
  for (const s of d.sections) {
    const x0 = Math.round(s.s * pxPerSec) - left, x1 = Math.round(s.e * pxPerSec) - left;
    if (x1 < 0 || x0 > W) continue;
    g.fillStyle = SECTION_COLORS[s.label] || "#888";
    g.fillRect(x0, 0, x1 - x0, H);
  }
  g.globalAlpha = 1;
}
// names: "B Pre-Chorus" (else just "B", where it fits); tags: false draws only the strip (the deck page's waveform,
// whose start lines and tags are its own, unstretched by the tempo)
function drawSectionTags(g, d, pxPerSec, left, W, H, names, tags = true) {
  const secs = d.sections;
  if (!secs) return;
  const dpr = devicePixelRatio || 1, bar = Math.max(2, Math.round(2 * dpr)), fs = Math.max(Math.round(9 * dpr), Math.round(H * .3));
  g.font = `bold ${fs}px sans-serif`; g.textBaseline = "top"; g.textAlign = "left";
  let first = true;
  secs.forEach((s, i) => {
    const x0 = Math.round(s.s * pxPerSec) - left, x1 = Math.round(s.e * pxPerSec) - left;
    if (x1 < 0 || x0 > W) return;
    const col = SECTION_COLORS[s.label] || "#888";
    g.fillStyle = col;
    g.fillRect(x0, H - bar, x1 - x0, bar);   // a strip along the bottom, in the section's colour
    // (zoomed in: the section under the left edge keeps its tag there, so you know where you are)
    const sticky = names && first && x0 < 0;
    first = false;
    if (!tags) return;
    const starts = sectionTag(secs, i);
    if (starts && x0 >= 0) {
      g.fillStyle = "rgba(0,0,0,.65)";
      g.fillRect(x0, 0, Math.max(1, Math.round(dpr)), H);   // where it starts
    }
    if (!starts && !sticky) return;
    const text = names ? `${s.letter} ${s.label}` : s.letter, w = g.measureText(text).width + fs * .5, x = Math.max(x0, 0);
    if (!names && x1 - x0 < w) return;
    g.fillStyle = col;
    g.fillRect(x + dpr, H - bar - fs * 1.15, w, fs * 1.15);
    g.fillStyle = "#101010";
    g.fillText(text, x + dpr + fs * .25, H - bar - fs * 1.1);
  });
}
// The section at t (seconds): its index in secs, or -1
function sectionAt(secs, t) {
  if (!secs) return -1;
  let lo = 0, hi = secs.length - 1;
  while (lo <= hi) { const m = (lo + hi) >> 1; if (t < secs[m].s) hi = m - 1; else if (t >= secs[m].e) lo = m + 1; else return m; }
  return -1;
}
// Bars from t to e (seconds), with their fraction (4 beats a bar): by the beat grid (d.beats), or the BPM without one
function barsBetween(d, t, e) {
  const b = d.beats;
  const beatAt = x => {   // beats from the grid's first, fractional (before the first and after the last: at their spacing)
    if (!b || b.length < 2) return d.bpm > 0 ? x * d.bpm / 60 : NaN;
    let lo = 0, hi = b.length - 1;
    if (x <= b[0]) return (x - b[0]) / (b[1] - b[0]);
    if (x >= b[hi]) return hi + (x - b[hi]) / (b[hi] - b[hi - 1]);
    while (hi - lo > 1) { const m = (lo + hi) >> 1; if (b[m] <= x) lo = m; else hi = m; }
    return lo + (x - b[lo]) / (b[hi] - b[lo]);
  };
  const n = (beatAt(e) - beatAt(t)) / 4;
  return n > 0 ? n : 0;
}

// A track's waveform being analysed: untilWf(id, ms) waits for the hub's "wf" for it (wfReady), or ms at most
const wfWaiting = new Map();   // track id -> what's waiting for its "wf"
const wfReady = id => { (wfWaiting.get(id) || []).forEach(f => f()); wfWaiting.delete(id); };
const untilWf = (id, ms) => new Promise(res => {
  const t = setTimeout(res, ms);
  wfWaiting.set(id, [...(wfWaiting.get(id) || []), () => { clearTimeout(t); res(); }]);
});
