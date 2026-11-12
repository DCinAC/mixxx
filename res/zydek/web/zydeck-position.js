// ZyDeck: where a playing track is, and where to draw it: one copy for every page that draws a waveform against
// the sound (the deck page, the phone page's decks, the calibration page), so they can't disagree.
//
// Play position between Mixxx's updates (15 a second, only when it changed). Each update carries the hub's
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
    const j = jogShown(d, now, lat);
    if (j !== undefined) return j;
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
  // Mixxx updates a deck's playposition 15 times a second (enginebuffer.cpp kPlaypositionUpdateRate) and the mapping
  // reads it every 20-33 ms, so a reading is up to that late, and two in a row can say 0.6x or 1.3x.
  // - Playing straight on: the speed is the deck's tempo, and each reading moves the drawing only 15 % of the way to it
  //   (smooth through that jitter).
  // - Scratching or nudging (the last 300 ms say another speed, or a reading lands 40 ms or more off the model): the
  //   speed of the last 150 ms, and up to 80 % of the way, the more the further off. (Measured on the tablet,
  //   2026-11-12: the drawing trailed a scratch by 80-90 ms with only the smooth way; this way about none, and played
  //   back on the same readings, straight play no rougher.)
  function posUpdate(d, p, now, ts) {   // p in seconds
    const t = Math.min(now, hubTime(ts, now));
    if (d.jog) return;   // (drawn from the computer's jog meanwhile: it seeds the model again as it's done)
    const nominal = d.playing ? d.ratio || 1 : 0;
    if (d.pt === undefined || Math.abs(p - posModel(d, t)) > .3) {   // a jump: follow it
      d.hist = [[t, p]];
      return posSeed(d, p, nominal, t);
    }
    const hist = d.hist || (d.hist = []);
    hist.push([t, p]);
    while (hist.length > 2 && t - hist[0][0] > 300) hist.shift();
    const slope = within => {   // track seconds a second through the readings of the last `within` ms
      const h = hist.filter(x => t - x[0] <= within);
      if (h.length < 2) return null;
      const mt = h.reduce((a, x) => a + x[0], 0) / h.length, mp = h.reduce((a, x) => a + x[1], 0) / h.length;
      let num = 0, den = 0;
      for (const [ht, hp] of h) { num += (ht - mt) * (hp - mp); den += (ht - mt) ** 2; }
      return den ? num / den * 1000 : null;
    };
    const pred = posModel(d, t), off = Math.abs(p - pred);
    const steady = hist.length >= 4 ? slope(300) : null;
    let v = nominal, k = .15;
    if ((steady !== null && Math.abs(steady - nominal) > .08) || off > .04) {
      v = slope(150) ?? steady ?? nominal;
      k = Math.min(.8, Math.max(.15, (off - .02) / .06 * .8));
    }
    d.pp = pred + (p - pred) * k; d.pt = t; d.pv = v;
  }
  // ---- the computer's DJ controller's jog (controller mode: ZyDeck Connect passes on its steps as they're turned) ----
  // While its top is touched (and Mixxx scratches: the mapping's call, the page's to make) the record is drawn from the
  // hand: each step moves it 1.8 s / 512 (a 33 1/3 record, the Inpulse's 512 steps a turn) times jogScale, the part
  // Mixxx really moves for one (its scratch clock runs late on macOS: further, learnt by the page as it scratches).
  // Mixxx's play position runs ~100 ms behind a hand, so it's left out until the hand lets go; then for 150 ms the
  // record goes on as Mixxx takes it back (at the deck's speed, or stopped), and the model above takes over from there.
  const JOG_STEP = (60 / (100 / 3)) / 512;
  const jogCfg = { scale: 1 };
  // touch: 0 let go, 1 touched, 2 touched with SHIFT (the mapping seeks: 4 steps a step); ticks: turned since the last
  function posJog(d, touch, ticks, t, scratching) {
    let j = d.jog;
    if (touch && scratching) {
      if (!j || !j.on) {
        const p0 = d.shown ?? posModel(d, t);
        j = d.jog = { on: true, p: p0, hist: [[t, p0]], steps: 0 };
      }
      if (ticks) {
        const n = ticks * (touch === 2 ? 4 : 1);
        j.p += n * JOG_STEP * jogCfg.scale; j.steps += n;
        j.hist.push([t, j.p]);
        while (j.hist.length > 2 && t - j.hist[1][0] > 500) j.hist.shift();
      }
    } else if (j && j.on) { j.on = false; j.off = t; j.hist.push([t, j.p]); }
  }
  function jogAt(j, t) {   // where the hand had it at t
    const h = j.hist;
    if (t >= h[h.length - 1][0]) return h[h.length - 1][1];
    for (let i = h.length - 1; i > 0; i--) if (h[i - 1][0] <= t) {
      const [ta, pa] = h[i - 1], [tb, pb] = h[i];
      return tb > ta ? pa + (pb - pa) * (t - ta) / (tb - ta) : pb;
    }
    return h[0][1];
  }
  function jogShown(d, now, lat) {   // what to draw from the hand (undefined: not now)
    const j = d.jog;
    if (!j) return undefined;
    const at = now - lat * 1000;   // (heard: the sound's delay later, as everything else is drawn)
    if (j.on || at - j.off < 150) {
      let p = jogAt(j, at);
      if (!j.on && at > j.off) p += (d.playing ? d.ratio || 1 : 0) * (at - j.off) / 1000;
      d.shown = p; d.shownAt = now;
      return p;
    }
    d.jog = null;   // let go a while ago: the model again, from here
    posSeed(d, d.shown, d.playing ? d.ratio || 1 : 0, now);
    d.shown = d.pp; d.hist = [[now, d.pp]];
    return undefined;
  }

  // stopped (play off): frozen where Mixxx is
  const posStop = (d, now) => { if (d.pt !== undefined) posSeed(d, posModel(d, now), 0, now); };

  // A waveform's columns, as Mixxx's RGB waveform (allshader/waveformrendererrgb.cpp): per column the loudest
  // low / mid / high mix red / green / blue, scaled so the strongest is full brightness, the height its loudness.
  // wf: {rate, n, data (all, low, mid, high per point; stride bytes a point)}; columns x0.. of width W at pxPerSec.
  function waveColumns(g, wf, x0, W, H, pxPerSec, full = .95) {   // full: a full-scale column's share of the height
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
      const h = Math.min(mid, Math.max(1, all / 255 * mid * full));
      g.fillRect(x, mid - h, 1, h * 2);
    }
  }

  // The slip position (the hub's, every 40 ms while it differs from the deck's; controller mode: Mixxx ZyDeck's control,
  // sent when it changes, and 0 when it's over, so a paused deck's goes quiet and stays), run on between readings at the
  // deck's speed; a reading nearby is eased into, one far off (a loop let go of) is followed.
  function slipModel(d, now) { return d.slipP + (d.playing ? d.ratio || 1 : 0) * (now - d.slipAt) / 1000; }
  function onSlip(d, p) {
    const now = performance.now();
    if (p == null || !(d.dur > 0)) { d.slipAt = undefined; return; }
    const t = p * d.dur, model = d.slipAt === undefined ? t : slipModel(d, now);
    d.slipP = Math.abs(t - model) > .15 ? t : model + (t - model) * .3; d.slipAt = now;
  }

  window.ZydeckPos = { posJog, jogCfg, JOG_STEP, slipModel, onSlip, hubTime, posSeed, posModel, posAt, posUpdate, posStop, posVisual, waveColumns, clockSync, clockStart,
    clockPong, setLatency, ahead };
})();
