// ZyDeck Discord bot: plays ZyDeck's mix in a Discord voice channel.
//
// ZyDeck streams its mix with Mixxx's Live Broadcasting (Opus in Ogg, 48 kHz, 20 ms packets) to this bot as if
// it were an Icecast server; the bot hands the Opus packets to Discord as they are (no decoding, no second
// encode). Commands: /zydeck join (your voice channel), /zydeck leave, /zydeck status.
//
// Config: .env beside this file (see README.md): DISCORD_TOKEN, STREAM_KEY, PORT (8780).
import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { Readable } from "node:stream";
import { fileURLToPath } from "node:url";
import { ActivityType, ChannelType, Client, Events, GatewayIntentBits, MessageFlags, SlashCommandBuilder } from "discord.js";
import {
  AudioPlayerStatus, NoSubscriberBehavior, StreamType, VoiceConnectionStatus,
  createAudioPlayer, createAudioResource, entersState, getVoiceConnection, joinVoiceChannel,
} from "@discordjs/voice";

const here = path.dirname(fileURLToPath(import.meta.url));
for (const line of fs.existsSync(path.join(here, ".env")) ? fs.readFileSync(path.join(here, ".env"), "utf8").split("\n") : []) {
  const m = /^\s*([A-Z_]+)\s*=\s*(.*?)\s*$/.exec(line);
  if (m && process.env[m[1]] === undefined) process.env[m[1]] = m[2].replace(/^"(.*)"$/, "$1");
}
const TOKEN = process.env.DISCORD_TOKEN, KEY = process.env.STREAM_KEY, PORT = +process.env.PORT || 8780;
// TUNNEL=1: also reachable from anywhere through a Cloudflare quick tunnel (cloudflared, no account; a new
// https://….trycloudflare.com address each time it starts: /zydeck link tells it)
const TUNNEL = /^(1|on|yes|true)$/i.test(process.env.TUNNEL || "");
const CLOUDFLARED = process.env.CLOUDFLARED || path.join(process.env.HOME || "", ".local/bin/cloudflared");
const MOUNT = "/zydeck";
if (!KEY) { console.error("STREAM_KEY is missing from .env"); process.exit(1); }
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

// Where to play: kept, so the bot goes back there when ZyDeck goes live again (or the bot restarts)
const STATE = path.join(here, "state.json");
let state = {};
try { state = JSON.parse(fs.readFileSync(STATE, "utf8")); } catch {}
const saveState = () => fs.writeFileSync(STATE, JSON.stringify(state, null, 1));

// ---- Ogg Opus in, packets out ---------------------------------------------------------------------------
// Pages (RFC 3533) into packets: OpusHead, OpusTags, then one 20 ms Opus packet each. Raw pages are kept too,
// for /listen (the stream in a browser, for checking it without Discord).
class OggReader {
  constructor(onPacket, onPage) { this.buf = Buffer.alloc(0); this.partial = []; this.onPacket = onPacket; this.onPage = onPage; }
  push(chunk) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    for (;;) {
      if (this.buf.length < 27) return;
      if (this.buf.toString("latin1", 0, 4) !== "OggS") {   // lost sync: find the next page
        const i = this.buf.indexOf("OggS", 1, "latin1");
        this.buf = i < 0 ? this.buf.subarray(Math.max(0, this.buf.length - 3)) : this.buf.subarray(i);
        if (i < 0) return;
        continue;
      }
      const nseg = this.buf[26];
      if (this.buf.length < 27 + nseg) return;
      let size = 27 + nseg;
      for (let i = 0; i < nseg; i++) size += this.buf[27 + i];
      if (this.buf.length < size) return;
      const page = this.buf.subarray(0, size), type = page[5];
      if (!(type & 1)) this.partial = [];   // not continuing a packet from the last page
      let off = 27 + nseg, start = off;
      for (let i = 0; i < nseg; i++) {
        const l = page[27 + i];
        off += l;
        if (l < 255) {
          this.partial.push(page.subarray(start, off));
          this.onPacket(Buffer.concat(this.partial));
          this.partial = [];
          start = off;
        }
      }
      if (start < off) this.partial.push(page.subarray(start, off));
      this.onPage(Buffer.from(page), page.readBigInt64LE(6) === 0n);   // granule 0: a header page
      this.buf = this.buf.subarray(size);
    }
  }
}

// ---- the live stream ------------------------------------------------------------------------------------
// Mixxx sends a page every 100 ms or so (ZyDeck's encoder; a few packets each), Discord takes one packet every
// 20 ms, and Wi-Fi delivers in lumps now and then. The queue evens that out: it starts once START packets are
// in, plays silence if it runs dry (and starts again at START), and only if it hasn't once dropped to
// TARGET + SLACK in 3 s (the phone's clock running a touch faster than this computer's) does it skip ahead.
const SILENCE = Buffer.from([0xf8, 0xff, 0xfe]);
const START = 15, TARGET = 15, SLACK = 10;   // 0.3 s ahead; trimmed back to it past 0.5 s held for 3 s
const live = { source: null, since: 0, queue: [], primed: false, headers: [], listeners: new Set(), packets: 0, dropped: 0, dry: 0,
  lowest: Infinity, windowAt: 0 };
function nextPacket() {
  const q = live.queue, now = Date.now();
  if (!live.primed) {
    if (q.length < START) return SILENCE;
    live.primed = true; live.lowest = Infinity; live.windowAt = now;
  }
  live.lowest = Math.min(live.lowest, q.length);
  if (now - live.windowAt >= 3000) {
    if (live.lowest > TARGET + SLACK) { const n = live.lowest - TARGET; q.splice(0, n); live.dropped += n; }
    live.lowest = Infinity; live.windowAt = now;
  }
  const p = q.shift();
  if (!p) { live.primed = false; live.dry++; return SILENCE; }
  return p;
}
const player = createAudioPlayer({ behaviors: { noSubscriber: NoSubscriberBehavior.Play, maxMissedFrames: Infinity } });
player.on("error", e => log("player:", e.message));
function startPlaying() {
  const feed = new Readable({ objectMode: true, highWaterMark: 1, read() { this.push(live.source ? nextPacket() : null); } });
  player.play(createAudioResource(feed, { inputType: StreamType.Opus }));
}
function onLive(on) {
  if (on) {
    live.queue = []; live.primed = false;
    startPlaying();
    rejoin();
  } else {
    player.stop(true);
  }
  setPresence();
}

// ---- the stream in: from ZyDeck's relay (a WebSocket at /ingest, the stream key as a Bearer token: what comes
// through a Cloudflare tunnel) or straight from Mixxx's Live Broadcasting (an Icecast-style SOURCE, on the network)
function startSource(sock) {
  if (live.source) live.source.destroy();   // the newest source wins (ZyDeck reconnecting)
  live.source = sock; live.since = Date.now(); live.headers = []; live.packets = 0; live.dropped = 0; live.dry = 0;
  const reader = new OggReader(p => {
    if (p.length >= 8 && (p.toString("latin1", 0, 8) === "OpusHead" || p.toString("latin1", 0, 8) === "OpusTags")) return;
    live.packets++;
    live.queue.push(p);
    if (live.queue.length > 500) { live.dropped += live.queue.length - TARGET; live.queue.splice(0, live.queue.length - TARGET); }   // nobody's taking them
  }, (page, header) => {
    if (header) live.headers.push(page);
    for (const l of live.listeners) l.write(page);
  });
  log(`ZyDeck is live (${sock.remoteAddress === "::1" || sock.remoteAddress?.endsWith("127.0.0.1") ? "through the tunnel" : sock.remoteAddress})`);
  onLive(true);
  return reader;
}
// WebSocket frames from the relay (masked, RFC 6455): binary ones are the stream; pings answered
function wsReader(sock, onData) {
  let buf = Buffer.alloc(0), parts = [];
  return chunk => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    for (;;) {
      if (buf.length < 2) return;
      const fin = buf[0] & 0x80, op = buf[0] & 0x0f, masked = buf[1] & 0x80;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (len > 1 << 22) return sock.destroy();
      const need = off + (masked ? 4 : 0) + len;
      if (buf.length < need) return;
      let payload = buf.subarray(off + (masked ? 4 : 0), need);
      if (masked) { const m = buf.subarray(off, off + 4); payload = Buffer.from(payload); for (let i = 0; i < payload.length; i++) payload[i] ^= m[i & 3]; }
      buf = buf.subarray(need);
      if (op === 0x8) { sock.end(Buffer.from([0x88, 0])); return; }
      if (op === 0x9) { sock.write(Buffer.concat([Buffer.from([0x8a, payload.length]), payload.subarray(0, 125)])); continue; }
      if (op === 0x2 || op === 0x0) { parts.push(payload); if (fin) { onData(parts.length === 1 ? parts[0] : Buffer.concat(parts)); parts = []; } }
    }
  };
}
const bearerOk = req => {
  const m = /^Bearer\s+(.+)$/i.exec(req.headers.authorization || "");
  return (m && m[1].trim() === KEY) || new URL(req.target, "http://x").searchParams.get("key") === KEY;
};

// ---- Icecast-style source endpoint (what Mixxx's Live Broadcasting talks to) ---------------------------
// libshout: "SOURCE /zydeck HTTP/1.0" first without a password (told 401 and what's allowed), then again with
// Basic auth on the same connection or a new one; "200 OK", then the Ogg stream until it hangs up.
const authOk = h => {
  const m = /^Basic\s+(.+)$/i.exec(h.authorization || "");
  if (!m) return false;
  const [, pass] = Buffer.from(m[1], "base64").toString().split(/:(.*)/s);
  return pass === KEY;
};
function parseHead(text) {
  const [first, ...lines] = text.split(/\r?\n/);
  const [method, target] = first.split(" ");
  const headers = {};
  for (const l of lines) { const i = l.indexOf(":"); if (i > 0) headers[l.slice(0, i).trim().toLowerCase()] = l.slice(i + 1).trim(); }
  return { method, target: target || "/", headers };
}
const json = (sock, code, obj) => {
  const body = JSON.stringify(obj);
  sock.end(`HTTP/1.1 ${code} OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n` +
    `Content-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n${body}`);
};
net.createServer(sock => {
  let buf = Buffer.alloc(0), streaming = false, reader = null;
  sock.setNoDelay(true);
  sock.setTimeout(30000, () => sock.destroy());   // a source that stops sending
  sock.on("data", chunk => {
    if (streaming) return reader.push(chunk);
    buf = Buffer.concat([buf, chunk]);
    if (buf.length > 16384) return sock.destroy();
    for (;;) {
      const end = buf.indexOf("\r\n\r\n");
      if (end < 0) return;
      const req = parseHead(buf.toString("latin1", 0, end));
      buf = buf.subarray(end + 4);
      const { pathname } = new URL(req.target, "http://x");
      if (req.method === "GET" && pathname === "/status") return bearerOk(req) ? json(sock, 200, status()) : json(sock, 401, { error: "key" });
      if (req.method === "GET" && pathname === "/ingest" && /websocket/i.test(req.headers.upgrade || "")) {
        if (!bearerOk(req)) return sock.end("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        const accept = crypto.createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC11B9E").digest("base64");
        sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
        streaming = true;
        const ogg = startSource(sock);
        reader = { push: wsReader(sock, data => ogg.push(data)) };
        if (buf.length) reader.push(buf);
        return;
      }
      if (req.method === "GET" && pathname === "/listen") {   // the stream itself, for a browser or ffplay
        if (new URL(req.target, "http://x").searchParams.get("key") !== KEY) return json(sock, 401, { error: "key" });
        sock.write("HTTP/1.0 200 OK\r\nContent-Type: audio/ogg\r\nCache-Control: no-store\r\n\r\n");
        for (const p of live.headers) sock.write(p);
        live.listeners.add(sock);
        sock.setTimeout(0);
        sock.on("close", () => live.listeners.delete(sock));
        return;
      }
      if ((req.method === "SOURCE" || req.method === "PUT") && pathname === MOUNT) {
        if (!authOk(req.headers)) {
          sock.write("HTTP/1.0 401 Authentication Required\r\nWWW-Authenticate: Basic realm=\"ZyDeck\"\r\nAllow: SOURCE, PUT\r\nContent-Length: 0\r\n\r\n");
          continue;   // libshout asks again, now with the password
        }
        sock.write("HTTP/1.0 200 OK\r\n\r\n");
        streaming = true;
        reader = startSource(sock);
        if (buf.length) reader.push(buf);
        return;
      }
      return json(sock, 404, { error: "not here" });
    }
  });
  const gone = () => {
    if (live.source === sock) {
      live.source = null;
      log(`ZyDeck stopped (${live.packets} packets, ${live.dropped} skipped, ran dry ${live.dry}×)`);
      onLive(false);
    }
  };
  sock.on("close", gone);
  sock.on("error", () => {});
}).listen(PORT, () => log(`taking ZyDeck's stream on port ${PORT}`));

// ---- Cloudflare quick tunnel (TUNNEL=1): cloudflared, run and kept running by the bot, which reads the address
let tunnelUrl = null, tunnelProc = null, stopping = false;
function startTunnel() {
  if (!TUNNEL || stopping) return;
  if (!fs.existsSync(CLOUDFLARED)) { log(`TUNNEL=1 but there's no cloudflared at ${CLOUDFLARED}`); return; }
  tunnelUrl = null;
  tunnelProc = spawn(CLOUDFLARED, ["tunnel", "--no-autoupdate", "--url", `http://localhost:${PORT}`], { stdio: ["ignore", "pipe", "pipe"] });
  const scan = d => {
    const m = /https:\/\/[a-z0-9-]+\.trycloudflare\.com/.exec(d.toString());
    if (m && m[0] !== tunnelUrl) { tunnelUrl = m[0]; log(`tunnel: ${tunnelUrl}`); }
  };
  tunnelProc.stdout.on("data", scan);
  tunnelProc.stderr.on("data", scan);
  tunnelProc.on("exit", code => {
    tunnelUrl = null; tunnelProc = null;
    if (!stopping) { log(`tunnel stopped (${code}): starting it again in 10 s`); setTimeout(startTunnel, 10000); }
  });
}
startTunnel();
const lanAddress = () => Object.values(os.networkInterfaces()).flat()
  .find(a => a && a.family === "IPv4" && !a.internal && /^(192\.168|10\.|172\.(1[6-9]|2\d|3[01]))/.test(a.address))?.address;

// ---- Discord ----------------------------------------------------------------------------------------------
const client = new Client({ intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildVoiceStates] });
const command = new SlashCommandBuilder().setName("zydeck").setDescription("ZyDeck's mix in a voice channel")
  .addSubcommand(c => c.setName("join").setDescription("Play ZyDeck in your voice channel (or the one given)")
    .addChannelOption(o => o.setName("channel").setDescription("Voice channel").addChannelTypes(ChannelType.GuildVoice, ChannelType.GuildStageVoice)))
  .addSubcommand(c => c.setName("leave").setDescription("Leave the voice channel"))
  .addSubcommand(c => c.setName("status").setDescription("Is ZyDeck live, and who's listening"))
  .addSubcommand(c => c.setName("link").setDescription("The address ZyDeck streams to (from anywhere: the tunnel)"));

function voiceChannel() {
  const g = state.guildId && client.guilds.cache.get(state.guildId);
  return g && g.channels.cache.get(state.channelId);
}
async function join(channel) {
  const connection = joinVoiceChannel({ channelId: channel.id, guildId: channel.guild.id, adapterCreator: channel.guild.voiceAdapterCreator, selfDeaf: true });
  connection.subscribe(player);
  connection.on(VoiceConnectionStatus.Disconnected, async () => {
    try {   // moved to another channel, or a network blip: Discord reconnects on its own
      await Promise.race([entersState(connection, VoiceConnectionStatus.Signalling, 5000), entersState(connection, VoiceConnectionStatus.Connecting, 5000)]);
    } catch { if (connection.state.status !== VoiceConnectionStatus.Destroyed) connection.destroy(); }
  });
  await entersState(connection, VoiceConnectionStatus.Ready, 20000);
  if (channel.type === ChannelType.GuildStageVoice) await channel.guild.members.me.voice.setSuppressed(false).catch(() => {});
  return connection;
}
function rejoin() {   // not after being kicked: only /zydeck join brings it back then
  const ch = voiceChannel();
  if (ch && !state.kicked && !getVoiceConnection(ch.guild.id)) join(ch).catch(e => log("couldn't join:", e.message));
}
// Kicked (disconnected by someone, or the channel deleted): out of the channel without a new one, and not by
// /zydeck leave. Moved to another channel: it goes along, and that's the channel from then on.
let leavingAt = 0;
client.on(Events.VoiceStateUpdate, (before, after) => {
  if (after.id !== client.user?.id) return;
  if (before.channelId && !after.channelId) {
    if (Date.now() - leavingAt < 5000) return;
    getVoiceConnection(before.guild.id)?.destroy();
    state.kicked = { channel: before.channel?.name ?? null, server: before.guild.name, at: Date.now() };
    saveState();
    log(`kicked from ${state.kicked.channel} (${state.kicked.server}): staying out until /zydeck join`);
  } else if (after.channelId && after.channelId !== state.channelId && after.guild.id === state.guildId) {
    state.channelId = after.channelId;
    saveState();
    log(`moved to ${after.channel?.name}`);
  }
});
function listeners() {
  const ch = voiceChannel();
  return ch ? ch.members.filter(m => !m.user.bot).size : 0;
}
function status() {
  const ch = voiceChannel(), connection = ch && getVoiceConnection(ch.guild.id);
  return {
    bot: client.user?.tag ?? null, live: !!live.source, liveSeconds: live.source ? Math.round((Date.now() - live.since) / 1000) : 0,
    server: ch?.guild.name ?? null, channel: ch?.name ?? null, inChannel: connection?.state.status === VoiceConnectionStatus.Ready,
    listeners: listeners(), buffered: live.queue.length * 20, skipped: live.dropped, ranDry: live.dry,
    kicked: state.kicked ?? null, tunnel: tunnelUrl,
  };
}
function setPresence() {
  client.user?.setActivity(live.source ? "ZyDeck live" : "for ZyDeck", { type: live.source ? ActivityType.Listening : ActivityType.Watching });
}

client.once(Events.ClientReady, async () => {
  log(`signed in as ${client.user.tag}`);
  log(`invite: https://discord.com/oauth2/authorize?client_id=${client.user.id}&scope=bot%20applications.commands&permissions=3146752`);   // View Channels + Connect + Speak
  for (const g of client.guilds.cache.values()) await g.commands.set([command]).catch(e => log(`commands in ${g.name}:`, e.message));
  setPresence();
  if (live.source) rejoin();
});
client.on(Events.GuildCreate, g => g.commands.set([command]).catch(() => {}));
client.on(Events.InteractionCreate, async i => {
  if (!i.isChatInputCommand() || i.commandName !== "zydeck") return;
  const sub = i.options.getSubcommand();
  if (sub === "join") {
    const ch = i.options.getChannel("channel") ?? i.member?.voice?.channel;
    if (!ch) return i.reply({ content: "Join a voice channel first (or name one).", flags: MessageFlags.Ephemeral });
    await i.deferReply();
    try {
      await join(ch);
      state = { guildId: ch.guild.id, channelId: ch.id }; saveState();   // (and no longer kicked)
      await i.editReply(live.source ? `Playing ZyDeck in **${ch.name}**.` : `In **${ch.name}**: ZyDeck isn't live yet. It'll play as soon as it is.`);
    } catch (e) { await i.editReply(`Couldn't join **${ch.name}**: ${e.message}`); }
  } else if (sub === "link") {   // only to whoever asked
    const lan = lanAddress();
    await i.reply({ flags: MessageFlags.Ephemeral, content:
      (tunnelUrl ? `From anywhere: **${tunnelUrl}**` : TUNNEL ? "The tunnel is starting: try again in a few seconds" :
        "The tunnel is off (`./launch.sh tunnel on` on the computer)") +
      (lan ? `\nOn the home network: \`${lan}:${PORT}\`` : "") +
      "\nIn ZyDeck: Settings › Discord › Bot address (the stream key stays the same). The tunnel's address changes when it restarts." });
  } else if (sub === "leave") {
    leavingAt = Date.now();
    getVoiceConnection(i.guildId)?.destroy();
    if (state.guildId === i.guildId) { state = {}; saveState(); }
    await i.reply("Left. `/zydeck join` to come back.");
  } else {
    const s = status();
    await i.reply({ flags: MessageFlags.Ephemeral, content:
      `${s.live ? `🔴 ZyDeck live for ${Math.floor(s.liveSeconds / 60)} min` : "ZyDeck isn't streaming"} · ` +
      `${s.channel ? `${s.inChannel ? "in" : "set to"} **${s.channel}**, ${s.listeners} listening` : "no voice channel (`/zydeck join`)"}` +
      (s.kicked ? ` · kicked from **${s.kicked.channel}**: \`/zydeck join\` to bring it back` : "") });
  }
});
if (TOKEN) client.login(TOKEN).catch(e => { log("Discord sign-in failed:", e.message); });
else log("No DISCORD_TOKEN in .env yet: taking the stream (and /listen), not on Discord");

for (const sig of ["SIGINT", "SIGTERM"]) process.on(sig, () => { stopping = true; tunnelProc?.kill(); client.destroy(); process.exit(0); });
