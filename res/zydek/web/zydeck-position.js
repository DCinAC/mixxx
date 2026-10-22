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
// Better than all that: the hub's own stream of where each playing deck is heard ("vp", ~60 a second, from Mixxx's
// audio timing, with its exact speed): drawn straight from it (posVisual), no regression and no smoothing. The
// model above is the fallback (a deck that's stopped, or ZyDeck Connect's decks in controller mode).
// Two more things the drawing counts: the hub's clock is matched by asking it (half the quickest round trip, as NTP
// does), and the screen shows a frame about two frames after it's drawn, so each frame is drawn for that moment.
//
// d: the track's state, kept on it: pp/pt/pv (a position, when, at what speed), shown (what was drawn), hist; vp
// (the hub's latest heard position: p, s, t). d.playing and d.ratio (the tempo) are the page's.
(function () {
  // ---- the hub's clock ----
  // pings: [when, round trip, this page's clock - the hub's]; the quickest of the last 30 s is the one used. Until the
  // first answer, the fastest delivery of a stamped message (late by that one-way trip).
  const clockSync = { samples: [], off: null, pings: [], best: null, timer: 0, burst: 0 };
  function clockPong(c, h, now) {   // the hub's answer to {clk: c}: its time h when it answered
    const rtt = now - c;
    if (!(rtt >= 0 && rtt < 2000)) return;
    clockSync.pings.push([now, rtt, (c + now) / 2 - h]);
    while (clockSync.pings.length > 1 && now - clockSync.pings[0][0] > 30000) clockSync.pings.shift();
    clockSync.best = clockSync.pings.reduce((b, x) => (x[1] < b[1] ? x : b));
  }
  function clockStart(send) {   // with the page's socket: a few quickly, then every 2 s
    clearInterval(clockSync.timer); clearTimeout(clockSync.burst);
    let n = 0;
    const ask = () => { try { send({ clk: performance.now() }); } catch {} };
    const burst = () => { ask(); if (++n < 5) clockSync.burst = setTimeout(burst, 120); };
    burst();
    clockSync.timer = setInterval(ask, 2000);
  }
  function hubTime(ts, now) {   // ts on the hub's clock -> this page's clock
    if (ts == null) return now;
    clockSync.samples.push([now, now - ts]);
    while (clockSync.samples.length > 1 && now - clockSync.samples[0][0] > 5000) clockSync.samples.shift();
    clockSync.off = clockSync.samples.reduce((m, x) => Math.min(m, x[1]), Infinity);
    return ts + (clockSync.best ? clockSync.best[2] : clockSync.off);
  }

  // ---- the hub's latency: all of it (lat, for the fallback model) and the part its heard positions leave out ----
  const latency = { full: 0, extra: 0 };
  function setLatency(full, extra) { latency.full = full || 0; latency.extra = extra ?? latency.full; }

  // ---- the screen: a frame shows about two frames after it's drawn (compositor, then the display) ----
  const frames = { last: 0, gaps: [] };
  function frameAt(now) {
    if (now === frames.last) return;
    const gap = now - frames.last;
    if (frames.last && gap > 4 && gap < 100) { frames.gaps.push(gap); if (frames.gaps.length > 90) frames.gaps.shift(); }
    frames.last = now;
  }
  function ahead() {   // seconds
    const g = frames.gaps.length ? [...frames.gaps].sort((a, b) => a - b)[frames.gaps.length >> 1] : 1000 / 60;
    return 2 * Math.min(g, 34) / 1000;
  }

  // ---- the hub's heard positions ----
  function posVisual(d, p, s, ts, now) { d.vp = { p, s, t: hubTime(ts, now) }; }   // seconds, track seconds per second
  const visual = (d, now) => !!d.vp && now - d.vp.t < 300;
  function posSeed(d, p, v, now) { Object.assign(d, { pp: p, pt: now, pv: v, shown: undefined }); }
  // where the track is at now (lat: seconds earlier, the sound's delay; the page's whole lat, the hub's and its own).
  // From the hub's heard positions, only the part they don't already count
  function posModel(d, now, lat = 0) {
    if (visual(d, now)) {
      const l = lat - (latency.full - latency.extra);
      return d.vp.p + d.vp.s * Math.max(-1.2, Math.min((now - d.vp.t) / 1000 - l, .25));
    }
    return d.pt === undefined ? d.pos || 0 : d.pp + d.pv * Math.max(-1.2, Math.min((now - d.pt) / 1000 - lat, .25));
  }
  // What to draw now: for when this frame will be on the screen. It never jumps back by the sound's delay:
  // - let go of the record (d.released: when; d.shown: where the hand left it), the sound is that delay behind the hand:
  //   the drawing goes on at half speed until the sound has caught up, instead of jumping back;
  // - stopped, the sound already on its way out still plays: the drawing runs on, at the speed it had, to where Mixxx
  //   stopped, instead of jumping there.
  // Any other jump (a cue, a loop, a seek) is drawn as it happens.
  function posAt(d, now, lat = 0) {
    frameAt(now);
    const exact = visual(d, now);
    const target = posModel(d, now, lat - ahead());
    const dt = d.shownAt ? Math.min((now - d.shownAt) / 1000, .1) : 0;
    d.shownAt = now;
    let shown = d.shown;
    const v = exact ? d.vp.s : d.pv;
    const dir = v > .05 ? 1 : v < -.05 ? -1 : 0;
    const slack = Math.max(lat, latency.full) + .15, gap = shown === undefined ? 0 : target - shown;
    if (shown === undefined) shown = target;
    else if (!dir) {   // stopped
      const last = d.lastV || 0;
      if (last && gap * last > 0 && Math.abs(gap) <= slack) shown += Math.sign(gap) * Math.min(Math.abs(gap), Math.abs(last) * dt);
      else shown = target;
    } else {
      d.lastV = v;
      const back = -gap * dir;   // how far the target is behind what's drawn
      if (Math.abs(gap) > slack + .25) shown = target;   // a real jump
      else if (back > 0) {
        if (d.released && now - d.released < 4000 && back <= slack + .25) shown += v * dt * .5;   // the sound catching up
        else if (exact ? back > .005 : back > .25) shown = target;   // a jump back (a loop, a cue)
        /* else a hair ahead of the music: hold until it catches up */
      } else shown = exact ? target : shown + gap * .5;   // (the model's guesses are eased; the hub's aren't)
    }
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

  window.ZydeckPos = { hubTime, posSeed, posModel, posAt, posUpdate, posStop, posVisual, waveColumns, clockSync, clockStart,
    clockPong, setLatency, ahead };
})();
