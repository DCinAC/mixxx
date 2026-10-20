// ZyDeck: where a playing track is, and where to draw it: one copy for every page that draws a waveform against
// the sound (the deck page, the phone page's decks, the calibration page), so they can't disagree.
//
// Play position between Mixxx's updates (about 30 a second, only when it changed). Each update carries the hub's
// clock (ts, ms) from the moment Mixxx reported the position, so network delay doesn't jitter the timing: hub time
// maps onto this page's clock through the fastest delivery seen in the last 5 s. Between updates the position runs
// on at the speed the updates show, at most 250 ms past the last one (no update means it stopped). What's drawn is
// lat (the hub's latency: buffer, output, offset) earlier, where the sound you hear is, and never steps back while
// the music runs forward: if the drawing got ahead it waits; if it's behind it catches up over a few frames; a
// real jump (seek, scratch) goes straight there.
//
// d: the track's state, kept on it: pp/pt/pv (a position, when, at what speed), shown (what was drawn), hist.
// d.playing and d.ratio (the tempo) are the page's.
(function () {
  const clockSync = { samples: [], off: null };
  function hubTime(ts, now) {   // ts on the hub's clock -> this page's clock
    if (ts == null) return now;
    clockSync.samples.push([now, now - ts]);
    while (clockSync.samples.length > 1 && now - clockSync.samples[0][0] > 5000) clockSync.samples.shift();
    clockSync.off = clockSync.samples.reduce((m, x) => Math.min(m, x[1]), Infinity);
    return ts + clockSync.off;
  }
  function posSeed(d, p, v, now) { Object.assign(d, { pp: p, pt: now, pv: v, shown: undefined }); }
  // where the track is at now (lat: seconds earlier, the sound's delay)
  function posModel(d, now, lat = 0) {
    return d.pt === undefined ? d.pos || 0 : d.pp + d.pv * Math.max(-1.2, Math.min((now - d.pt) / 1000 - lat, .25));
  }
  function posAt(d, now, lat = 0) {   // what to draw now
    const target = posModel(d, now, lat);
    let shown = d.shown;
    const dir = d.pv > .05 ? 1 : d.pv < -.05 ? -1 : 0;
    if (shown === undefined || Math.abs(target - shown) > .25 || !dir) shown = target;
    else if ((target - shown) * dir < 0) { /* ahead of the music: hold until it catches up */ }
    else shown += (target - shown) * .5;
    d.shown = shown;
    return shown;
  }
  // Mixxx moves the position once per audio buffer (~21 ms) and it's read every 33 ms, so single steps say
  // 0.6x or 1.3x in turn. The speed is the deck's tempo unless the last 300 ms clearly say otherwise
  // (scratching, nudging), and each reading moves the drawing only 15 % of the way to it.
  function posUpdate(d, p, now, ts) {   // p in seconds
    const t = Math.min(now, hubTime(ts, now));
    const nominal = d.playing ? d.ratio || 1 : 0;
    if (d.pt === undefined || Math.abs(p - posModel(d, t)) > .3) {   // a jump: follow it
      d.hist = [[t, p]];
      return posSeed(d, p, nominal, t);
    }
    const hist = d.hist || (d.hist = []);
    hist.push([t, p]);
    while (hist.length > 2 && t - hist[0][0] > 300) hist.shift();
    let v = nominal;
    if (hist.length >= 4) {
      const mt = hist.reduce((a, x) => a + x[0], 0) / hist.length, mp = hist.reduce((a, x) => a + x[1], 0) / hist.length;
      let num = 0, den = 0;
      for (const [ht, hp] of hist) { num += (ht - mt) * (hp - mp); den += (ht - mt) ** 2; }
      const slope = den ? num / den * 1000 : nominal;
      if (Math.abs(slope - nominal) > .08) v = slope;
    }
    const pred = posModel(d, t);
    d.pp = pred + (p - pred) * .15; d.pt = t; d.pv = v;
  }
  // stopped (play off): frozen where Mixxx is
  const posStop = (d, now) => { if (d.pt !== undefined) posSeed(d, posModel(d, now), 0, now); };

  // A waveform's columns, as Mixxx's RGB waveform (allshader/waveformrendererrgb.cpp): per column the loudest
  // low / mid / high mix red / green / blue, scaled so the strongest is full brightness, the height its loudness.
  // wf: {rate, n, data (all, low, mid, high per point; stride bytes a point)}; columns x0.. of width W at pxPerSec.
  function waveColumns(g, wf, x0, W, H, pxPerSec) {
    const a = wf.data, st = wf.stride || 4, perPx = wf.rate / pxPerSec, mid = H / 2;
    for (let x = 0; x < W; x++) {
      const i0 = Math.floor((x0 + x) * perPx), i1 = Math.max(i0 + 1, Math.floor((x0 + x + 1) * perPx));
      if (i1 <= 0 || i0 >= wf.n) continue;
      let all = 0, lo = 0, mi = 0, hi = 0;
      for (let i = Math.max(0, i0); i < Math.min(wf.n, i1); i++) {
        const j = i * st;
        if (a[j] > all) all = a[j]; if (a[j + 1] > lo) lo = a[j + 1]; if (a[j + 2] > mi) mi = a[j + 2]; if (a[j + 3] > hi) hi = a[j + 3];
      }
      const m = Math.max(lo, mi, hi);
      if (m <= 0) continue;
      g.fillStyle = `rgb(${Math.round(255 * lo / m)},${Math.round(255 * mi / m)},${Math.round(255 * hi / m)})`;
      const h = Math.min(mid, Math.max(1, all / 255 * mid * .95));
      g.fillRect(x, mid - h, 1, h * 2);
    }
  }

  window.ZydeckPos = { hubTime, posSeed, posModel, posAt, posUpdate, posStop, waveColumns, clockSync };
})();
