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
   It makes `~/Documents/Zydek/discord-bot/.env` with a new stream key. Put the token in it:
   `DISCORD_TOKEN=...`, then `systemctl --user restart zydeck-discord`.
3. **Invite it to your server**: the log prints an invite link (`journalctl --user -u zydeck-discord`), asking
   for Connect and Speak.
4. **On the phone**: ZyDeck › Discord: the bot's address (this computer's IP, port 8780) and the stream key
   from `.env`.

## Use

- In Discord, join a voice channel and type `/zydeck join`. The bot remembers the channel and goes back to it
  whenever ZyDeck goes live.
- In ZyDeck, **Go live**. `/zydeck status` shows who's listening; `/zydeck leave` sends the bot away.
- `http://<computer>:8780/status` is the state as JSON; `http://<computer>:8780/listen?key=<stream key>` is the
  stream itself (for checking it in a browser).

## On a cloud server instead

The same bot runs on any small Linux server: install it there, open port 8780 (TCP), and give ZyDeck the
server's address. ZyDeck uploads about 60 MB an hour, from anywhere with internet.
