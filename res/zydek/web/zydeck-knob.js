// ZyDeck: touch and knobs, one copy for every page with knobs (the deck page, the phone page's mixer), so a knob turns
// the same way everywhere, by the same setting (Settings › Touch › Knob gesture: Drag, Spin or Pinch).
//
// zydeckTouch(env) sets up the page's touch handling and gives back its parts. env:
//   settings()  the deck page's settings (knobMode, palm, inputMode), read at each touch
//   mm(n)       n millimetres in CSS pixels
//   rect(el)    an element's place on screen (default getBoundingClientRect; the deck page's is cached per layout)
//   tick(el)    a click under the finger (a knob landing on its centre)
//   beat(ctx)   seconds a beat of what the knob controls (the shooting star runs one pass a beat)
//   holdMs()    how long a knob with a hold action is held still for it (default 1000)
//   controls    what a finger in Pinch mode can land on without starting the knob beside it (a selector)
window.zydeckTouch = function (env) {
  const cfg = env.settings, mm = env.mm, cachedRect = env.rect || (el => el.getBoundingClientRect());
  const tick = el => env.tick?.(el), holdMs = () => env.holdMs?.() ?? 1000;
  const controls = env.controls || ".knob, button, input, select, .fader, #xf, .pad, canvas";
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const h = (tag, cls, html) => { const el = document.createElement(tag); if (cls) el.className = cls; if (html != null) el.innerHTML = html; return el; };

  const active = new Map();
  function isPalm(e) {
    return cfg().palm > 0 && e.pointerType === "touch" && Math.max(e.width || 0, e.height || 0) > mm(cfg().palm);
  }
  function drag(el, h) {
    el._drag = h;   // (pinch: a touch beside a knob can start it, pinchOpen)
    el.addEventListener("pointerdown", e => {
      // a control inside another (a knob in an FX pad) has the touch: the outer one leaves it alone
      if (isPalm(e) || e.zydekClaimed) return;
      startDrag(el, h, e);
    });
  }
  function startDrag(el, h, e) {
    e.preventDefault();
    const st = h.down(e);
    if (st === false) return false;
    e.zydekClaimed = true;
    try { el.setPointerCapture(e.pointerId); } catch {}
    active.set(e.pointerId, { h, st });
    return true;
  }
  // Touch moves. Precision: as the browser delivers them, once a frame, lined up with drawing (and with the predicted
  // points the knobs and faders draw ahead with). Real-time: each one the moment it arrives (pointerrawupdate),
  // so controls reach Mixxx up to a frame sooner; only in a secure context (this device's own page).
  const RAW_INPUT = "onpointerrawupdate" in window;
  const realtimeInput = () => cfg().inputMode === "realtime" && RAW_INPUT;
  const onMove = e => { const a = active.get(e.pointerId); if (a && a.h.move) a.h.move(e, a.st); };
  addEventListener("pointermove", e => { if (!realtimeInput()) onMove(e); });
  if (RAW_INPUT) addEventListener("pointerrawupdate", e => { if (realtimeInput()) onMove(e); });
  ["pointerup", "pointercancel"].forEach(t => addEventListener(t, e => {
    const a = active.get(e.pointerId);
    if (a) { active.delete(e.pointerId); if (a.h.up) a.h.up(e, a.st); }
  }));
  // Where the finger is about to be (Chrome predicts a frame or so ahead): knobs and faders are drawn there, so
  // they don't trail the finger by the screen's own delay. Values sent are the real ones.
  const predicted = e => { const p = e.getPredictedEvents?.(); return p && p.length ? p[p.length - 1] : null; };

  // ---- knob: grab and drag only; up or right = clockwise; ~40 mm of drag per full turn; double-tap resets
  function polar(a, r = 15) { const t = (a - 90) * Math.PI / 180; return [20 + r * Math.cos(t), 20 + r * Math.sin(t)]; }
  function arc(a0, a1) {
    if (Math.abs(a1 - a0) < .5) return "";
    const [x0, y0] = polar(Math.min(a0, a1)), [x1, y1] = polar(Math.max(a0, a1));
    return `M${x0} ${y0} A15 15 0 ${Math.abs(a1 - a0) > 180 ? 1 : 0} 1 ${x1} ${y1}`;
  }
  // target(): what the knob controls (e.g. the deck on that side). Locked at touch-down, so a deck switch
  // mid-gesture can't redirect a held control; the knob keeps showing that target until released.
  const SVG = "http://www.w3.org/2000/svg";
  let prSeq = 0;
  // Overlay while spinning a knob: a ring at the finger's radius, a radius line pointing where the knob points,
  // and a "shooting star" that streaks from the centre out to the finger twice, one pass per beat.
  function protractor(beatSec) {
    const svg = document.createElementNS(SVG, "svg"), id = "comet" + (++prSeq);
    svg.setAttribute("class", "protractor");
    svg.innerHTML = `<defs><linearGradient id="${id}" gradientUnits="userSpaceOnUse">` +
      `<stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset="1" stop-color="#fff" stop-opacity=".95"/></linearGradient></defs>` +
      `<circle class="ring"/><line class="ray"/><line class="comet" stroke="url(#${id})"/>`;
    document.body.appendChild(svg);
    const grad = svg.querySelector("linearGradient"), comet = svg.querySelector(".comet");
    svg.geo = null;
    const t0 = performance.now(), passMs = clamp(beatSec, .2, 2) * 1000;
    const step = now => {
      if (!svg.isConnected || !svg.geo) { if (svg.isConnected) requestAnimationFrame(step); return; }
      const t = (now - t0) / passMs;
      if (t >= 2) { comet.style.display = "none"; return; }               // two passes = two beats, then gone
      const { cx, cy, fx, fy } = svg.geo, r = Math.hypot(fx - cx, fy - cy) || 1, ux = (fx - cx) / r, uy = (fy - cy) / r;
      const phase = t % 1, ease = 1 - (1 - phase) ** 2;                    // fast out of the centre, slowing at the finger
      const head = ease * r, tail = Math.max(0, ease - .45) * r;
      const pts = [cx + ux * tail, cy + uy * tail, cx + ux * head, cy + uy * head];
      ["x1", "y1", "x2", "y2"].forEach((k, i) => { comet.setAttribute(k, pts[i]); grad.setAttribute(k, pts[i]); });
      comet.style.opacity = phase > .8 ? (1 - phase) / .2 : 1;            // fade as it reaches the finger
      requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
    return svg;
  }
  function drawProtractor(svg, cx, cy, fx, fy, value) {
    const dist = Math.hypot(fx - cx, fy - cy), r = Math.max(dist, mm(3));
    svg.geo = { cx, cy, fx, fy };
    const ring = svg.querySelector(".ring");
    ring.setAttribute("cx", cx); ring.setAttribute("cy", cy); ring.setAttribute("r", r);
    const a = (-135 + value * 270 - 90) * Math.PI / 180;                  // the knob's own pointer angle
    const ray = svg.querySelector(".ray");
    ray.setAttribute("x1", cx); ray.setAttribute("y1", cy);
    ray.setAttribute("x2", cx + Math.cos(a) * r); ray.setAttribute("y2", cy + Math.sin(a) * r);
    ray.classList.toggle("dead", dist < mm(3));                           // inside the dead zone: no rotation read
  }

  // Pinch: the ring round the knob at the fingers' spread plus 8 mm, the knob's own pointer as the ray, and the shooting star
  // running out along it (over it), twice, a beat a pass
  function drawPinch(svg, cx, cy, p1, p2, value) {
    const r = Math.hypot(p2.x - p1.x, p2.y - p1.y) + mm(8);
    const a = (-135 + value * 270 - 90) * Math.PI / 180, ex = cx + Math.cos(a) * r, ey = cy + Math.sin(a) * r;
    svg.geo = { cx, cy, fx: ex, fy: ey };
    const ring = svg.querySelector(".ring");
    ring.setAttribute("cx", cx); ring.setAttribute("cy", cy); ring.setAttribute("r", r);
    const ray = svg.querySelector(".ray");
    ray.setAttribute("x1", cx); ray.setAttribute("y1", cy); ray.setAttribute("x2", ex); ray.setAttribute("y2", ey);
    ray.classList.remove("dead");
  }
  // A finger landing within PINCH_MM of a knob gesture's one finger (Knob gesture: Pinch), whatever it lands on, can
  // make a pinch with it: the page hands it over before any control sees it. The pinch turns the knob under the
  // fingers' midpoint (pinchKnob), which needn't be the one the first finger started (that one goes back as it was).
  // Two hands can pinch two knobs at once: a finger close to another free knob could be the other hand's first, so it
  // waits PINCH_WAIT_MS for a partner of its own (a hand's two fingers land within ~25 ms of each other) and only then
  // joins the pinch. One finger each on two knobs turns both
  const PINCH_MM = 35, PINCH_WAIT_MS = 100;
  const pinchOpen = new Set();   // knob gestures with one finger, that a second may join
  let pending = null;            // that finger, while it waits: {pid, at, pick, timer}
  // and a finger on nothing in particular within PINCH_REACH_MM of a knob's centre starts that knob: two fingers round
  // a knob land beside it as often as on it
  const PINCH_REACH_MM = 14;
  addEventListener("pointerdown", e => {
    if (cfg().knobMode !== "pinch" || e.pointerType !== "touch" || isPalm(e)) return;
    const at = { x: e.clientX, y: e.clientY }, discs = knobDiscs();
    if (pending) {   // a finger waiting for a partner: this one, if the two pinch a knob nobody has
      const d = pinchKnob(pending.at, at, discs);
      if (d && !d.k.classList.contains("held")) {
        const p = pending;
        settle(null);
        const st = startOn(d.k, p.pid, p.at);
        if (st) { claim(e, st); return; }
      }
    }
    let pick = null;   // the one-finger gesture this finger pinches with, and the knob that turns (the best centred)
    pinchOpen.forEach(st => {
      if (st.p2 || st.pinched || Math.hypot(at.x - st.p1.x, at.y - st.p1.y) > mm(PINCH_MM)) return;
      const d = pinchKnob(st.p1, at, discs);
      if (!d || (d.k !== st.el && d.k.classList.contains("held"))) return;   // (another hand has that one)
      const off = Math.hypot((st.p1.x + at.x) / 2 - d.x, (st.p1.y + at.y) / 2 - d.y);
      if (!pick || off < pick.off) pick = { st, d, off };
    });
    if (pick) {
      if (!pending && (freeKnobNear(pick.st.p1, pick.d, discs) || freeKnobNear(at, pick.d, discs))) {
        pend(e, at, pick);   // (it could be another hand's: a moment to see)
        return;
      }
      const st = pick.d.k === pick.st.el ? pick.st : moveTo(pick.st, pick.d.k);
      if (st) { claim(e, st); return; }
    }
    if (e.target.closest?.(controls)) return;
    const near = reachKnob(at, discs);
    if (near?._drag && startDrag(near, near._drag, e)) e.stopPropagation();
  }, true);

  // The knobs on the screen now: each one's element, centre and radius
  const knobDiscs = () => [...document.querySelectorAll(".knob")].map(k => {
    const r = cachedRect(k.querySelector("svg"));
    return r.width ? { k, x: r.left + r.width / 2, y: r.top + r.height / 2, R: Math.min(r.width, r.height) / 2 } : null;
  }).filter(Boolean);
  // The free knob within PINCH_REACH_MM of a point (the nearest), or null
  const reachKnob = (at, discs) => {
    let near = null, nd = mm(PINCH_REACH_MM);
    discs.forEach(d => {
      if (d.k.classList.contains("held")) return;   // (a knob already in a hand)
      const dd = Math.hypot(at.x - d.x, at.y - d.y);
      if (dd < nd) { nd = dd; near = d.k; }
    });
    return near;
  };
  // What fingers a and b pinch: the knob under their midpoint (within its disc and PINCH_MID_MM more), unless both
  // fingers are right on other knobs' faces (PINCH_FACE of their radius: a finger each on two knobs) or more than
  // PINCH_BETWEEN of another knob's width is under the line between them. That knob's disc, or null
  const PINCH_MID_MM = 2, PINCH_FACE = .7, PINCH_BETWEEN = .5;
  function pinchKnob(a, b, discs) {
    const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
    let best = null, bd = Infinity;
    discs.forEach(d => { const dd = Math.hypot(mx - d.x, my - d.y); if (dd < bd) { bd = dd; best = d; } });
    if (!best || bd > best.R + mm(PINCH_MID_MM)) return null;
    const onFace = f => discs.some(d => d !== best && Math.hypot(f.x - d.x, f.y - d.y) < d.R * PINCH_FACE);
    if (onFace(a) && onFace(b)) return null;
    const dx = b.x - a.x, dy = b.y - a.y, len = Math.hypot(dx, dy);
    if (len < 1) return best;
    const ux = dx / len, uy = dy / len;
    for (const d of discs) {
      if (d === best) continue;
      const t = (d.x - a.x) * ux + (d.y - a.y) * uy, off = Math.abs((d.x - a.x) * uy - (d.y - a.y) * ux);
      if (off >= d.R) continue;
      const half = Math.sqrt(d.R * d.R - off * off);
      if (Math.min(len, t + half) - Math.max(0, t - half) > PINCH_BETWEEN * 2 * d.R) return null;
    }
    return best;
  }
  // Whether finger f is close to a free knob other than k (its centre nearer than PINCH_NEARER of the way to k's): it
  // could be another hand's, starting that one
  const PINCH_NEARER = .75;
  const freeKnobNear = (f, k, discs) => {
    const dk = Math.hypot(f.x - k.x, f.y - k.y);
    return discs.some(d => d !== k && !d.k.classList.contains("held") && Math.hypot(f.x - d.x, f.y - d.y) < PINCH_NEARER * dk);
  };
  // Finger e is st's second: a pinch
  function claim(e, st) {
    e.preventDefault?.(); e.stopPropagation?.(); e.zydekClaimed = true;
    try { st.el.setPointerCapture(e.pointerId); } catch {}
    active.set(e.pointerId, { h: { move: ev => st.move2(ev), up: () => st.up2() }, st });
    st.join(e);
  }
  // A one-finger gesture on knob k for finger pid, down already at `at`
  function startOn(k, pid, at) {
    if (!k?._drag) return null;
    const ev = { clientX: at.x, clientY: at.y, pointerId: pid, pointerType: "touch", preventDefault() {} };
    return startDrag(k, k._drag, ev) ? active.get(pid)?.st || null : null;
  }
  // st moved to knob k (the pinch's midpoint is on k): its own knob goes back as it was
  function moveTo(st, k) {
    st.drop();
    return startOn(k, st.pid, st.p1);
  }
  // A finger that could be a pinch's second or another hand's first: held a moment (its moves followed)
  function pend(e, at, pick) {
    e.preventDefault(); e.stopPropagation(); e.zydekClaimed = true;
    const p = pending = { pid: e.pointerId, at, pick };
    active.set(e.pointerId, { st: p, h: {
      move: ev => { p.at = { x: ev.clientX, y: ev.clientY }; },
      up: () => { if (pending === p) settle(null); },   // (a tap: nothing)
    } });
    p.timer = setTimeout(() => { if (pending === p) settle(p); }, PINCH_WAIT_MS);
  }
  // The wait over. p: nobody came for it, so it joins the pinch it would have (or, that one taken meanwhile, starts
  // the free knob it's on or beside); null: it's been dealt with
  function settle(p) {
    if (pending) clearTimeout(pending.timer);
    pending = null;
    if (!p) return;
    const st0 = p.pick.st;
    if (pinchOpen.has(st0) && !st0.p2 && !st0.pinched) {
      const st = p.pick.d.k === st0.el ? st0 : moveTo(st0, p.pick.d.k);
      if (st) { claim({ clientX: p.at.x, clientY: p.at.y, pointerId: p.pid }, st); return; }
    }
    startOn(reachKnob(p.at, knobDiscs()), p.pid, p.at);
  }

  // Drag (both): distance along the finger's path counts the same in every direction.
  // The path's direction is measured over the last 1 mm travelled (by distance, not per touch event, so it doesn't
  // depend on speed and the screen's ~0.1 mm jitter can't flip it). Each touch step is projected onto that
  // direction instead of adding up step lengths, so jitter cancels out rather than accumulating.
  // Moving towards up/right raises the value. Only along the up-left/down-right diagonals, where that's
  // ambiguous, the previous direction decides (±20° hysteresis), so the sign can't flicker there.
  const AXIS_MM = .6, BORDER = Math.sin(20 * Math.PI / 180);
  function strokeDelta(st, ddx, ddy) {
    const t = st.trail, x = st.x, y = st.y;
    t.push([x, y]);
    if (t.length > 200) t.splice(0, t.length - 200);
    for (let i = t.length - 2; i >= 0; i--) {
      const len = Math.hypot(x - t[i][0], y - t[i][1]);
      if (len < mm(AXIS_MM)) continue;
      let ux = (x - t[i][0]) / len, uy = (y - t[i][1]) / len;
      const screen = (ux - uy) / Math.SQRT2, a = st.axis;     // screen > 0: pointing up/right
      const flip = a && Math.abs(screen) < BORDER ? ux * a[0] + uy * a[1] < 0 : screen < 0;
      st.axis = flip ? [-ux, -uy] : [ux, uy];
      t.splice(0, i);
      break;
    }
    // until the direction is known (the first 0.6 mm): up/right raises it, as it will after, so it moves at once
    if (!st.axis) return (ddx - ddy) / Math.SQRT2;
    return ddx * st.axis[0] + ddy * st.axis[1];
  }

  // hold: { onHold, fill: knobEl => [elements that show the hold filling up] }: held still for holdMs (a second), the knob
  // goes back to where it was and onHold runs; moving more than 5 mm first means it's being turned (no hold).
  function knobHoldOff(st) {
    if (!st.holdT) return;
    clearTimeout(st.holdT); st.holdT = null;
    st.fill.forEach(f => f.classList.remove("holding"));
  }
  function knob(label, get, set, def, bipolar, target = () => null, onTap = null, hold = null) {
    // where the finger has turned it (st.raw); a knob with a centre (bipolar: EQ, filter, CUE MIX) clicks into the
    // centre, held there within 4° of the knob's 270° either side, with a tick as it lands (or turns straight past)
    const CENTRE_DZ = 4 / 270;
    const turnTo = (st, raw) => {
      const prev = st.raw ?? st.v;
      st.raw = clamp(raw, 0, 1);
      if (!bipolar) { st.v = st.raw; return; }
      const centred = Math.abs(st.raw - .5) <= CENTRE_DZ, crossed = (prev - .5) * (st.raw - .5) < 0;
      if ((centred || crossed) && !st.atCentre) tick(el);
      st.atCentre = centred;
      st.v = centred ? .5 : st.raw;
    };
    const el = h("div", "knob", `<svg viewBox="0 0 40 40"><path class="ktrack" d="${arc(-135, 135)}"/><circle class="kbg" cx="20" cy="20" r="11"/>` +
      `<path class="karc"/><line class="kptr" x1="20" y1="20" x2="20" y2="11"/></svg><span>${label}</span>`);
    let lastTap = 0;
    const karc = el.querySelector(".karc"), kptr = el.querySelector(".kptr");
    el.paint = shown => {
      if (shown != null && el.hold) el.hold.shown = shown;   // held: other repaints keep the finger's place
      const v = shown ?? el.hold?.shown ?? get(el.hold ? el.hold.ctx : target());
      if (v === el._v) return;
      el._v = v;
      const a = -135 + v * 270;
      // a knob with a centre, on it: no arc (it'd be a dot), its ring lit instead
      const centred = !!bipolar && v === .5;
      if (el._centred !== centred) { el._centred = centred; el.classList.toggle("centred", centred); }
      karc.setAttribute("d", centred ? "" : arc(bipolar ? 0 : -135, a));
      const [x, y] = polar(a, 10.5);
      kptr.setAttribute("x2", x); kptr.setAttribute("y2", y);
    };
    drag(el, {
      down: e => {
        const now = performance.now(), ctx = target();
        if (def != null && now - lastTap > 70 && now - lastTap < 300) { set(def, ctx); el.paint(); lastTap = 0; return false; }   // (70 ms: a bounce isn't a double tap)
        lastTap = now;
        el.classList.add("held");
        el.hold = { ctx };
        const r = cachedRect(el.querySelector("svg"));
        const st = { x: e.clientX, y: e.clientY, v: get(ctx), ctx, cx: r.left + r.width / 2, cy: r.top + r.height / 2, a: null,
          atCentre: !!bipolar && Math.abs(get(ctx) - .5) <= CENTRE_DZ,   // starting there isn't landing there
          trail: [[e.clientX, e.clientY]], axis: null, pend: [0, 0], t0: now, x0: e.clientX, y0: e.clientY, v0: get(ctx) };
        if (hold) {
          st.fill = hold.fill(el);
          st.fill.forEach(f => { f.style.setProperty("--hold", `${holdMs()}ms`); f.classList.add("holding"); });
          st.holdT = setTimeout(() => {
            st.holdT = null; st.held = true;
            st.fill.forEach(f => f.classList.remove("holding"));
            if (st.v !== st.v0) { st.v = st.v0; set(st.v0, st.ctx); }
            el.paint(st.v0); if (st.pr) { st.pr.remove(); st.pr = null; }
            hold.onHold();
          }, holdMs());
        }
        if (cfg().knobMode === "spin") {
          st.pr = protractor(env.beat(ctx));   // a beat of the knob's deck
          drawProtractor(st.pr, st.cx, st.cy, e.clientX, e.clientY, st.v);
        }
        if (cfg().knobMode === "pinch") {
          // One finger drags; a second (pinchOpen) makes it a pinch: the line between the two is the knob, and
          // 270° of twist its whole range. A second finger within 250 ms: the first one's moment alone doesn't count.
          st.el = el; st.p1 = { x: e.clientX, y: e.clientY }; st.pid = e.pointerId;
          st.drop = () => {   // (the finger moves on to another knob: this one as it was)
            knobHoldOff(st); pinchOpen.delete(st);
            if (st.v !== st.v0) set(st.v0, st.ctx);
            el.classList.remove("held"); el.hold = null; el.paint();
            if (st.pr) { st.pr.remove(); st.pr = null; }
          };
          const angle = () => Math.atan2(st.p2.y - st.p1.y, st.p2.x - st.p1.x);
          st.twist = () => {
            const a = angle();
            let da = a - st.pa;
            if (da > Math.PI) da -= 2 * Math.PI; else if (da < -Math.PI) da += 2 * Math.PI;
            st.pa = a;
            turnTo(st, (st.raw ?? st.v) + da / (1.5 * Math.PI));
            set(st.v, st.ctx); el.paint(st.v); drawPinch(st.pr, st.cx, st.cy, st.p1, st.p2, st.v);   // (the knob itself as it turns)
          };
          st.join = e2 => {
            knobHoldOff(st); lastTap = 0;   // (a quick pinch after this one isn't a double tap)
            if (performance.now() - st.t0 < 250 && st.v !== st.v0) { st.v = st.v0; st.raw = undefined; set(st.v0, st.ctx); }
            st.p2 = { x: e2.clientX, y: e2.clientY }; st.pinch = true; st.pa = angle();
            st.pr = protractor(env.beat(st.ctx));
            drawPinch(st.pr, st.cx, st.cy, st.p1, st.p2, st.v); el.paint(st.v);
          };
          st.move2 = e2 => { if (st.pinch) { st.p2 = { x: e2.clientX, y: e2.clientY }; st.twist(); } };
          // the second finger off: the pinch is over (the finger left on the knob doesn't turn it)
          st.up2 = () => { if (!st.pinch) return; st.pinch = false; st.pinched = true; if (st.pr) { st.pr.remove(); st.pr = null; } };
          pinchOpen.add(st);
        }
        return st;
      },
      move: (e, st) => {
        if (st.held) return;   // it was a hold: the menu's open, the knob stays put
        if (st.p1) st.p1 = { x: e.clientX, y: e.clientY };
        if (st.pinch) { st.twist(); return; }
        if (st.pinched) return;
        if (st.holdT && Math.hypot(e.clientX - st.x0, e.clientY - st.y0) > mm(5)) knobHoldOff(st);
        if (cfg().knobMode === "spin") {
          // Angle around the knob's centre: one full circle of the finger = the knob's whole range.
          // The same finger movement is a smaller angle at a bigger radius, so circling wider = finer.
          const dx = e.clientX - st.cx, dy = e.clientY - st.cy;
          if (Math.hypot(dx, dy) < mm(3)) { st.a = null; if (st.pr) drawProtractor(st.pr, st.cx, st.cy, e.clientX, e.clientY, st.v); return; }
          const a = Math.atan2(dy, dx);
          if (st.a !== null) {
            let da = a - st.a;
            if (da > Math.PI) da -= 2 * Math.PI; else if (da < -Math.PI) da += 2 * Math.PI;
            turnTo(st, (st.raw ?? st.v) + da / (2 * Math.PI));
          }
          st.a = a;
          if (st.pr) drawProtractor(st.pr, st.cx, st.cy, e.clientX, e.clientY, st.v);
        } else {
          const ddx = e.clientX - st.x, ddy = e.clientY - st.y;
          st.x = e.clientX; st.y = e.clientY;
          // dragH: only left/right movement counts · dragV: only up/down movement counts
          const delta = strokeDelta(st, ddx, ddy);
          turnTo(st, (st.raw ?? st.v) + delta / mm(40));
        }
        set(st.v, st.ctx);
        const p = cfg().knobMode !== "spin" && !st.atCentre && predicted(e);
        if (p) {   // drawn a little ahead, where the finger is going
          const px = p.clientX - e.clientX, py = p.clientY - e.clientY;
          const ahead = st.axis ? px * st.axis[0] + py * st.axis[1] : (px - py) / Math.SQRT2;
          el.paint(clamp(st.v + ahead / mm(40), 0, 1));
        } else el.paint();
      },
      up: (e, st) => {
        knobHoldOff(st);
        pinchOpen.delete(st); st.pinch = false;
        el.classList.remove("held"); el.hold = null; el.paint(); if (st.pr) st.pr.remove();
        // a short touch without moving: the knob's tap action (FX: on/off)
        if (onTap && !st.held && !st.p2 && performance.now() - st.t0 < 300 && Math.hypot(e.clientX - st.x0, e.clientY - st.y0) < mm(2)) onTap(st.ctx);
      },
    });
    return el;
  }

  return { active, isPalm, drag, startDrag, RAW_INPUT, realtimeInput, predicted, polar, arc, knob };
};
