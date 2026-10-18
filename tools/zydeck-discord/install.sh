#!/usr/bin/env bash
# Installs (or updates) the ZyDeck Discord bot on this Linux computer, as a user service that starts with it.
#   bash tools/zydeck-discord/install.sh
# Node.js goes to ~/.local/node, the bot to ~/Documents/Zydek/discord-bot (its .env and state.json are kept).
set -euo pipefail
NODE_VERSION=v24.21.0
SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="$HOME/Documents/Zydek/discord-bot"

if [ ! -x "$HOME/.local/node/bin/node" ] || [ "$("$HOME/.local/node/bin/node" --version)" != "$NODE_VERSION" ]; then
  echo "Installing Node.js $NODE_VERSION in ~/.local/node"
  tmp=$(mktemp -d)
  curl -fsSL "https://nodejs.org/dist/$NODE_VERSION/node-$NODE_VERSION-linux-x64.tar.xz" -o "$tmp/node.tar.xz"
  curl -fsSL "https://nodejs.org/dist/$NODE_VERSION/SHASUMS256.txt" -o "$tmp/SHASUMS256.txt"
  (cd "$tmp" && grep " node-$NODE_VERSION-linux-x64.tar.xz\$" SHASUMS256.txt | sed "s/node-$NODE_VERSION-linux-x64.tar.xz/node.tar.xz/" | sha256sum -c -)
  rm -rf "$HOME/.local/node" && mkdir -p "$HOME/.local/node"
  tar -xJf "$tmp/node.tar.xz" -C "$HOME/.local/node" --strip-components=1
  rm -rf "$tmp"
fi
export PATH="$HOME/.local/node/bin:$PATH"

CLOUDFLARED_VERSION=2026.9.3   # for ./launch.sh tunnel on (a Cloudflare quick tunnel, no account)
if [ ! -x "$HOME/.local/bin/cloudflared" ] || ! "$HOME/.local/bin/cloudflared" --version | grep -q "$CLOUDFLARED_VERSION"; then
  echo "Installing cloudflared $CLOUDFLARED_VERSION in ~/.local/bin"
  mkdir -p "$HOME/.local/bin"
  curl -fsSL "https://github.com/cloudflare/cloudflared/releases/download/$CLOUDFLARED_VERSION/cloudflared-linux-amd64" -o "$HOME/.local/bin/cloudflared.new"
  chmod +x "$HOME/.local/bin/cloudflared.new" && mv "$HOME/.local/bin/cloudflared.new" "$HOME/.local/bin/cloudflared"
fi

mkdir -p "$DEST"
cp "$SRC/bot.mjs" "$SRC/package.json" "$SRC/README.md" "$DEST/"
if [ ! -f "$DEST/.env" ]; then
  key=$(head -c 18 /dev/urandom | base64 | tr '+/' '-_')
  printf 'DISCORD_TOKEN=\nSTREAM_KEY=%s\nPORT=8780\n' "$key" > "$DEST/.env"
  chmod 600 "$DEST/.env"
  echo "Made $DEST/.env with a new stream key; put the bot's token in it (DISCORD_TOKEN=...)"
fi
(cd "$DEST" && npm install --omit=dev --no-audit --no-fund)
cp "$SRC/launch.sh" "$HOME/Documents/Zydek/launch.sh" && chmod +x "$HOME/Documents/Zydek/launch.sh"

mkdir -p "$HOME/.config/systemd/user"
cp "$SRC/zydeck-discord.service" "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable zydeck-discord >/dev/null
systemctl --user restart zydeck-discord
echo "Running. ~/Documents/Zydek/launch.sh: token, status, logs, invite, phone <IP> (see its top)"
