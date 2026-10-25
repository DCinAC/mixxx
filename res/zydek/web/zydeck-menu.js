// ZyDeck: the settings menu. One list of every setting (its name, its line, its control, where it's kept) and one
// drawing of it, for both pages: the phone page draws it (sideways a window over the decks, upright its Settings
// page under the waveforms) and so does the deck page on its own (a tablet's browser, a linked device, UI test).
// System settings talk to the hub (/api/...), so a linked device gets the hub's; Controller settings belong to the
// deck page, which hands them over with ZydeckMenu.provide(). Drawn in the Hardware look or Element's
// (zydeck-menu.css), as the Look › Menus setting says.
(function () {
  const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const qs = o => Object.entries(o).map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join("&");
  const bridge = () => { try { return window.ZydekAndroid || parent.ZydekAndroid; } catch { return window.ZydekAndroid; } };
  const store = { get(k, d) { try { const v = localStorage.getItem("zphone." + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
                  set(k, v) { try { localStorage.setItem("zphone." + k, JSON.stringify(v)); } catch {} } };
  async function api(path) {   // the hub's answer, or an Error with its message
    const r = await fetch(path);
    let body = null; try { body = await r.json(); } catch {}
    if (body && body.starting) return body;
    if (!r.ok || (body && body.ok === false)) throw new Error((body && body.error) || r.statusText);
    return body;
  }
  async function webJob(path) {   // zydek_web.py: start a job (waiting out Python's first start), then its result
    let r;
    for (;;) { r = await api(path); if (!r.starting) break; await sleep(1500); }
    if (!r.job) return r;
    for (;;) {
      const j = await api(`/api/web/job?${qs({ id: r.job })}`);
      if (j.state === "done") return j.result;
      if (j.state === "error") throw new Error(j.error);
      await sleep(400);
    }
  }

  // ---- what the menu knows: the hub's state (pushed over /ws by the page, or asked for), the page it's in ----
  const sys = { remote: { on: false }, rec: null, dc: null, opt: null, audio: null, ls: null, prefs: null, acc: null, kits: null,
    tracklist: null, live: false, stemCheck: null };
  let host = { page: "deck", toast: m => console.log(m), ctl: () => null, linked: false, hubHost: location.host };
  const ctl = () => { try { return host.ctl(); } catch { return null; } };
  const S = () => ctl()?.S() || {};
  const isTablet = () => Math.min(screen.width, screen.height) >= 600;
  const upright = () => ui?.layout === "page";   // the phone page's own Settings page (its login and folders pages work there)
  const inApp = () => !!bridge();
  const pl = {};                 // the process lamps' last state, per key: "", "busy", "ok", "err"
  const setPl = (k, s) => { pl[k] = s; refresh(); };
  async function run(k, fn) {    // a process: lamp running while it works, green or red after
    setPl(k, "busy");
    try { const r = await fn(); setPl(k, r === false ? "err" : "ok"); return r; }
    catch (e) { setPl(k, "err"); host.toast(e.message || String(e), "bad"); }
  }
  // a setting both pages keep (the deck page's copy is the one that counts; the phone page reads its own)
  function shared(key, phoneKey, def) {
    return { get: () => { const s = S(); return key in s ? s[key] : store.get(phoneKey, def); },
             set: v => { store.set(phoneKey, v); ctl()?.set(key, v, true); host.changed?.(key, v); } };
  }
  const C = (key, def, after) => ({ get: () => S()[key] ?? def, set: (v, final = true) => { ctl()?.set(key, v, final); after?.(v); } });

  // ---- loading the hub's state the rows need ----
  const loads = {
    audio: async () => { try { sys.audio = await api("/api/audio"); pend.main = pend.phones = null; } catch (e) { sys.audio = { error: e.message }; } },
    ls: async () => { try { sys.ls = await api("/api/livestems"); } catch {} },
    prefs: async () => { try { const p = await api("/api/web/prefs"); if (!p.starting) { sys.prefs = p; if (!sys.stemCheck && p.stemServer) checkStems(); } } catch {} },
    acc: async () => { try { sys.acc = await webJob("/api/web/account"); } catch {} },
    kits: async () => { try { sys.kits = (await api("/api/kits")).kits; } catch { sys.kits = null; } },
    rec: async () => { try { sys.rec = await api("/api/record"); } catch {} },
    dc: async () => { try { sys.dc = await api("/api/discord"); } catch {} },
    tl: async () => { try { sys.tracklist = await api("/api/tracklist"); } catch {} },
  };
  async function loadAll() { await Promise.all(Object.values(loads).map(f => f())); refresh(); }
  async function checkStems() {
    const server = sys.prefs?.stemServer;
    if (!server) { sys.stemCheck = null; pl.stems = ""; refresh(); return; }
    sys.stemCheck = { state: "…" }; setPl("stems", "busy");
    try {
      const h = await webJob(`/api/web/stemcheck?${qs({ server })}`);
      sys.stemCheck = h; setPl("stems", "ok");
    } catch (e) { sys.stemCheck = { error: e.message }; setPl("stems", "err"); }
  }

  // ---- Audio output: applied as it's picked (the pickers' choice until Mixxx has it) ----
  const pend = { main: null, phones: null };
  let applying = false, applyT = null;
  function applyOutputs() {   // a moment after the last pick (a device, then its channels, is one change)
    clearTimeout(applyT);
    applyT = setTimeout(async () => {
      const [main, mainChannels] = outNow("main"), [headphones, headphonesChannels] = outNow("phones");
      applying = true; refresh();
      try {
        const r = await api(`/api/audio/set?${qs({ main, mainChannels, headphones, headphonesChannels })}`);
        host.toast(r.shared ? "Outputs changed: the cue plays on the main output too" : "Outputs changed", r.shared ? "" : "ok");
      } catch (e) { host.toast(e.message, "bad"); }
      applying = false;
      await loads.audio(); refresh();
    }, 400);
  }
  const devOpts = none => [["", none], ...(sys.audio?.devices || []).map(d => [d.name, d.name])];
  function chOpts(device) {
    const d = (sys.audio?.devices || []).find(x => x.name === device), n = d ? d.channels : 2, out = [];
    for (let b = 0; b + 1 < n; b += 2) out.push([`${b},2`, `${b + 1}–${b + 2}`]);
    for (let b = 0; b < n; b++) out.push([`${b},1`, `${b + 1}${b === 0 ? " left" : b === 1 ? " right" : ""}`]);
    return out;
  }
  const outNow = which => pend[which] || (sys.audio ? (which === "main" ? [sys.audio.main || "", sys.audio.mainChannels || "0,2"]
    : [sys.audio.headphones || "", sys.audio.headphonesChannels || "0,2"]) : ["", "0,2"]);
  const chRange = v => { const [b, c] = (v || "0,2").split(",").map(Number); return [b, b + c]; };
  function sharedOut() {
    const [md, mc] = outNow("main"), [pd, pc] = outNow("phones");
    if (!pd || pd !== md) return false;
    const [m0, m1] = chRange(mc), [p0, p1] = chRange(pc);
    return m0 < p1 && p0 < m1;
  }

  // ---- the list ----
  const TABS = { system: "System", controller: "Controller" };
  const SECTIONS = [
    ["system", "conn", "Connections"], ["system", "audio", "Audio"], ["system", "library", "Library"], ["system", "stems", "Stems"],
    ["system", "acc", "Accounts"], ["system", "rec", "Record & stream"], ["system", "look", "Look"], ["system", "start", "Start-up"],
    ["system", "dev", "Developer"],
    ["controller", "decks", "Decks"], ["controller", "fx", "Effects"], ["controller", "sampler", "Sampler"], ["controller", "touch", "Touch"],
    ["controller", "screen", "Screen"],
  ];
  const onOff = (get, set) => ({ kind: "toggle", get, set });
  const remoteText = r => !r.on ? "Off" : r.via === "wifi" ? (r.live ? `Mixxx on ${r.computer || "the computer"} · Wi-Fi` : `Linked to ${r.computer || "the computer"} · not answering`)
    : r.usb === 2 ? (r.live ? "Mixxx on the computer · USB" : "USB MIDI ready · Mixxx not answering")
    : r.usb === 1 ? "Cable in, but not set to MIDI (Android's USB notification)" : "Plug in the USB cable, or start ZyDeck Connect";
  const botLine = b => !b.bot ? "Live · the bot isn't signed in to Discord"
    : b.kicked && !b.inChannel ? `Live · kicked from ${b.kicked.channel || "its channel"}: /zydeck join brings it back`
    : !b.channel ? "Live · in Discord: /zydeck join from a voice channel"
    : `Live in ${b.channel} · ${b.listeners} listening`;
  const DC_STATE = ["Not streaming", "Connecting to the bot…", "Live", "Can't reach the bot", "Live (with problems)"];
  const BUFS = [[3, "5 ms"], [4, "11 ms"], [5, "21 ms"], [6, "43 ms"], [7, "85 ms"]];
  const bufName = i => (BUFS.find(b => b[0] === i) || [0, `buffer ${i}`])[1];
  // Mixxx's output as Android runs it (the hub, from PortAudio's Oboe host): straight to the hardware or through
  // Android's mixer, the queue after Mixxx's block, the underruns
  const pathLine = p => !p || p.mmap === undefined ? ""
    : `${p.mmap === 1 ? "Straight to the hardware" : "Through Android's mixer"} · ${p.queueMs} ms queue · ${p.glitches ? `${p.glitches} glitch${p.glitches > 1 ? "es" : ""}` : "no glitches"}`;
  const BT_WARN = "Bluetooth: the sound comes about 250 ms late. Fine for listening; for mixing, use USB-C headphones.";
  const mmss = s => { s = Math.floor(s || 0); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`; };
  const ROWS = [
    // System › Connections
    { id: "link", sec: "pin", name: "Link devices", kind: "custom", build: buildLink, st: () => linkSt(),
      desc: "Control another ZyDeck on the same Wi-Fi or hotspot from here, like a Bluetooth controller.",
      more: "The tablet as the phone's decks, say: everything you touch goes to the other ZyDeck, which plays the music. ◀ Back to this device returns. The first time, the other ZyDeck asks whether to allow this one." },
    { id: "paired", sec: "pin", name: "Allowed devices", kind: "custom", build: buildPaired, st: () => pairedSt(), show: () => inApp() && !host.linked,
      desc: "The ZyDecks you allowed to control this one over the Wi-Fi. Nothing else on the network can use it.",
      more: "Forget them all, and each one asks again next time." },
    { id: "ctlmode", sec: "conn", name: "Controller mode", ...onOff(() => !!sys.remote.on, v => api(`/api/controller?on=${v ? 1 : 0}`).then(() => { sys.remote.on = v; refresh(); })),
      desc: "Control Mixxx or other DJ software on a computer, over USB or Wi-Fi.", st: () => sys.remote.on ? remoteText(sys.remote) : "",
      more: "Over the USB cable: choose MIDI in Android's USB notification, then in Mixxx open Preferences › Controllers, pick this device and load the ZyDeck mapping. With ZyDeck Connect running on the computer it links over Wi-Fi, so the cable can go; track names and waveforms need ZyDeck Connect too." },
    { id: "midi", sec: "conn", name: "MIDI layout", kind: "seg", opts: () => [["zydeck", "ZyDeck"], ["standard", "Standard"]],
      get: () => sys.remote.layout || "zydeck", set: v => api(`/api/controller?layout=${v}`).then(() => { sys.remote.layout = v; refresh(); }),
      desc: "ZyDeck for Mixxx; Standard for MIDI Learn in Serato, rekordbox or Traktor.",
      more: "Standard sends plain notes and CCs: the decks on MIDI channels 1–4, the mixer and samplers on 16. Those apps don't send back positions or waveforms, so the decks show only what their lamps say." },
    { id: "pcservers", sec: "conn", name: "Pair by QR", kind: "proc", label: () => "Scan QR", lampless: true, show: () => !!bridge()?.scanQr,
      act: async () => { bridge().scanQr(); },
      desc: "ZyDeck Connect on a computer, or a PC's stem server and Discord bot: scan the code it shows.",
      more: "ZyDeck Connect's code links this device to Mixxx on that computer over Wi-Fi, no cable needed. The code ./launch.sh shows on a PC holds the stem server's and the bot's addresses, the stem token and the stream key (./launch.sh qr shows it again; ./launch.sh qr anywhere, the addresses from outside the home network). ZyDeck asks before it changes anything." },
    { id: "tablet", sec: "conn", name: "Tablet controller", kind: "custom", build: buildTablet, show: () => host.page === "phone" && !isTablet() && !host.linked,
      desc: "Open these decks on a tablet: scan the code with its camera, on the same Wi-Fi or this phone's hotspot." },
    // System › Audio
    { id: "audio-remote", sec: "audio", name: "Audio", kind: "note", show: () => !!sys.remote.on, desc: "Set on the computer's Mixxx while controller mode is on." },
    { id: "main", sec: "audio", name: "Main mix", kind: "custom", build: r => buildOut(r, "main"), show: () => !sys.remote.on,
      st: () => { const a = sys.audio; if (applying) return "Applying…"; if (!a) return ""; if (a.error) return a.error;
        if (!a.main && !a.headphones) return "No output: Mixxx is silent";
        if (a.kind === "Bluetooth") return BT_WARN;
        return [a.latencyMs ? a.latencyMs.toFixed(1) + " ms block" : "", pathLine(a.path) || a.api || ""].filter(Boolean).join(" · "); },
      desc: "" },
    { id: "phones", sec: "audio", name: "Headphones (cue)", kind: "custom", build: r => buildOut(r, "phones"), show: () => !sys.remote.on,
      desc: "", st: () => sharedOut() ? "Shares the main's channels: everyone hears what you cue" : "",
      more: "Android plays to one device at a time, so main and headphones go on one device's channels: a USB interface with 4 outputs (main 1–2, headphones 3–4), or one pair of earphones split (main on the left, headphones on the right). “Android: follow the system” plays wherever Android sends music; Bluetooth adds a lot of delay." },
    { id: "buffer", sec: "audio", name: "Audio buffer", kind: "seg", opts: () => BUFS, show: () => !sys.remote.on && !!sys.audio?.devices,
      get: () => sys.audio?.buffer, set: async v => { await api(`/api/audio/buffer?index=${v}`); await loads.audio(); refresh(); },
      desc: "" },
    { id: "optimize", sec: "audio", name: "Optimize", kind: "proc", show: () => !sys.remote.on && !!sys.audio?.devices,
      label: () => sys.opt?.running ? "Cancel" : "Optimize",
      lamp: () => sys.opt?.running ? "busy" : sys.opt?.error ? "err" : sys.opt?.chosen ? "ok" : "",
      st: () => { const o = sys.opt; if (!o) return ""; const tried = (o.results || []).map(r => `${bufName(r.buffer)}: ${r.queueMs != null ? `${r.queueMs} ms queue${r.late ? ", still glitching" : ""}` : r.dropouts ? r.dropouts + " dropouts" : "clean"}`).join(" · ");
        return o.running ? `${o.state || ""} ${tried}` : o.error ? o.error : o.chosen ? `Kept ${bufName(o.chosen)}${tried ? " · " + tried : ""}` : ""; },
      act: async () => { const o = await api(`/api/audio/optimize${sys.opt?.running ? "?cancel=1" : ""}`); if (o && o.state !== undefined) sys.opt = o; },
      desc: "" },
    { id: "latency", sec: "audio", name: "Latency compensation", kind: "slider", min: -200, max: 800, step: 5, show: () => !sys.remote.on && !!sys.audio?.devices,
      get: () => sys.audio?.offsetMs ?? 0, fmt: v => `${v > 0 ? "+" : ""}${v} ms`,
      set: async (v, final) => { if (!final) return; try { const a = await api(`/api/audio/latency?offset=${v}`); Object.assign(sys.audio, a); refresh(); } catch {} },
      st: () => { const a = sys.audio; if (!a || a.totalMs == null) return "";
        const heard = a.streamMs >= 0 ? `heard ${a.streamMs} ms after Mixxx` : a.measuredMs >= 0 ? `${Math.round(a.latencyMs)} ms buffer + ${a.measuredMs} ms output` : "not measured yet";
        return `${a.route ? a.route + ": " : ""}${heard} · drawn ${a.totalMs} ms later${a.kind ? ` · this offset is the ${a.kind === "output" ? "output" : a.kind}'s` : ""}`; },
      extra: { key: "measure", label: "Measure", act: async () => { await api("/api/audio/latency?measure=1"); await sleep(2500); await loads.audio(); } },
      desc: "" },
    { id: "calibrate", sec: "pinctl", name: "Line this screen up", kind: "proc", label: () => "Calibrate", lampless: true,
      show: () => !sys.remote.on && !!ctl(),
      act: async () => {
        const q = qs({ scope: "device", view: S().viewOffset || 0, ...(host.linked ? { hub: host.hubHost } : {}) });
        openPage(`/calibrate?${q}`, () => loads.audio().then(refresh));
      },
      desc: "This screen on top of the host: a linked device's link and display. A rising C E G C plays in the host's headphones; watch, nudge, or tap along.",
      more: "The host's own output is lined up in System › Audio › Calibrate (do that one first, on the host)." },
    { id: "calibratehost", sec: "audio", name: "Line the host up", kind: "proc", label: () => "Calibrate", lampless: true,
      show: () => !sys.remote.on && !host.linked,
      act: async () => { openPage("/calibrate?scope=host", () => loads.audio().then(refresh)); },
      desc: "This device's own output (headphones, Bluetooth, the buffer): a rising C E G C plays in the headphones; watch, nudge, or tap along." },
    // System › Library
    { id: "keyfmt", sec: "library", name: "Key notation", kind: "seg", opts: () => [["camelot", "Camelot 8A"], ["open", "Open Key 1m"], ["musical", "Musical Am"]],
      ...shared("keyNotation", "keyNotation", "camelot"), desc: "How keys are written, on the decks and in the library." },
    { id: "folders", sec: "library", name: "Music folders", kind: "proc", label: () => "Open", lampless: true, show: () => host.page === "phone" && upright(),
      act: async () => { host.go?.("fold"); }, desc: "The folders ZyDeck scans for tracks, and the ones it leaves out." },
    // System › Stems
    { id: "livestems", sec: "stems", name: "Live stems", ...onOff(() => sys.ls ? sys.ls.auto !== false : true, async v => { await api(`/api/livestems?auto=${v ? 1 : 0}`); sys.ls = { ...(sys.ls || {}), auto: v }; refresh(); }),
      disabled: () => sys.ls && sys.ls.available === false,
      desc: "When a deck loads a track without stems, this device separates it (about half a minute) and swaps in the stems version.",
      more: "If the deck is already playing, the STEM pads offer the switch (it makes a short gap). Stems go to Music/ZyDeck Stems and are reused next time. The stem server on a PC still gives the best quality." },
    { id: "stemserver", sec: "stems", name: "Stem server (PC)", kind: "field", fields: [{ ph: "192.168.0.100:8770", get: () => sys.prefs?.stemServer || "", mode: "url" }],
      btn: { key: "stems", label: "Connect", keep: true, act: async ([server]) => {
        server = server.trim() || "192.168.0.100:8770";
        await api(`/api/web/prefs?${qs({ stemServer: server })}`); sys.prefs = { ...(sys.prefs || {}), stemServer: server };
        host.changed?.("stemServer", server); await checkStems(); if (sys.stemCheck?.error) throw new Error(sys.stemCheck.error);
      } },
      st: () => { const c = sys.stemCheck; return !sys.prefs?.stemServer ? "Not set" : !c ? "" : c.error ? c.error : c.gpu ? `Connected · ${c.gpu}${c.busy ? " · separating" : ""}${c.queued ? ` · ${c.queued} queued` : ""}` : "Connecting…"; },
      lamp: () => pl.stems || "",
      desc: "A PC running ZyDeck's stem server: tap a track › Make stems on the PC. Its address and port (8770 if left out).",
      more: "The PC splits tracks into vocals, drums, bass and other; they come back as .stem.m4a files in Music/ZyDeck Web. From anywhere: its https:// address (a Cloudflare tunnel) and its token." },
    { id: "stemtoken", sec: "stems", name: "Stem server token", kind: "field",
      fields: [{ ph: () => sys.prefs?.hasStemToken ? "Saved (type to change)" : "Token", secret: true, get: () => "" }],
      btn: { key: "token", label: "Save", act: async ([token]) => { await api(`/api/web/prefs?${qs({ stemToken: token.trim() })}`); await loads.prefs(); host.toast("Token saved", "ok"); return true; } },
      desc: "Only from outside the home network: stemserver/token on the PC." },
    { id: "stemmodel", sec: "stems", name: "Separation model", kind: "seg",
      opts: () => Object.entries(sys.stemCheck?.presets || { best: "Best", fast: "Fast" }).map(([k, v]) => [k, String(v).split(/ [—-] /)[0]]),
      get: () => sys.prefs?.stemPreset || "best", set: async v => { await api(`/api/web/prefs?${qs({ stemPreset: v })}`); sys.prefs = { ...(sys.prefs || {}), stemPreset: v }; host.changed?.("stemPreset", v); refresh(); },
      desc: "Best: BS-RoFormer (large, slower). Fast: HTDemucs FT." },
    // System › Accounts (logging in is this device's app)
    ...["cn", "intl"].map(site => ({ id: "bili-" + site, sec: "acc", name: site === "cn" ? "Bilibili" : "Bilibili International", kind: "custom", build: r => buildBili(r, site),
      show: () => host.page === "phone" && inApp() && !host.linked && upright(), desc: site === "cn" ? "bilibili.com links and BV codes" : "bilibili.tv links" })),
    { id: "hires", sec: "acc", name: "Hi-Res lossless", ...onOff(() => !!sys.acc?.hiRes, async v => { await api(`/api/web/prefs?${qs({ biliHiRes: v ? 1 : 0 })}`); sys.acc = { ...(sys.acc || {}), hiRes: v }; refresh(); }),
      show: () => host.page === "phone" && inApp() && !host.linked && upright(), desc: "FLAC from Bilibili when the account has premium." },
    { id: "cookies", sec: "acc", name: "Paste cookies", kind: "custom", build: buildCookies, show: () => host.page === "phone" && inApp() && !host.linked && upright(),
      desc: "Instead of logging in: SESSDATA=…; bili_jct=… or a cookies.txt.",
      more: "You log in on Bilibili's own page; ZyDeck keeps only the session cookie, privately on this device, and uses it for downloads (no more blocks, and Hi-Res audio with premium). Sessions run out after a while: log in again then." },
    { id: "acc-up", sec: "acc", name: "Bilibili accounts", kind: "note", show: () => host.page === "phone" && inApp() && !host.linked && !upright(),
      desc: "Logging in opens Bilibili's own page: hold the device upright, then Settings › Accounts." },
    // System › Recording & streaming
    { id: "record", sec: "rec", name: "Record", kind: "toggle", lampc: "rec", onLabel: "Stop", offLabel: "Record", show: () => !sys.remote.on,
      get: () => !!sys.rec?.on, set: async v => { await api(`/api/record?on=${v ? 1 : 0}`); if (!v) host.toast("Saved in Music/ZyDeck Recordings, with a .cue tracklist", "ok"); await loads.rec(); refresh(); },
      st: () => { const r = sys.rec; if (!r) return ""; return r.status === 2 ? `Recording · ${mmss(r.seconds)} · ${(r.file || "").split("/").pop()}` : r.status === 1 ? "Starting…" : "Not recording"; },
      desc: "The main output as FLAC in Music/ZyDeck Recordings, with a .cue tracklist beside it." },
    { id: "recpoint", sec: "rec", name: "Recording point", kind: "seg", opts: () => [[false, "After everything"], [true, "Before effects"]], show: () => !sys.remote.on,
      get: () => !!sys.rec?.preFx, set: async v => { sys.rec = await api(`/api/record?prefx=${v ? 1 : 0}`); refresh(); },
      desc: "After everything: what's heard. Before effects: the effect units left out (EQ, filter and faders stay)." },
    { id: "tracklist", sec: "rec", name: "This session's tracklist", kind: "custom", build: buildTracklist, show: () => !sys.remote.on,
      desc: "What's played since ZyDeck started, as Mixxx's history has it." },
    { id: "discord", sec: "rec", name: "Discord", kind: "toggle", onLabel: "Live", offLabel: "Go live",
      get: () => !!sys.dc?.on, set: async v => { sys.dc = await api(`/api/discord?on=${v ? 1 : 0}`); refresh(); },
      st: () => { const d = sys.dc; if (!d) return ""; if (!d.host || !d.hasKey) return "Set the bot's address and key below"; return !d.on ? `Bot: ${d.host}` : d.status === 2 && d.bot ? botLine(d.bot) : (DC_STATE[d.status] || "…") + (d.status === 3 && d.error ? ` · ${d.error}` : ""); },
      desc: "Plays the mix in a Discord voice channel through the ZyDeck Discord bot. Listeners hear it about a second behind.",
      more: "The bot runs on a computer or a server (launch_bot.sh). In Discord: /zydeck join from a voice channel." },
    { id: "dcbot", sec: "rec", name: "Discord bot", kind: "field",
      fields: [{ ph: "Address: 192.168.0.100:8780 or https://…", get: () => sys.dc?.host || "", mode: "url" },
               { ph: () => sys.dc?.hasKey ? "Stream key: saved (type to change)" : "Stream key", secret: true, get: () => "" }],
      btn: { key: "dc", label: "Save", act: async ([h, k]) => { sys.dc = await api(`/api/discord?${qs(k.trim() ? { host: h.trim(), key: k.trim() } : { host: h.trim() })}`); host.toast("Saved", "ok"); return true; } },
      desc: "Its address, and the stream key from the bot's .env." },
    // System › Look
    { id: "theme", sec: "look", name: "Controller UI", kind: "seg", opts: () => [["d", "Hardware"], ["element", "Element"]], ...C("theme", "d"),
      desc: "The decks. Hardware: keys in a frame with a lamp, rubber pads lit from behind, a jog wheel's edge for nudging. Element: ZyDeck's flat look." },
    { id: "menulook", sec: "look", name: "Menu UI", kind: "seg", opts: () => [["d", "Hardware"], ["element", "Element"]], ...C("menuLook", "d", () => paintLook()),
      desc: "The menus: these settings, the FX menu and the track menu. The library keeps its own look." },
    { id: "cuetime", sec: "look", name: "Hot cue time", kind: "seg", opts: () => [["off", "Off"], ["left", "Left"], ["right", "Right"]], ...C("cueTime", "off"),
      desc: "Each hot cue pad: where in the track the cue is, in its lower left or right corner." },
    { id: "knobcentre", sec: "look", name: "Knob at centre", kind: "seg", opts: () => [["lit", "Ring lit"], ["dark", "Dark"]], ...C("knobCentre", "lit"),
      desc: "A knob with a centre (gain, EQ, filter), on it: its ring lit, or left dark." },
    { id: "lampside", sec: "look", name: "Lamp on wide keys", kind: "seg", opts: () => [["left", "Left"], ["right", "Right"], ["bottom", "Bottom"]], ...C("lampSide", "left"),
      desc: "Hardware decks: where the lamp sits on the long, flat keys." },
    { id: "keytext", sec: "look", name: "Key text", kind: "seg", opts: () => [["adaptive", "Adaptive"], ["element", "Fixed"]], ...C("keyText", "adaptive"),
      desc: "Hardware decks: adaptive sizes each key's text to the key, the same along a row; fixed keeps Element's sizes." },
    { id: "drawmode", sec: "look", name: "Drawing", kind: "seg", opts: () => [["layers", "Pre-rendered"], ["live", "Live"]], ...C("drawMode", "layers"),
      desc: "Pre-rendered draws each key, pad, knob and fader cap once and keeps it, so a touch redraws only what changed. They look the same." },
    // System › Start-up (this device's app)
    { id: "intro", sec: "start", name: "Intro", kind: "seg", opts: () => [["gild", "Gold leaf"], ["drop", "Drop"]], show: () => inApp() && !host.linked,
      get: () => store.get("introStyle", host.introDefaults?.style || "gild"), set: v => { store.set("introStyle", v); host.toast("Shows next time ZyDeck starts", "ok"); },
      desc: "Gold leaf: a clay brick turns to gold, its waveform becomes the decks' tape. Drop: the brick falls in and bounces up as the decks." },
    { id: "introfull", sec: "start", name: "Full intro every time", ...onOff(() => store.get("introFull", host.introDefaults?.full ?? false), v => store.set("introFull", v)), show: () => inApp() && !host.linked,
      desc: "Off: the full intro plays only the very first time; after that, just the logo. A tap skips it." },
    // System › Developer
    { id: "telemetry", sec: "dev", name: "Telemetry", kind: "toggle", get: () => !!S().telemetry && !!S().telemetryAsked,
      set: (v, final = true) => { ctl()?.set("telemetryAsked", true, false); ctl()?.set("telemetry", v, final); }, show: () => ctl() && !ctl().phone,
      desc: "Shares performance data with ZyDeck's debug tools on your network: frame rate, touch timing, the decks' state and the controls you press. No track names, library or anything personal. Off unless you say yes." },

    // Controller › Decks
    { id: "newtrack", sec: "decks", name: "On a new track", kind: "custom", build: buildNewTrack, show: () => !!ctl(),
      desc: "KEY LOCK, QUANTIZE and SLIP when a deck loads a track: on, off, or as they were." },
    { id: "dir", sec: "decks", name: "Tempo direction", kind: "seg", opts: () => [[true, "Down = faster"], [false, "Up = faster"]], ...C("fasterDown", true),
      desc: "Which way along the tempo lane plays faster." },
    { id: "revs", sec: "decks", name: "Platter strip", kind: "seg", opts: () => [[.5, "½ rev"], [1, "1 rev"], [2, "2 rev"]], ...C("revs", 1),
      st: () => ctl()?.info?.revs?.() || "", desc: "How much of the record one full strip covers." },
    { id: "unload", sec: "decks", name: "Unload", kind: "seg", opts: () => [[true, "Ask every time"], [false, "Don't ask"]], ...C("askUnload", true),
      desc: "Hold a paused deck's title for half a second: the eject sign comes up beside it; tap it to unload." },
    { id: "ejectat", sec: "decks", name: "Eject sign", kind: "seg", multi: true, opts: () => [["left", "Left"], ["leftmid", "Left-mid"], ["middle", "Middle"], ["right", "Right"]],
      get: () => ctl()?.ejectPlaces?.() || ["leftmid"], set: v => ctl()?.toggleEject?.(v),
      desc: "Where it comes up on the waveform: any of them (left and left-mid would overlap, so one of those)." },
    // Controller › Effects
    { id: "fxlayout", sec: "fx", name: "FX layout", kind: "seg", opts: () => [["beat", "Beat FX"], ["multi", "Multi-FX"]], ...C("fxLayout", "beat"),
      desc: "Beat FX: one effect per side with its beat length, LEVEL and ON, like Pioneer's. Multi-FX: three effects with a knob each and MIX, like Serato's." },
    // Controller › Sampler
    { id: "ssync", sec: "sampler", name: "Follow the master tempo", kind: "toggle", ...shared("sampleSync", "sampleSync", true),
      desc: "What a pad gets when a track, capture or cut sample goes on it. SHIFT + a loaded pad changes it for that pad." },
    { id: "skl", sec: "sampler", name: "Keylock", kind: "toggle", ...shared("sampleKeylock", "sampleKeylock", true),
      desc: "The sample keeps its pitch when its tempo changes. SHIFT + a loaded pad changes it for that pad." },
    { id: "kits", sec: "sampler", name: "Sampler kits", kind: "custom", build: buildKits, show: () => !sys.remote.on,
      desc: "What's on the 32 sampler pads, with each pad's mode and tempo setting. Loading one fills the pads." },
    // Controller › Touch
    { id: "input", sec: "touch", name: "Touch input", kind: "seg", show: () => !!ctl(),
      opts: () => ctl()?.rawInput ? [["precision", "Precision"], ["realtime", "Real-time"]] : [["precision", "Precision"]], ...C("inputMode", "precision"),
      st: () => ctl() && !ctl().rawInput ? "Real-time needs ZyDeck's own app on this device" : "",
      desc: "Precision: moves line up with the screen, drawn ahead of the finger. Real-time: each move goes to Mixxx the moment it arrives." },
    { id: "knob", sec: "touch", name: "Knob gesture", kind: "seg", opts: () => [["drag", "Drag"], ["spin", "Spin"], ["pinch", "Pinch"]], ...C("knobMode", "drag"),
      desc: "Drag: up or right turns it up. Spin: circle the knob; the farther out, the finer. Pinch: two fingers on the knob, twisted as a real one (one finger still drags)." },
    { id: "sens", sec: "touch", name: "Tempo sensitivity", kind: "slider", min: .04, max: .4, step: .01, ...C("sens", .16),
      fmt: v => `${(+v).toFixed(2)} %/mm`, st: () => { const v = S().sens || .16; return `±8 % over ${(8 / v).toFixed(0)} mm of drag`; }, desc: "Tempo change per mm of drag." },
    { id: "nudge", sec: "touch", name: "Nudge sensitivity", kind: "slider", min: .1, max: 1, step: .05, ...C("nudgeSens", .3),
      fmt: v => `${Math.round(v * 100)} %`, desc: "How much the nudge bars bend the speed: lower is finer." },
    { id: "catchup", sec: "touch", name: "Slipmat grip", kind: "slider", min: 0, max: 800, step: 10, ...C("catchup", 180),
      fmt: v => +v ? `${v} ms` : "Off", desc: "After you let go, the time from a standstill back to full speed." },
    { id: "palm", sec: "touch", name: "Palm rejection", kind: "slider", min: 0, max: 30, step: 1, ...C("palm", 18), show: () => ctl() && !ctl().phone,
      fmt: v => +v ? `${v} mm` : "Off", desc: "Ignores touches wider than this." },
    // Controller › Screen
    { id: "layout", sec: "screen", name: "Layout", kind: "seg", opts: () => [["stacked", "Strips on top"], ["side", "Side by side"]], ...C("layout", "stacked"), show: () => ctl() && !ctl().phone,
      desc: "Strips on top: both platter strips across the top, like Mixxx's waveforms (about twice as long)." },
    { id: "autofs", sec: "screen", name: "Fullscreen on first touch", kind: "toggle", ...C("autoFs", true), show: () => ctl() && !ctl().phone && !inApp(),
      desc: "In a browser: the first touch goes fullscreen." },
    { id: "endwarn", sec: "screen", name: "Track end warning light", kind: "toggle", ...C("endWarn", true), show: () => !!ctl(),
      desc: "In a playing track's last 30 seconds, its waveform flashes pink, as Mixxx's overview does (on the phone's library the time left blinks with it)." },
    { id: "padbeat", sec: "screen", name: "Pad mode light keeps the beat", kind: "toggle", ...C("padBeat", true), show: () => !!ctl(),
      desc: "While the deck plays, the chosen pad mode's light flashes with the beat: lit on each beat, dark half way to the next." },
    { id: "knobsize", sec: "screen", name: "Mixer knob size", kind: "slider", min: 9, max: 18, step: .5, ...C("knobSize", 10.5), show: () => !!ctl(),
      fmt: v => `${v} mm`, st: () => ctl()?.info?.knob?.() || "", desc: "Knob diameter; the mixer widens to fit." },
    { id: "bar", sec: "screen", name: "Top bar height", kind: "slider", min: 12, max: 28, step: 1, ...C("bar", 15), show: () => ctl() && !ctl().phone,
      fmt: v => `${v} mm`, desc: "The row with the waveforms and the top-bar keys." },
    { id: "wavemode", sec: "screen", name: "Waveform rendering mode", kind: "seg", opts: () => [["live", "Live render"], ["extreme", "Live render · extreme"]], ...C("waveMode", "live"),
      desc: "Live render: stems drawn once by the phone's Mixxx, and the gain only changes the height. Extreme: everything drawn again as it changes, the EQ included." ,
      more: "Live render costs almost nothing while you play: each stem is its own layer, so muting one just folds it away. Extreme redraws what's on the strip (at most 16 times a second) whenever an EQ, gain or stem moves: closest to what you hear, more work for the screen." },
    { id: "pxmm", sec: "screen", name: "Screen scale", kind: "slider", min: 3.5, max: 8.5, step: .01, ...C("pxmm", 5.93), ruler: true,
      fmt: v => `${(+v).toFixed(2)} px/mm`, desc: "Adjust until the bar below measures 50 mm with a ruler." },
  ];
  const ROW = Object.fromEntries(ROWS.map(r => [r.id, r]));
  ROWS.forEach(r => { r.tab = r.sec === "pinctl" ? "controller" : (SECTIONS.find(s => s[1] === r.sec) || ["both"])[0]; });
  // "pin": Link devices, first in both tabs; "pinctl": right under it, in the Controller tab only

  // ---- drawing ----
  const PL = '<i class="zpl" aria-hidden="true"><i></i><i></i><i></i><i></i></i>';
  // pick any: a window cut as a tick, lit from behind when it's on
  const CHECK = '<i class="zc" aria-hidden="true"><svg viewBox="0 0 16 16"><path d="M3.2 8.6l3.1 3.1 6.5-7"/></svg></i>';
  const key = (label, cls = "") => el("button", "zk " + cls, label);
  const procKey = (label, k) => { const b = key(`<span>${esc(label)}</span>${PL}`, "proc"); b.dataset.pl = k; return b; };
  function lampOf(b, s) { const p = b.querySelector(".zpl"); if (p) p.className = "zpl" + (s ? " " + s : ""); }

  let ui = null;                 // the drawn menu: {root, layout, tab, rows: [{r, el, upd}], ...}
  let curTab = "system";
  function paintLook() { if (ui) ui.root.classList.toggle("hw", (S().menuLook || "d") === "d"), ui.root.classList.toggle("el", (S().menuLook || "d") !== "d"); }

  function rowEl(r) {
    const e = el("div", "zm-row" + (r.kind === "note" ? " note" : "") + (r.bare ? " bare" : ""));
    e.dataset.id = r.id;
    const lbl = el("div", "zm-lbl", `<b>${esc(r.name)}${r.more ? '<button class="zm-more" aria-label="More about this">i</button>' : ""}</b>${r.desc ? `<small>${esc(r.desc)}</small>` : ""}`);
    const more = r.more ? el("p", "zm-moretext", esc(r.more)) : null;
    if (more) { more.hidden = true; lbl.appendChild(more); lbl.querySelector(".zm-more").addEventListener("click", () => { more.hidden = !more.hidden; }); }
    const c = el("div", "zm-ctl"), st = el("span", "zm-st");
    c.appendChild(st);
    e.append(lbl, c);
    const upds = [];
    const disabled = () => !!r.disabled?.();
    switch (r.kind) {
      case "toggle": {
        const b = key("", "tog" + (r.lampc ? " " + r.lampc : ""));
        b.innerHTML = '<i class="zl" aria-hidden="true"></i><span></span>';
        b.addEventListener("click", async () => { const v = !r.get(); try { await r.set(v); } catch (err) { host.toast(err.message, "bad"); } refresh(); });
        c.appendChild(b);
        upds.push(() => { const on = !!r.get(); b.classList.toggle("on", on); b.setAttribute("aria-pressed", on); b.disabled = disabled();
          b.querySelector("span").textContent = on ? (r.onLabel || "On") : (r.offLabel || "Off"); });
        break;
      }
      case "seg": {   // pick one: a bank; pick any (multi): separate keys, each with a checkmark window
        const box = el("div", r.multi ? "zseg multi" : "zseg");
        c.appendChild(box);
        let sig = "";
        upds.push(() => {
          const opts = r.opts(), s2 = JSON.stringify(opts);
          if (s2 !== sig) {
            sig = s2; box.innerHTML = "";
            opts.forEach(([v, label]) => {
              const b = key(`${r.multi ? CHECK : '<i class="zs" aria-hidden="true"></i>'}${esc(label)}`, r.multi ? "s m" : "s");
              b.addEventListener("click", async () => { try { await r.set(v); } catch (err) { host.toast(err.message, "bad"); } refresh(); });
              b._v = v; box.appendChild(b);
            });
          }
          const cur = r.get();
          [...box.children].forEach(b => { const on = r.multi ? (cur || []).includes(b._v) : b._v === cur; b.classList.toggle("on", on); b.setAttribute("aria-pressed", on); b.disabled = disabled(); });
        });
        break;
      }
      case "slider": {
        const box = el("div", "zsld"), inp = el("input"), rd = el("span", "zrd");
        Object.assign(inp, { type: "range", min: r.min, max: r.max, step: r.step });
        inp.setAttribute("aria-label", r.name);
        inp.addEventListener("input", () => { rd.textContent = r.fmt(+inp.value); r.set(+inp.value, false); });
        inp.addEventListener("change", () => { r.set(+inp.value, true); refresh(); });
        box.append(inp, rd); c.appendChild(box);
        if (r.ruler) c.appendChild(el("div", "zruler"));
        upds.push(() => { if (document.activeElement !== inp) inp.value = r.get(); rd.textContent = r.fmt(+inp.value); inp.style.setProperty("--p", `${(inp.value - r.min) / (r.max - r.min) * 100}%`); });
        inp.addEventListener("input", () => inp.style.setProperty("--p", `${(inp.value - r.min) / (r.max - r.min) * 100}%`));
        break;
      }
      case "field": {
        const ins = r.fields.map(f => {
          const i = el("input", "zf"); i.type = f.secret ? "password" : "text"; i.autocomplete = "off"; i.spellcheck = false;
          i.setAttribute("autocapitalize", "off"); if (f.mode) i.inputMode = f.mode;
          i.setAttribute("aria-label", typeof f.ph === "function" ? f.ph() : f.ph);
          c.appendChild(i); return i;
        });
        // saved as it's left (Enter, or tapping away): no key to press; a lamp beside it says how it went. With
        // btn.keep, a key instead (Connect: also to try again as it is), and Enter presses it
        const b = r.btn.keep ? procKey(r.btn.label, r.btn.key) : el("span", "zm-autolamp", PL);
        const save = () => run(r.btn.key, async () => { const ok = await r.btn.act(ins.map(i => i.value)); r.fields.forEach((f, k) => { if (f.secret) ins[k].value = ""; ins[k]._typed = false; }); return ok; }).then(refresh);
        ins.forEach(i => {
          if (!r.btn.keep) i.addEventListener("change", () => { if (i.value.trim() || !r.fields[ins.indexOf(i)].secret) save(); });
          i.addEventListener("keydown", ev => { if (ev.key === "Enter") { ev.preventDefault(); i.blur(); if (r.btn.keep) save(); } });
        });
        if (r.btn.keep) b.addEventListener("click", save);
        c.appendChild(b);
        upds.push(() => {
          r.fields.forEach((f, k) => { const i = ins[k]; i.placeholder = typeof f.ph === "function" ? f.ph() : f.ph; if (document.activeElement !== i && !f.secret && !i._typed) i.value = f.get(); });
          lampOf(b, r.lamp ? r.lamp() : pl[r.btn.key] || "");
        });
        ins.forEach(i => i.addEventListener("input", () => { i._typed = true; }));
        break;
      }
      case "proc": {
        const b = r.lampless ? key(esc(r.label())) : procKey(r.label(), r.id);
        b.addEventListener("click", () => run(r.id, r.act).then(refresh));
        c.appendChild(b);
        upds.push(() => { const s = b.querySelector("span") || b; s.textContent = r.label(); b.disabled = disabled(); if (!r.lampless) lampOf(b, r.lamp ? r.lamp() : pl[r.id] || ""); });
        break;
      }
      case "custom": upds.push(r.build(c, e) || (() => {}));
    }
    if (r.extra) {
      const b = procKey(r.extra.label, r.extra.key);
      b.addEventListener("click", () => run(r.extra.key, r.extra.act).then(refresh));
      c.appendChild(b);
      upds.push(() => lampOf(b, pl[r.extra.key] || ""));
    }
    st.hidden = true;
    if (r.st) upds.push(() => { st.textContent = r.st(); st.hidden = !st.textContent; });
    return { r, el: e, upd: () => upds.forEach(f => { try { f(); } catch (err) { console.warn("menu", r.id, err); } }) };
  }

  function draw() {
    if (!ui) return;
    const q = ui.q.value.trim().toLowerCase(), words = q.split(/\s+/).filter(Boolean);
    const hit = r => words.every(w => `${r.name} ${r.desc || ""} ${r.more || ""} ${(SECTIONS.find(s => s[1] === r.sec) || [])[2] || ""}`.toLowerCase().includes(w));
    ui.tabs.forEach(b => { const on = !words.length && b._tab === curTab; b.classList.toggle("on", on); b.setAttribute("aria-selected", on); });
    ui.rows.replaceChildren(ui.pinBox);
    ui.rail.replaceChildren();
    ui.list = [];
    // under Link devices, in the Controller tab (or when it's searched for)
    ROWS.filter(r => r.sec === "pinctl" && (!r.show || r.show()) && (words.length ? hit(r) : curTab === "controller")).forEach(r => {
      const x = rowEl(r); x.el.classList.add("pinned"); ui.list.push(x); ui.rows.appendChild(x.el);
    });
    const secs = SECTIONS.filter(s => words.length || s[0] === curTab);
    secs.forEach(([tab, id, name]) => {
      const rows = ROWS.filter(r => r.sec === id && (!r.show || r.show()) && (!words.length || hit(r)));
      if (!rows.length) return;
      const h = el("div", "zm-sec", esc(words.length ? `${TABS[tab]} › ${name}` : name));
      h.id = "zm-sec-" + id;
      ui.rows.appendChild(h);
      rows.forEach(r => { const x = rowEl(r); ui.list.push(x); ui.rows.appendChild(x.el); });
      if (!words.length) {
        const b = key(`<i class="zs" aria-hidden="true"></i>${esc(name)}`, "r");
        b._sec = id;
        b.addEventListener("click", () => { const t = h.offsetTop - ui.rows.offsetTop; (ui.layout === "page" ? scrollToSec(h) : ui.rows.scrollTo({ top: t - 4 })); spy(id); });
        ui.rail.appendChild(b);
      }
    });
    if (words.length && !ui.list.length) ui.rows.appendChild(el("div", "zm-none", "Nothing matches in either tab."));
    if (!words.length && curTab === "controller" && ctl()) {
      const reset = key("Reset", "r reset");
      reset.addEventListener("click", () => { if (confirm("Put every Controller setting back as it came?")) { ctl().reset(); refresh(); } });
      ui.rail.appendChild(el("div", "grow")); ui.rail.appendChild(reset);
    }
    if (!words.length && curTab === "controller" && !ctl()) ui.rows.appendChild(el("div", "zm-none", "The decks aren't open yet."));
    refresh();
    spy(secs.find(s => ui.rows.querySelector("#zm-sec-" + s[1]))?.[1]);
  }
  function spy(id) { if (!ui) return; [...ui.rail.children].forEach(b => b._sec && b.classList.toggle("on", b._sec === id)); }
  // upright: the head and the section keys stick under the page's waveforms; this is where they end
  const stuckBottom = () => (parseFloat(getComputedStyle(ui.root).getPropertyValue("--zm-top")) || 0) + ui.head.offsetHeight + ui.rail.offsetHeight;
  function scrollToSec(h) { scrollTo({ top: h.getBoundingClientRect().top + scrollY - stuckBottom() - 4 }); }
  function onScroll() {
    if (!ui || ui.q.value.trim()) return;
    const box = ui.layout === "page" ? null : ui.rows;
    const lim = (box ? box.getBoundingClientRect().top : stuckBottom()) + 40;
    let cur = null;
    ui.rows.querySelectorAll(".zm-sec").forEach(h => { if (h.getBoundingClientRect().top <= lim) cur = h.id.slice(7); });
    spy(cur || ui.rows.querySelector(".zm-sec")?.id.slice(7));
  }
  let refreshing = false;
  function refresh() {
    if (!ui || refreshing) return;
    refreshing = true;
    requestAnimationFrame(() => { refreshing = false; if (ui) { paintLook(); ui.pin.upd(); ui.list.forEach(x => x.upd()); paintHeadState(); } });
  }
  function paintHeadState() {
    if (!ui) return;
    ui.back.hidden = !host.linked;
    ui.back.querySelector("span").textContent = "◀ Back to this device";
    ui.fs.hidden = ui.layout === "page" || !document.fullscreenEnabled || inApp();
    ui.fs.textContent = document.fullscreenElement ? "Exit fullscreen" : "Fullscreen";
  }

  /// Draws the menu into box: layout "window" (a window over the page; box is its overlay) or "page" (box is
  /// the page's own place for it).
  function build(box, layout) {
    const root = el("div", `zm ${layout}`);
    root.innerHTML = `<div class="zm-box" role="dialog" aria-label="Settings">
      <div class="zm-head"><div class="zm-tabs zseg" role="tablist"></div>
        <label class="zm-q"><span class="zm-vh">Search settings</span><input class="zf" type="search" placeholder="Search both tabs" autocomplete="off" spellcheck="false"></label>
        <button class="zk zm-back"><span></span></button><button class="zk zm-fs"></button>
        <button class="zk zm-x" aria-label="Close"><svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 3l10 10M13 3L3 13" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/></svg></button></div>
      <div class="zm-body"><div class="zm-rail" role="navigation" aria-label="Sections"></div><div class="zm-rows"></div></div></div>`;
    box.replaceChildren(root);
    const $ = s => root.querySelector(s);
    ui = { root, layout, head: $(".zm-head"), q: $(".zm-q input"), rail: $(".zm-rail"), rows: $(".zm-rows"), back: $(".zm-back"), fs: $(".zm-fs"), x: $(".zm-x"), list: [] };
    // Link devices: first in both tabs, the same one (drawn once, put back on top when the list changes), so it
    // doesn't move when the tab does; it scrolls with the list
    ui.pin = rowEl(ROW.link);
    ui.pinBox = el("div", "zm-link");
    ui.pinBox.appendChild(ui.pin.el);
    if (layout === "page") new ResizeObserver(() => root.style.setProperty("--zm-headh", ui.head.offsetHeight + "px")).observe(ui.head);
    ui.tabs = Object.entries(TABS).map(([t, name]) => {
      const b = key(`<i class="zs" aria-hidden="true"></i>${name}`, "s");
      b._tab = t; b.setAttribute("role", "tab");
      b.addEventListener("click", () => { ui.q.value = ""; curTab = t; draw(); if (layout === "page") scrollTo({ top: 0 }); else ui.rows.scrollTop = 0; });
      $(".zm-tabs").appendChild(b); return b;
    });
    ui.q.addEventListener("input", draw);
    ui.back.addEventListener("click", () => bridge()?.linkClose?.());
    ui.fs.addEventListener("click", () => { host.fsToggled?.(); (document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen({ navigationUI: "hide" })).catch(() => {}); close(); });
    ui.x.hidden = layout === "page";
    ui.x.addEventListener("click", close);
    if (layout === "window") { box.onclick = e => { if (e.target === box) close(); }; ui.rows.addEventListener("scroll", onScroll, { passive: true }); }
    paintLook();
    draw();
  }
  addEventListener("scroll", () => { if (ui?.layout === "page") onScroll(); }, { passive: true });
  document.addEventListener("fullscreenchange", refresh);

  let winBox = null, pageBox = null;   // the page's place, drawn again when a window over it closes
  function open({ tab, layout = "window", into, q } = {}) {
    if (tab) curTab = tab;
    if (layout === "page") pageBox = into;
    if (layout === "window") {
      if (!winBox) { winBox = el("div", "zm-win"); document.body.appendChild(winBox); }
      winBox.style.setProperty("--zmm", host.mm?.() || "5.93px");
      winBox.classList.add("open");
      build(winBox, "window");
    } else build(into, "page");
    if (q) { ui.q.value = q; draw(); }
    loadAll();
  }
  function close() {
    if (ui?.layout === "window") {
      winBox.classList.remove("open"); winBox.replaceChildren(); ui = null; host.closed?.();
      if (pageBox?.isConnected) build(pageBox, "page");
    }
  }
  const isOpen = () => !!ui && (ui.layout === "page" ? ui.root.isConnected && ui.root.offsetParent !== null : winBox?.classList.contains("open"));

  // A page over everything (its own: the calibration), closed by its "close-page" message
  function openPage(src, onClose) {
    const f = el("iframe", "zm-pageframe");
    f.src = src; f.title = "ZyDeck";
    const gone = e => {
      if (e.source !== f.contentWindow) return;
      if (e.data?.zydek === "view-offset") { ctl()?.set("viewOffset", +e.data.ms || 0, false); return; }   // this screen's own
      if (e.data?.zydek === "close-page") { removeEventListener("message", gone); f.remove(); onClose?.(); }
    };
    addEventListener("message", gone);
    document.body.appendChild(f);
  }

  // ---- the custom rows ----
  let linkSt = () => "";
  const link = { scanning: false, found: [] };   // kept between windows
  function buildLink(c) {
    const tray = el("div", "zm-tray"), scan = procKey("Scan", "scan");
    c.appendChild(scan);
    linkSt = () => host.linked ? `Linked to ${host.hubHost.replace(/:\d+$/, "")}` : link.scanning ? (link.found.length ? `${link.found.length} found · still looking` : "Looking…")
      : pl.scan === "err" ? (bridge()?.linkScan ? "None found" : "") : link.found.length ? `${link.found.length} found` : "";
    scan.addEventListener("click", async () => {
      const b = bridge();
      if (!b || !b.linkScan) { host.toast("Only in the ZyDeck app", "bad"); setPl("scan", "err"); return; }
      if (link.scanning) return;
      link.scanning = true; link.found = []; pl.scan = "busy"; refresh();
      b.linkScan();
      try {
        for (let i = 0; i < 30; i++) {
          await sleep(500);
          let r; try { r = JSON.parse(b.linkDevices()); } catch { continue; }
          for (const d of r.devices) if (!link.found.some(f => f.host === d.host && f.port === d.port)) link.found.push(d);
          refresh();
          if (!r.scanning && i > 4) break;
        }
      } finally { link.scanning = false; setPl("scan", link.found.length ? "ok" : "err"); }
    });
    let sig = "";
    return () => {
      const row = scan.closest(".zm-row");
      if (row && !tray.isConnected) row.after(tray);
      lampOf(scan, pl.scan || "");
      const s2 = JSON.stringify([link.found, link.scanning, pl.scan, host.linked]);
      if (s2 !== sig) {
        sig = s2;
        const note = host.linked ? `This device is the other ZyDeck's controller now: ◀ Back to this device returns.`
          : link.scanning ? "Looking for ZyDecks on this Wi-Fi…" : pl.scan === "err" ? "None found: is the other ZyDeck open, on the same Wi-Fi or hotspot?"
          : "ZyDecks nearby show up here: Scan looks for them.";
        tray.replaceChildren(...(link.found.length ? link.found.map(d => {
          const r = el("div", "zm-dev", `<span class="who"><b>${esc(d.name)}</b><small>${esc(d.host)}</small></span>`);
          const go = procKey("Connect", "conn-" + d.host);
          go.addEventListener("click", async () => {
            if (pl["conn-" + d.host] === "busy") return;
            setPl("conn-" + d.host, "busy");
            const k = await linkKey(d);
            if (!k) { setPl("conn-" + d.host, "err"); return; }
            const page = isTablet() ? "/controller?" : "/controller?phone=1&";
            bridge().linkOpen(`http://127.0.0.1:8766${page}hub=${encodeURIComponent(`${d.host}:${d.port}`)}&k=${encodeURIComponent(k)}`);
          });
          lampOf(go, pl["conn-" + d.host] || "");
          r.appendChild(go);
          return r;
        }) : [el("div", "zm-trayempty", esc(note))]));
      }
    };
  }
  function buildTablet(c) {
    const show = key("Show code", "tog");
    show.innerHTML = '<i class="zl" aria-hidden="true"></i><span>Show code</span>';
    const box = el("div", "zm-qr");
    let open = false, url = store.get("tcUrl", null);
    show.addEventListener("click", () => { open = !open; refresh(); if (open) fill(); });
    c.appendChild(show);
    async function fill() {
      let urls = [];
      try { urls = await api("/lan-urls"); } catch {}
      if (!urls.length) { box.innerHTML = `<div class="zm-qrnote">Not on Wi-Fi: join one, or turn on the hotspot</div>`; return; }
      if (!urls.some(u => u.url === url)) url = urls[0].url;
      const nets = el("div", "zseg");
      if (urls.length > 1) urls.forEach(u => {
        const b = key(`<i class="zs" aria-hidden="true"></i>${esc(u.label)}`, "s" + (u.url === url ? " on" : ""));
        b.addEventListener("click", () => { url = u.url; store.set("tcUrl", url); fill(); });
        nets.appendChild(b);
      });
      const link = `${url}/controller`;
      box.replaceChildren(nets, el("div", "zm-qrcode", `<div class="zm-qrnote">…</div>`), el("div", "zm-qraddr", esc(link.replace(/^http:\/\//, ""))));
      for (let i = 0; i < 20; i++) {
        const r = await api(`/api/web/qr?${qs({ text: link })}`).catch(e => ({ error: e.message }));
        if (r.svg) { box.querySelector(".zm-qrcode").innerHTML = r.svg; return; }
        box.querySelector(".zm-qrcode").innerHTML = `<div class="zm-qrnote">${esc(r.error || "…")}</div>`;
        if (!r.starting) return;
        await sleep(1500);
      }
    }
    return () => {
      show.classList.toggle("on", open); show.querySelector("span").textContent = open ? "Hide code" : "Show code";
      const row = show.closest(".zm-row");
      if (open && row && !box.isConnected) row.after(box);
      if (!open) box.remove();
    };
  }
  function buildOut(c, which) {
    const dev = el("select", "zk pick wide"), ch = el("select", "zk pick ch");
    dev.setAttribute("aria-label", which === "main" ? "Main mix device" : "Headphones device");
    ch.setAttribute("aria-label", "Channels");
    const opt = list => list.map(([v, t]) => `<option value="${esc(v)}">${esc(t)}</option>`).join("");
    dev.addEventListener("change", () => {
      const d = (sys.audio?.devices || []).find(x => x.name === dev.value);
      const four = which === "phones" && d && d.channels >= 4 && d.name === outNow("main")[0];
      pend[which] = [dev.value, four ? "2,2" : "0,2"]; refresh(); applyOutputs();
    });
    ch.addEventListener("change", () => { pend[which] = [dev.value, ch.value]; refresh(); applyOutputs(); });
    c.append(dev, ch);
    let sig = "";
    return () => {
      const none = which === "main" ? "None: only the headphones play" : "None";
      const [d, cv] = outNow(which), s2 = JSON.stringify([sys.audio?.devices, d]);
      if (s2 !== sig) { sig = s2; dev.innerHTML = opt(devOpts(none)); ch.innerHTML = opt(chOpts(d)); }
      dev.value = d; ch.value = cv; ch.disabled = !d;
      if (ch.selectedIndex < 0) ch.selectedIndex = 0;
    };
  }
  function buildBili(c, site) {
    const inB = key("Log in"), outB = key("Log out");
    inB.addEventListener("click", () => host.openLogin?.(site));
    outB.addEventListener("click", () => { bridge()?.bilibiliLogout(site); });
    c.append(inB, outB);
    return () => {
      const b = bridge(), local = b?.bilibiliState ? JSON.parse(b.bilibiliState()) : null, has = local && local[site];
      inB.hidden = !!has; outB.hidden = !has;
      const st = c.querySelector(".zm-st"), a = sys.acc && sys.acc[site];
      st.textContent = has ? (a?.login ? `Logged in as ${a.name}${site === "cn" ? (a.premium ? ` · ${a.vip || "premium"}: Hi-Res` : " · no premium") : ""}` : "Logged in") : "Not logged in";
      st.hidden = false;
    };
  }
  function buildCookies(c) {
    const s = el("div", "zseg"), ta = el("textarea", "zf");
    let site = "cn";
    [["cn", "Bilibili"], ["intl", "International"]].forEach(([v, t]) => {
      const b = key(`<i class="zs" aria-hidden="true"></i>${t}`, "s"); b._v = v;
      b.addEventListener("click", () => { site = v; [...s.children].forEach(x => x.classList.toggle("on", x._v === site)); });
      s.appendChild(b);
    });
    s.firstChild.classList.add("on");
    ta.rows = 2; ta.placeholder = "SESSDATA=…; bili_jct=…"; ta.setAttribute("aria-label", "Cookies");
    const save = key("Save");
    save.addEventListener("click", () => {
      const b = bridge(); if (!b) return host.toast("Only in the ZyDeck app", "bad");
      const err = b.bilibiliPaste(site, ta.value);
      if (err) return host.toast(err, "bad");
      ta.value = ""; host.toast("Saved the Bilibili login", "ok"); loads.acc().then(refresh);
    });
    c.append(s, ta, save);
  }
  function buildTracklist(c) {
    const copy = procKey("Copy", "tl"), pre = el("pre", "zm-tl");
    copy.addEventListener("click", () => run("tl", async () => {
      await loads.tl();
      const tl = sys.tracklist;
      if (!tl?.text) throw new Error("Nothing played yet this session");
      try { await navigator.clipboard.writeText(tl.text); } catch { throw new Error("Couldn't copy: select the list instead"); }
      host.toast(`Tracklist copied (${tl.tracks.length} tracks)`, "ok");
    }));
    c.appendChild(copy);
    return () => {
      const row = copy.closest(".zm-row"), tl = sys.tracklist;
      lampOf(copy, pl.tl || "");
      pre.textContent = tl?.text || "";
      if (tl?.text && row && !pre.isConnected) row.after(pre);
      const st = c.querySelector(".zm-st"); st.textContent = tl ? (tl.tracks?.length ? `${tl.tracks.length} tracks` : "Nothing played yet") : ""; st.hidden = !st.textContent;
    };
  }
  function buildNewTrack(c) {
    const box = el("div", "zm-subs");
    const NAMES = ["Key lock", "Quantize", "Slip"];
    const segs = NAMES.map((n, i) => {
      const line = el("div", "zm-sr", `<span>${n}</span>`), s = el("div", "zseg");
      [[1, "On"], [0, "Off"], [-1, "As it was"]].forEach(([v, t]) => {
        const b = key(`<i class="zs" aria-hidden="true"></i>${t}`, "s"); b._v = v;
        b.addEventListener("click", () => { ctl()?.setDeckDefault(i, v); refresh(); });
        s.appendChild(b);
      });
      line.appendChild(s); box.appendChild(line); return s;
    });
    c.appendChild(box);
    return () => { const d = ctl()?.deckDefaults() || [-1, -1, -1]; segs.forEach((s, i) => [...s.children].forEach(b => b.classList.toggle("on", b._v === d[i]))); };
  }
  function buildKits(c) {
    const name = el("input", "zf"), save = procKey("Save the pads", "kitsave"), list = el("div", "zm-sub");
    name.placeholder = "Kit name"; name.setAttribute("aria-label", "Kit name"); name.autocomplete = "off";
    save.addEventListener("click", () => run("kitsave", async () => {
      const n = name.value.trim(); if (!n) throw new Error("Give the kit a name");
      const r = await api(`/api/kits/save?${qs({ name: n })}`); host.toast(`Saved ${n}: ${r.pads} pads`, "ok"); name.value = ""; await loads.kits();
    }));
    name.addEventListener("keydown", e => { if (e.key === "Enter") { e.preventDefault(); save.click(); } });
    c.append(name, save);
    return () => {
      const row = c.closest(".zm-row");
      lampOf(save, pl.kitsave || "");
      if (row && !list.isConnected) row.after(list);
      const kits = sys.kits || [];
      const st = c.querySelector(".zm-st"); st.textContent = sys.kits == null ? "" : kits.length ? "" : "No kits yet: fill some pads, then save them"; st.hidden = !st.textContent;
      list.replaceChildren(...kits.map(k => {
        const r = el("div", "zm-row sub", `<div class="zm-lbl"><b>${esc(k.name)}</b><small>${k.pads} pad${k.pads === 1 ? "" : "s"}${k.missing ? ` · ${k.missing} missing` : ""}</small></div><div class="zm-ctl"></div>`);
        const load = procKey("Load", "kit-" + k.name), del = key("Delete");
        load.addEventListener("click", () => run("kit-" + k.name, async () => { const x = await api(`/api/kits/load?${qs({ name: k.name })}`); host.toast(`Loaded ${k.name}: ${x.pads} pads${x.missing ? `, ${x.missing} missing` : ""}`, "ok"); }));
        del.addEventListener("click", async () => { if (!confirm(`Delete the kit "${k.name}"? (The sample files stay.)`)) return; try { await api(`/api/kits/delete?${qs({ name: k.name })}`); await loads.kits(); refresh(); } catch (e) { host.toast(e.message, "bad"); } });
        lampOf(load, pl["kit-" + k.name] || "");
        r.querySelector(".zm-ctl").append(load, del);
        return r;
      }));
    };
  }

  // ---- Link devices: another ZyDeck lets this one in only once someone there allowed it (src/zydek/zydekhub.cpp,
  // pairApi). Its key is kept here by its name; without one (or one it no longer takes) this asks, and waits up to a
  // minute while the other one shows the question. The key, or null
  const linkKeys = () => store.get("linkKeys", {}) || {};
  async function linkKey(d) {
    const base = `http://${d.host}:${d.port}/api/pair`, keys = linkKeys(), kept = keys[d.name];
    const get = async q => (await fetch(`${base}?${qs(q)}`)).json();
    try {
      if (kept && (await get({ k: kept })).state === "yes") return kept;
      const me = await api("/api/whoami").catch(() => ({}));
      const id = Array.from(crypto.getRandomValues(new Uint8Array(12)), b => b.toString(16).padStart(2, "0")).join("");
      host.toast(`Asked ${d.name}: allow this device there`);
      for (let i = 0; i < 60; i++) {
        const r = await get({ id, name: me.name || "A ZyDeck" });
        if (r.state === "yes" && r.key) { store.set("linkKeys", { ...linkKeys(), [d.name]: r.key }); return r.key; }
        if (r.state === "no") { host.toast(`${d.name} didn't allow it`, "bad"); return null; }
        if (r.state === "busy") { host.toast(`${d.name} is answering another device: try again in a moment`, "bad"); return null; }
        await sleep(1000);
      }
      host.toast(`${d.name} didn't answer`, "bad");
    } catch { host.toast(`Couldn't reach ${d.name}`, "bad"); }
    return null;
  }
  // and here: another ZyDeck asking (the hub's "pairask")
  async function pairAsked(m) {
    const yes = await ask(`Allow ${m.name} to control this ZyDeck?`, [
      `A ZyDeck called ${m.name} (${m.peer}) on this Wi-Fi asks to link. Allowed, it can see your library and load and play tracks here, as this device can.`,
      "Allow it only if it's yours. You can take it back in Settings › Allowed devices."], "Allow", "Don't allow");
    await api(`/api/pair?${qs({ answer: m.id, yes: yes ? 1 : 0 })}`).catch(e => host.toast(e.message, "bad"));
    if (yes) { host.toast(`${m.name} can control this ZyDeck now`, "ok"); pairedN = null; refresh(); }
  }
  let pairedN = null, pairedSt = () => pairedN == null ? "" : pairedN ? `${pairedN} allowed` : "None";
  function buildPaired(c) {
    const forget = key("Forget all");
    c.appendChild(forget);
    forget.addEventListener("click", async () => {
      if (!pairedN) return;
      if (!(await ask("Forget the allowed devices?", ["They can't control this ZyDeck any more; each one asks again next time."], "Forget"))) return;
      await api("/api/pair?forget=1").then(() => { pairedN = 0; host.toast("No other device can control this ZyDeck now", "ok"); refresh(); })
        .catch(e => host.toast(e.message, "bad"));
    });
    return () => {
      if (pairedN == null) { pairedN = 0; api("/api/pair?list=1").then(r => { pairedN = (r.devices || []).length; refresh(); }).catch(() => {}); }
      forget.disabled = !pairedN;
    };
  }

  // ---- pairing with a PC's servers: zydeck://pair?name=&stems=&st=&bot=&bk= (the QR ./launch.sh shows) ----
  // A question over everything, readable whatever the look (as the library's Remove): resolves true or false
  function ask(title, lines, yes, no = "Cancel") {
    return new Promise(done => {
      const box = el("div", "zm-ask", `<div class="box" role="dialog" aria-modal="true"><b>${esc(title)}</b>
        ${lines.map(l => `<p>${esc(l)}</p>`).join("")}<div class="acts"><button class="no">${esc(no)}</button><button class="yes">${esc(yes)}</button></div></div>`);
      const end = v => { box.remove(); done(v); };
      box.addEventListener("click", e => { if (e.target === box) end(false); });
      box.querySelector(".no").addEventListener("click", () => end(false));
      box.querySelector(".yes").addEventListener("click", () => end(true));
      document.body.appendChild(box);
    });
  }
  async function pair(url) {
    let q;
    try { q = new URL(url); } catch { return false; }
    if (q.protocol !== "zydeck:") return false;
    const kind = (q.host + q.pathname).replace(/^\/+/, "").split(/[/?]/)[0];
    q = q.searchParams;
    if (kind === "connect") return connectTo(q);
    if (kind !== "pair") return false;
    const stems = q.get("stems"), st = q.get("st"), bot = q.get("bot"), bk = q.get("bk"), name = q.get("name") || "the PC";
    if (!stems && !bot) { host.toast("That code has no servers in it", "bad"); return true; }
    const lines = [stems && `Stem server: ${stems}${st ? " and its token" : ""}`, bot && `Discord bot: ${bot}${bk ? " and its stream key" : ""}`].filter(Boolean);
    if (!(await ask(`Use ${name}'s servers?`, lines, "Use them"))) return true;
    await run("pcservers", async () => {
      if (stems) {
        await api(`/api/web/prefs?${qs({ stemServer: stems })}`);
        if (st) await api(`/api/web/prefs?${qs({ stemToken: st })}`);
        sys.prefs = { ...(sys.prefs || {}), stemServer: stems }; host.changed?.("stemServer", stems);
      }
      if (bot) sys.dc = await api(`/api/discord?${qs(bk ? { host: bot, key: bk } : { host: bot })}`);
      await loads.prefs();
      if (stems) checkStems();
      host.toast(`Set: ${stems && bot ? "the stem server and the Discord bot" : stems ? "the stem server" : "the Discord bot"}`, "ok");
    });
    refresh();
    return true;
  }

  // ZyDeck Connect on a computer: controller mode, linked over Wi-Fi (its code holds what the cable's hello gets)
  async function connectTo(q, asked = false) {   // (asked: the computer asked, having found this device on the network)
    const name = q.get("name") || "the computer", hosts = (q.get("hosts") || "").split(",").filter(Boolean), key = q.get("key");
    if (!hosts.length || !key) { host.toast("That code has no computer in it", "bad"); return true; }
    if (host.linked) { host.toast("This device is controlling another ZyDeck: go ◀ Back to this device, then scan again", "bad"); return true; }
    if (document.querySelector(".zm-ask")) return true;   // (one question at a time)
    if (!(await ask(asked ? `${name} wants to link` : `Control Mixxx on ${name}?`, [asked
      ? `ZyDeck Connect on ${name} (${hosts[0]}) found this device on the Wi-Fi. Link it, and ZyDeck goes to controller mode and controls Mixxx there.`
      : `ZyDeck goes to controller mode and links to ZyDeck Connect at ${hosts[0]} over Wi-Fi.`,
      "The music then plays on the computer."], "Link"))) return true;
    await run("ctlmode", async () => {
      const offer = JSON.stringify({ name, hosts, port: +q.get("port") || 8771, key });
      sys.remote = await api(`/api/controller?${qs({ on: 1, offer, link: 1 })}`);
      host.toast(`Linking to ${name}…`, "ok");
    });
    refresh();
    return true;
  }

  // ---- what the pages call ----
  /// The hub's pushes (/ws): remote, opt, rec, dc; and init (its remote and rec)
  function push(m) {
    if (m.t === "pairask" && m.id && window === top) { pairAsked(m); return; }   // another ZyDeck asks to link (Link devices)
    if (m.t === "offer" && m.o && window === top) {   // ZyDeck Connect on a computer found this device and asks to link
      const o = m.o, q = new URLSearchParams({ name: o.name || "", hosts: (o.hosts || []).join(","), port: o.port || 8771, key: o.key || "" });
      connectTo(q, true);
      return;
    }
    if (m.init) { if (m.init.remote) sys.remote = m.init.remote; if (m.init.rec) sys.rec = m.init.rec; sys.live = !!m.init.live; }
    else if (m.t === "remote") sys.remote = m;
    else if (m.t === "opt") { sys.opt = m.o; if (!m.o.running) loads.audio().then(refresh); }
    else if (m.t === "rec") sys.rec = m.r;
    else if (m.t === "dc") sys.dc = m.d;
    else return;
    refresh();
  }
  const providers = {};
  window.ZydeckMenu = {
    setup(h) {
      host = { ...host, ...h };
      // Pairing by QR (ZyDeck Connect's, or the one a PC's ./launch.sh shows): Android hands the code to the page
      // on top (the library, or the decks when linked to another ZyDeck), from Settings › Pair by QR or the camera
      // app; one that came before the page was ready waits for it there
      if (window !== top) return;
      window.zydekPair = url => { pair(url); return true; };
      window.zydekScanError = m => m === "getting" ? host.toast("Getting the scanner ready (only the first time)…")
        : host.toast(m === "not-zydeck" ? "That's not a ZyDeck code" : `Couldn't scan: ${m}`, "bad");
      const waiting = bridge()?.takePair?.();
      if (waiting) setTimeout(() => pair(waiting), 0);
    },
    provide(name, p) { providers[name] = p; refresh(); },
    providers, open, close, isOpen, refresh, push, pair, ask, rows: ROWS,
    reload: what => (loads[what] ? loads[what]() : loadAll()).then(refresh),
    get tab() { return curTab; },
    /// how many rows match q (for a page's own search)
    matches: q => { const w = q.toLowerCase().split(/\s+/).filter(Boolean); return w.length ? ROWS.filter(r => w.every(x => `${r.name} ${r.desc}`.toLowerCase().includes(x))).length : 0; },
  };
})();
