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

// A track's waveform being analysed: untilWf(id, ms) waits for the hub's "wf" for it (wfReady), or ms at most
const wfWaiting = new Map();   // track id -> what's waiting for its "wf"
const wfReady = id => { (wfWaiting.get(id) || []).forEach(f => f()); wfWaiting.delete(id); };
const untilWf = (id, ms) => new Promise(res => {
  const t = setTimeout(res, ms);
  wfWaiting.set(id, [...(wfWaiting.get(id) || []), () => { clearTimeout(t); res(); }]);
});
