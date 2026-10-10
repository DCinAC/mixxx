// Tablet Pads — Mixxx mapping for the browser pad screens served by `python -m mixxx_yt.pads`.
// Deck pad modes mirror the Hercules DJControl Inpulse 300 MK2 4-deck mapping, so the tablet and the
// controller behave the same.
//
// Input (tablet -> Mixxx), note on = press, note on vel 0 / note off = release:
//   ch 1, notes 0..63      sampler pads -> [Sampler1]..[Sampler64]
//       tap loaded pad = play from cue (retrigger) · hold = stop · tap empty pad = load selected track
//   ch 2..5 = decks 1..4, note = mode*8 + pad (pads 1-8 = top row 1-4, bottom row 5-8)
//       mode 0 HOT CUE     hotcue 1-8 activate (held = preview)
//       mode 1 CUE SHIFT   clear hotcue 1-8
//       mode 2 ROLL        beatlooproll 1/8 1/4 1/2 1 2 4 8 16 (held)
//       mode 3 STEMS       pads 1-4 mute/unmute stem 4,3,2,1 (Vocals, Other, Bass, Drums)
//       mode 4 BEATJUMP    -1 +1 -2 +2 / -4 +4 -8 +8
//       mode 5 FX          unit 1 (decks 1/3) or 2 (decks 2/4): pads 1-3 effect on/off,
//                          pad 4 route unit to this deck, pads 5-7 next effect
//       mode 6 FX SHIFT    same with unit 3 or 4
//       mode 7 KEY         pad 1 = pitch -1 semitone, pad 2 = +1, pad 3 = reset key (top-bar buttons)
//   (The Inpulse's SAMPLER pad mode is sent as plain sampler notes on ch 1: decks 1/3 -> 1-8, 2/4 -> 9-16.)
//
// Output (Mixxx -> tablet):
//   ch 1 note i, vel 0/1/2   sampler i empty / loaded / playing
//   ch 3 note deck*8+pad     roll pad active
//   ch 4 note deck           deck empty / loaded / playing
//   ch 5 note deck*8+pad     stem pad (0-3) unmuted
//   ch 6 note deck           stem count of the loaded track
//   ch 7 note unit*8+j       FX unit 1-4: j 0-2 effect enabled, j 3-6 routed to deck 1-4
//   ch 8 note deck           key shift in semitones + 64
//   SysEx F0 7D <type> <index> <sub> <v 4x7-bit, LSB first> F7
//       type 0x4D sampler length ms · 0x4E deck length ms (index = deck)
//       type 0x4F hot cue: index = deck, sub = pad, v = colour + 1 (0 = no cue)
//   Scripts can't read titles, so the pad server looks lengths up in the Mixxx library to show names.
//
// Controller page (SysEx in both directions, handled by incomingData):
//   F0 7D 50 mode <"group,key"> 00 <v7> F7    set a control (mode 0 = value, 1 = parameter 0..1)
//   F0 7D 51 kind <"group,key"> F7            subscribe (kind 0 = value on change, 1 = value polled ~30/s,
//                                             2 = parameter on change); Mixxx answers with 58 messages
//   F0 7D 52 deck op <t3> F7                  scratch: op 1 = grab (scratchEnable), 2 = ticks, 0 = release
//   F0 7D 53 deck overlimit <v7> F7           tempo in % (widens rateRange past the base range if allowed)
//   F0 7D 54 deck mode F7                     sync: 0 off (tempo kept), 1 on (tempo + beat), 3 on as the master
//   F0 7D 5E <seq 3x7 bits> F7                latency ping: sent straight back, unchanged
//   out: F0 7D 58 <"group,key"> 00 <v7> F7    control value · F0 7D 5F F7 = "mapping (re)started"
//   v7 = round(value * 1e5) + 2^48 as 7 x 7 bits, LSB first (sample positions fit); t3 = signed ticks + 2^20 as 3 x 7 bits.

var TabletPads = {};

TabletPads.NUM = 64;
TabletPads.DECKS = 4;
TabletPads.HOLD_MS = 450;
TabletPads.ROLL_SIZES = ["0.125", "0.25", "0.5", "1", "2", "4", "8", "16"];
TabletPads.JUMPS = [["1", "_backward"], ["1", "_forward"], ["2", "_backward"], ["2", "_forward"],
    ["4", "_backward"], ["4", "_forward"], ["8", "_backward"], ["8", "_forward"]];
TabletPads.STEM_FOR_PAD = [4, 3, 2, 1];
TabletPads._conns = [];
TabletPads._holdTimers = {};

TabletPads.sampler = function (i) {
    return "[Sampler" + (i + 1) + "]";
};

TabletPads.deck = function (d) {
    return "[Channel" + (d + 1) + "]";
};

TabletPads.stem = function (d, pad) {
    return "[Channel" + (d + 1) + "_Stem" + TabletPads.STEM_FOR_PAD[pad] + "]";
};

// Inpulse layout: the left deck (1/3) drives FX unit 1, the right deck (2/4) unit 2; SHIFT uses 3/4.
TabletPads.fxUnit = function (d, shift) {
    return (d % 2) + 1 + (shift ? 2 : 0);
};

TabletPads.playState = function (g) {
    if (!engine.getValue(g, "track_loaded")) {
        return 0;
    }
    return engine.getValue(g, "play") ? 2 : 1;
};

TabletPads.sysex = function (type, index, sub, v) {
    midi.sendSysexMsg([0xF0, 0x7D, type, index, sub,
        v & 0x7F, (v >> 7) & 0x7F, (v >> 14) & 0x7F, (v >> 21) & 0x7F, 0xF7], 10);
};

TabletPads.lengthMs = function (g) {
    return engine.getValue(g, "track_loaded") ? Math.round(engine.getValue(g, "duration") * 1000) : 0;
};

// ---- feedback senders ----

TabletPads.sendSampler = function (i) {
    midi.sendShortMsg(0x90, i, TabletPads.playState(TabletPads.sampler(i)));
};

TabletPads.sendSamplerInfo = function (i) {
    TabletPads.sysex(0x4D, i, 0, TabletPads.lengthMs(TabletPads.sampler(i)));
};

TabletPads.sendDeck = function (d) {
    midi.sendShortMsg(0x93, d, TabletPads.playState(TabletPads.deck(d)));
};

TabletPads.sendDeckInfo = function (d) {
    TabletPads.sysex(0x4E, d, 0, TabletPads.lengthMs(TabletPads.deck(d)));
};

TabletPads.sendCue = function (d, k) {
    var g = TabletPads.deck(d);
    var set = engine.getValue(g, "hotcue_" + (k + 1) + "_status") > 0;
    var color = engine.getValue(g, "hotcue_" + (k + 1) + "_color");
    TabletPads.sysex(0x4F, d, k, set ? (Math.max(0, color) & 0xFFFFFF) + 1 : 0);
};

TabletPads.sendRoll = function (d, k) {
    var on = engine.getValue(TabletPads.deck(d), "beatlooproll_" + TabletPads.ROLL_SIZES[k] + "_activate") > 0;
    midi.sendShortMsg(0x92, d * 8 + k, on ? 1 : 0);
};

TabletPads.sendStem = function (d, pad) {
    midi.sendShortMsg(0x94, d * 8 + pad, engine.getValue(TabletPads.stem(d, pad), "mute") > 0 ? 0 : 1);
};

TabletPads.sendStemCount = function (d) {
    midi.sendShortMsg(0x95, d, engine.getValue(TabletPads.deck(d), "stem_count"));
};

TabletPads.sendFx = function (u, j) {
    var unit = "[EffectRack1_EffectUnit" + (u + 1) + "]";
    var on = j < 3
        ? engine.getValue("[EffectRack1_EffectUnit" + (u + 1) + "_Effect" + (j + 1) + "]", "enabled")
        : engine.getValue(unit, "group_" + TabletPads.deck(j - 3) + "_enable");
    midi.sendShortMsg(0x96, u * 8 + j, on > 0 ? 1 : 0);
};

TabletPads.sendPitch = function (d) {
    var v = Math.round(engine.getValue(TabletPads.deck(d), "pitch")) + 64;
    midi.sendShortMsg(0x97, d, Math.max(0, Math.min(127, v)));
};

TabletPads.connect = function (group, key, fn) {
    var c = engine.makeConnection(group, key, fn);
    if (c) {
        TabletPads._conns.push(c);
    }
    fn();
};

// =====================================================================================================
// Controller page: generic control access over SysEx
// =====================================================================================================
TabletPads.TICKS_PER_REV = 3600;         // platter resolution: one revolution (1.8 s at 33 1/3) = 3600 ticks
TabletPads._subs = {};
TabletPads._poll = [];
TabletPads._baseRange = {};

// v7: 7 bytes, so sample positions (tens of millions) fit; 2^48 = 281474976710656, exact in JavaScript.
TabletPads.encodeValue = function (v) {
    var n = Math.round(v * 1e5) + 281474976710656, out = [];
    for (var i = 0; i < 7; i++) { out.push(n % 128); n = Math.floor(n / 128); }
    return out;
};
TabletPads.decodeValue = function (b, at) {
    var n = 0;
    for (var i = 6; i >= 0; i--) { n = n * 128 + b[at + i]; }
    return (n - 281474976710656) / 1e5;
};
TabletPads.readName = function (b, at) {      // ASCII "group,key" up to a 0x00 terminator
    var str = "";
    while (at < b.length && b[at] !== 0 && b[at] !== 0xF7) { str += String.fromCharCode(b[at]); at++; }
    var comma = str.indexOf(",");
    return { group: str.substring(0, comma), key: str.substring(comma + 1), name: str, end: at };
};
TabletPads.sendValue = function (name, v) {
    var msg = [0xF0, 0x7D, 0x58];
    for (var i = 0; i < name.length; i++) { msg.push(name.charCodeAt(i) & 0x7F); }
    msg.push(0);
    msg = msg.concat(TabletPads.encodeValue(v));
    msg.push(0xF7);
    midi.sendSysexMsg(msg, msg.length);
};

TabletPads.subscribe = function (group, key, kind) {
    var name = group + "," + key, id = kind + ":" + name;   // value and parameter subscriptions are separate
    if (TabletPads._subs[id]) {                 // already watched: just send the current value again
        TabletPads._subs[id].send();
        return;
    }
    var send = function () {
        TabletPads.sendValue(name, kind === 2 ? engine.getParameter(group, key) : engine.getValue(group, key));
    };
    var sub = { send: send, last: null };
    if (kind === 1) {
        sub.group = group; sub.key = key; sub.name = name;
        TabletPads._poll.push(sub);
    } else {
        sub.conn = engine.makeConnection(group, key, send);
    }
    TabletPads._subs[id] = sub;
    send();
};

TabletPads.pollTick = function () {
    for (var i = 0; i < TabletPads._poll.length; i++) {
        var p = TabletPads._poll[i], v = engine.getValue(p.group, p.key);
        if (v !== p.last) { p.last = v; TabletPads.sendValue(p.name, v); }
    }
};

TabletPads.setTempo = function (d, overlimit, percent) {
    var g = TabletPads.deck(d);
    if (TabletPads._baseRange[d] === undefined) { TabletPads._baseRange[d] = engine.getValue(g, "rateRange"); }
    var base = TabletPads._baseRange[d], t = percent / 100, range = base;
    if (overlimit) {
        // Keep the range just wider than the tempo, so Mixxx's own slider stays readable; up to 300 % (Mixxx
        // allows 400 %), and never slower than 1 % of normal speed.
        range = Math.min(3, Math.max(base, Math.ceil(Math.abs(t) * 100 + 1) / 100));
    }
    t = Math.max(Math.max(-range, -0.99), Math.min(range, t));
    if (Math.abs(engine.getValue(g, "rateRange") - range) > 1e-9) {
        engine.setValue(g, "rateRange", range);       // Mixxx keeps the actual tempo when the range changes
    }
    engine.setValue(g, "rate", t / (range * engine.getValue(g, "rate_dir")));
};

// Pioneer-style sync. mode 0: off (the tempo stays where sync put it) · 1: on, tempo and beats locked to the
// master · 3: on, and this deck becomes the master (others follow its tempo). Mixxx makes it its "soft"
// leader: like a CDJ's master, it passes to another synced deck if it stops.
TabletPads.setSync = function (d, mode) {
    var g = TabletPads.deck(d);
    if (mode === 0) {
        engine.setValue(g, "sync_enabled", 0);
        return;
    }
    if (mode === 3) {
        // Straight to leader: turning plain sync on first would pull this deck to another deck's tempo.
        engine.setValue(g, "sync_leader", 1);
        return;
    }
    engine.setValue(g, "sync_enabled", 1);
    engine.setValue(g, "beatsync_phase", 1);
};

TabletPads.incomingData = function (data, length) {
    var b = new Uint8Array(data);
    if (b.length < 4 || b[0] !== 0xF0 || b[1] !== 0x7D) { return; }
    var type = b[2], nm;
    if (type === 0x50) {
        nm = TabletPads.readName(b, 4);
        var v = TabletPads.decodeValue(b, nm.end + 1);
        if (b[3] === 1) { engine.setParameter(nm.group, nm.key, v); } else { engine.setValue(nm.group, nm.key, v); }
    } else if (type === 0x51) {
        nm = TabletPads.readName(b, 4);
        TabletPads.subscribe(nm.group, nm.key, b[3]);
    } else if (type === 0x52) {
        var deck = b[3] + 1, op = b[4];
        if (op === 1) {
            engine.scratchEnable(deck, TabletPads.TICKS_PER_REV, 33 + 1 / 3, 1 / 8, 1 / 8 / 32, false);
        } else if (op === 2) {
            engine.scratchTick(deck, (b[5] + b[6] * 128 + b[7] * 16384) - 1048576);
        } else {
            engine.scratchDisable(deck, false);
        }
    } else if (type === 0x53) {
        TabletPads.setTempo(b[3], b[4] === 1, TabletPads.decodeValue(b, 5));
    } else if (type === 0x54) {
        TabletPads.setSync(b[3], b[4]);
    } else if (type === 0x5E && b.length === 7) {
        midi.sendSysexMsg([0xF0, 0x7D, 0x5E, b[3], b[4], b[5], 0xF7], 7);
    }
};

TabletPads.init = function () {
    // Both are created synchronously when raised, and never shrink an existing count.
    if (engine.getValue("[App]", "num_samplers") < TabletPads.NUM) {
        engine.setValue("[App]", "num_samplers", TabletPads.NUM);
    }
    if (engine.getValue("[App]", "num_decks") < TabletPads.DECKS) {
        engine.setValue("[App]", "num_decks", TabletPads.DECKS);
    }

    for (var i = 0; i < TabletPads.NUM; i++) {
        (function (idx) {
            var g = TabletPads.sampler(idx);
            var state = function () { TabletPads.sendSampler(idx); };
            var info = function () { TabletPads.sendSamplerInfo(idx); };
            TabletPads.connect(g, "track_loaded", state);
            TabletPads.connect(g, "play", state);
            TabletPads.connect(g, "track_loaded", info);
            TabletPads.connect(g, "duration", info);
        })(i);
    }

    for (var d = 0; d < TabletPads.DECKS; d++) {
        (function (deck) {
            var g = TabletPads.deck(deck);
            var state = function () { TabletPads.sendDeck(deck); };
            var info = function () { TabletPads.sendDeckInfo(deck); };
            TabletPads.connect(g, "track_loaded", state);
            TabletPads.connect(g, "play", state);
            TabletPads.connect(g, "track_loaded", info);
            TabletPads.connect(g, "duration", info);
            TabletPads.connect(g, "stem_count", function () { TabletPads.sendStemCount(deck); });
            TabletPads.connect(g, "pitch", function () { TabletPads.sendPitch(deck); });
            for (var k = 0; k < 8; k++) {
                (function (pad) {
                    var cue = function () { TabletPads.sendCue(deck, pad); };
                    TabletPads.connect(g, "hotcue_" + (pad + 1) + "_status", cue);
                    TabletPads.connect(g, "hotcue_" + (pad + 1) + "_color", cue);
                    TabletPads.connect(g, "beatlooproll_" + TabletPads.ROLL_SIZES[pad] + "_activate",
                        function () { TabletPads.sendRoll(deck, pad); });
                    if (pad < 4) {
                        TabletPads.connect(TabletPads.stem(deck, pad), "mute",
                            function () { TabletPads.sendStem(deck, pad); });
                    }
                })(k);
            }
        })(d);
    }

    // Controller page: poll fast-changing values (play position) ~30 times a second, and tell the pad server
    // the mapping (re)started so it re-sends the page's subscriptions.
    TabletPads._pollTimer = engine.beginTimer(33, TabletPads.pollTick);
    midi.sendSysexMsg([0xF0, 0x7D, 0x5F, 0xF7], 4);

    for (var u = 0; u < 4; u++) {
        for (var j = 0; j < 7; j++) {
            (function (unit, slot) {
                var fn = function () { TabletPads.sendFx(unit, slot); };
                if (slot < 3) {
                    TabletPads.connect("[EffectRack1_EffectUnit" + (unit + 1) + "_Effect" + (slot + 1) + "]", "enabled", fn);
                } else {
                    TabletPads.connect("[EffectRack1_EffectUnit" + (unit + 1) + "]",
                        "group_" + TabletPads.deck(slot - 3) + "_enable", fn);
                }
            })(u, j);
        }
    }
};

TabletPads.shutdown = function () {
    if (TabletPads._pollTimer) { engine.stopTimer(TabletPads._pollTimer); }
    for (var name in TabletPads._subs) {
        if (TabletPads._subs[name].conn) { TabletPads._subs[name].conn.disconnect(); }
    }
    for (var i = 0; i < TabletPads._conns.length; i++) {
        TabletPads._conns[i].disconnect();
    }
    TabletPads._conns = [];
    for (var n = 0; n < TabletPads.NUM; n++) {
        midi.sendShortMsg(0x90, n, 0);
    }
    for (var d = 0; d < TabletPads.DECKS; d++) {
        midi.sendShortMsg(0x93, d, 0);
    }
};

// ---- input handlers ----

TabletPads.pad = function (channel, control, value, status, group) {
    var i = control;
    if (i < 0 || i >= TabletPads.NUM) {
        return;
    }
    var g = TabletPads.sampler(i);
    var pressed = (status & 0xF0) === 0x90 && value > 0;

    if (TabletPads._holdTimers[i]) {
        engine.stopTimer(TabletPads._holdTimers[i]);
        delete TabletPads._holdTimers[i];
    }
    if (!pressed) {
        return;
    }
    if (!engine.getValue(g, "track_loaded")) {
        engine.setValue(g, "LoadSelectedTrack", 1);
        return;
    }
    engine.setValue(g, "cue_gotoandplay", 1);
    TabletPads._holdTimers[i] = engine.beginTimer(TabletPads.HOLD_MS, function () {
        delete TabletPads._holdTimers[i];
        engine.setValue(g, "cue_gotoandstop", 1);
    }, true);
};

TabletPads.deckPad = function (channel, control, value, status, group) {
    var d = (status & 0x0F) - 1;
    var mode = control >> 3;
    var k = control & 7;
    if (d < 0 || d >= TabletPads.DECKS) {
        return;
    }
    var g = TabletPads.deck(d);
    var pressed = (status & 0xF0) === 0x90 && value > 0;

    // Held modes: press and release both matter.
    if (mode === 0) {
        engine.setValue(g, "hotcue_" + (k + 1) + "_activate", pressed ? 1 : 0);
        return;
    }
    if (mode === 2) {
        engine.setValue(g, "beatlooproll_" + TabletPads.ROLL_SIZES[k] + "_activate", pressed ? 1 : 0);
        return;
    }
    if (!pressed) {
        return;
    }
    if (mode === 1) {
        engine.setValue(g, "hotcue_" + (k + 1) + "_clear", 1);
    } else if (mode === 3) {
        if (k < 4) {
            var stem = TabletPads.stem(d, k);
            if (engine.getValue(stem, "mute")) {
                engine.setValue(stem, "mute", 0);
                engine.setValue(stem, "volume", 1.0);
            } else {
                engine.setValue(stem, "mute", 1);
            }
        }
    } else if (mode === 4) {
        engine.setValue(g, "beatjump_" + TabletPads.JUMPS[k][0] + TabletPads.JUMPS[k][1], 1);
    } else if (mode === 5 || mode === 6) {
        var u = TabletPads.fxUnit(d, mode === 6);
        var unit = "[EffectRack1_EffectUnit" + u + "]";
        if (k < 3) {
            script.toggleControl("[EffectRack1_EffectUnit" + u + "_Effect" + (k + 1) + "]", "enabled");
        } else if (k === 3) {
            script.toggleControl(unit, "group_" + g + "_enable");
        } else if (k < 7) {
            engine.setValue("[EffectRack1_EffectUnit" + u + "_Effect" + (k - 3) + "]", "next_effect", 1);
        }
    } else if (mode === 7) {
        engine.setValue(g, ["pitch_down", "pitch_up", "reset_key"][k] || "", 1);
    }
};
