# ZyDeck Discord bot

Plays ZyDeck's mix in a Discord voice channel. ZyDeck sends its mix (what's recorded: Settings › Advanced ›
Recording point applies) with Mixxx's Live Broadcasting, Opus at 128 kbps in 20 ms packets, and the bot hands
those packets to Discord unchanged. Listeners hear it about a second behind you.

## Set up (once)

1. **The bot's Discord account** (you do this; the token is a password, keep it to yourself):
   - <https://discord.com/developers/applications> › **New Application** › name it (e.g. "ZyDeck").
   - **Bot** › **Reset Token** › copy it.
   - No privileged intents are needed.
2. **Install it on the computer** (Linux, runs as a user service and starts with the computer):
   ```bash
   bash tools/zydeck-discord/install.sh
   ```
   It makes `~/Documents/Zydek/discord-bot/.env` with a new stream key, and `~/Documents/Zydek/launch.sh`.
   Then `~/Documents/Zydek/launch.sh` asks for the token (typed hidden) and starts the bot.
3. **Invite it to your server**: `launch.sh invite` prints the link (permissions 3146752: View Channels,
   Connect, Speak).
4. **On the phone**: `launch.sh` shows a QR code: ZyDeck › Settings › Connections › PC servers › Scan QR sets
   the bot's address and stream key (and the stem server's, when it's on this PC too). Or `launch.sh phone <phone IP>`
   fills them in over the network, or type them in Settings › Discord (`launch.sh key` shows the key).
   `launch.sh` now runs both servers: see `tools/zydeck-server/launch.sh`.

## Use

- In Discord, join a voice channel and type `/zydeck join`. The bot remembers the channel and goes back to it
  whenever ZyDeck goes live.
- In ZyDeck, **Go live**. `/zydeck status` shows who's listening; `/zydeck leave` sends the bot away.
- `http://<computer>:8780/status` is the state as JSON; `http://<computer>:8780/listen?key=<stream key>` is the
  stream itself (for checking it in a browser).

## From anywhere: a Cloudflare tunnel

`launch.sh tunnel on` runs a Cloudflare quick tunnel (cloudflared, no account): the bot is then also at an
`https://….trycloudflare.com` address. `/zydeck link` in Discord (or `launch.sh link`) tells it; put it in ZyDeck ›
Settings › Discord › Bot address (or `launch.sh phone <phone IP> tunnel` while the phone is home). ZyDeck sends the
stream through it over an encrypted WebSocket. A quick tunnel's address changes each time it starts (the
computer restarting, say), so look it up again then.

## On a cloud server instead

The same bot runs on any small Linux server: install it there, open port 8780 (TCP), and give ZyDeck the
server's address. ZyDeck uploads about 60 MB an hour, from anywhere with internet.
