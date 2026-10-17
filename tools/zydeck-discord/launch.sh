#!/usr/bin/env bash
# The ZyDeck Discord bot on this PC (install.sh puts this in ~/Documents/Zydek; the bot runs as the user
# service zydeck-discord and starts with the computer):
#   ./launch.sh                 start it (asks for the bot's token the first time)
#   ./launch.sh token           set or replace the bot's token (typed hidden, saved in discord-bot/.env)
#   ./launch.sh stop | restart | status
#   ./launch.sh logs            follow its log (Ctrl+C stops following, not the bot)
#   ./launch.sh invite          the link that adds the bot to a Discord server
#   ./launch.sh phone <IP>      give ZyDeck on the phone this bot's address and stream key
#   ./launch.sh key             show the stream key (to type into ZyDeck by hand)
set -euo pipefail
ZYDEK="$(cd "$(dirname "$0")" && pwd)"
BOT="$ZYDEK/discord-bot"
ENV="$BOT/.env"
SVC=zydeck-discord
PERMISSIONS=3146752   # View Channels 1024 + Connect 1048576 + Speak 2097152
say() { printf '\033[1;33m%s\033[0m\n' "$*"; }
[ -f "$ENV" ] || { say "The bot isn't installed: bash $ZYDEK/mixxx/tools/zydeck-discord/install.sh"; exit 1; }

get() { sed -n "s/^$1=//p" "$ENV" | head -1; }
put() {   # put NAME VALUE: replaces that line of .env (kept private)
  local tmp
  tmp=$(mktemp "$BOT/.env.XXXXXX")
  grep -v "^$1=" "$ENV" > "$tmp" || true
  printf '%s=%s\n' "$1" "$2" >> "$tmp"
  chmod 600 "$tmp"
  mv "$tmp" "$ENV"
}
ask_token() {
  say "Paste the bot's token (Discord Developer Portal › your application › Bot › Reset Token). It won't show:"
  local t
  read -rs t
  echo
  t="${t//[[:space:]]/}"
  [[ "$t" =~ ^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$ ]] || { say "That doesn't look like a bot token (three parts with dots)"; exit 1; }
  put DISCORD_TOKEN "$t"
  say "Saved in $ENV"
}
app_id() {   # a bot token's first part is its ID, in base64
  local p
  p=$(get DISCORD_TOKEN | cut -d. -f1)
  [ -n "$p" ] || return 1
  while [ $(( ${#p} % 4 )) -ne 0 ]; do p="$p="; done
  printf '%s' "$p" | tr '_-' '/+' | base64 -d 2>/dev/null
}
invite() {
  local id
  id=$(app_id) || { say "No token yet: ./launch.sh token"; return 1; }
  echo "https://discord.com/oauth2/authorize?client_id=$id&scope=bot%20applications.commands&permissions=$PERMISSIONS"
}
my_ip() { hostname -I | awk '{print $1}'; }
port() { local p; p=$(get PORT); echo "${p:-8780}"; }
recent_log() { sleep 3; journalctl --user -u $SVC -n "${1:-5}" --no-pager -o cat; }

case "${1:-start}" in
  start)
    [ -n "$(get DISCORD_TOKEN)" ] || ask_token
    systemctl --user enable $SVC >/dev/null 2>&1
    systemctl --user restart $SVC
    recent_log 4
    say "Add it to a Discord server: $(invite)"
    say "Phone: ZyDeck › Settings › Discord: address $(my_ip):$(port) and the stream key (./launch.sh key), or ./launch.sh phone <phone IP>"
    ;;
  token) ask_token; systemctl --user restart $SVC; recent_log 3 ;;
  stop) systemctl --user stop $SVC; say "Stopped (it starts again with the computer, or ./launch.sh)" ;;
  restart) systemctl --user restart $SVC; recent_log 3 ;;
  status)
    systemctl --user --no-pager status $SVC 2>/dev/null | sed -n '1,3p' || true
    curl -s -m 3 "localhost:$(port)/status" && echo || say "Not answering on port $(port)"
    ;;
  logs) exec journalctl --user -u $SVC -f -o cat ;;
  invite) invite ;;
  key) get STREAM_KEY ;;
  phone)
    [ -n "${2:-}" ] || { say "./launch.sh phone <the phone's IP, e.g. 192.168.0.123>"; exit 1; }
    if curl -fsS -m 5 -G "http://$2:8766/api/discord" --data-urlencode "host=$(my_ip):$(port)" \
        --data-urlencode "key=$(get STREAM_KEY)" >/dev/null; then
      say "ZyDeck on $2 streams to $(my_ip):$(port) now: Settings › Discord › Go live"
    else
      say "Couldn't reach ZyDeck at $2 (open it on the phone, on the same Wi-Fi)"
      exit 1
    fi
    ;;
  *) sed -n '2,11p' "$0"; exit 1 ;;
esac
