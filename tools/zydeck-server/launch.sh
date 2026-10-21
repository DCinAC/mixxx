#!/usr/bin/env bash
# ZyDeck's servers on this PC, both user services that start with the computer and restart if they crash:
# the stem server (zydeck-stems, GPU stems, port 8770) and the Discord bot (zydeck-discord, port 8780).
#   ./launch.sh                 start both (or restart them), check they answer, and show the pairing QR
#   ./launch.sh start | stop | restart | status [stems|bot]      both, or the one named
#   ./launch.sh logs [stems|bot]                                 follow one's log (Ctrl+C stops following, not it)
#   ./launch.sh qr [anywhere]   the pairing QR again: ZyDeck › Settings › Connections › Pair by QR
#                               sets the stem server and the bot (their addresses, token and stream key) in one
#                               go. "anywhere": their addresses from outside the home network (set below)
# The stem server:
#   ./launch.sh stems public <URL> | off   its address from anywhere (a Cloudflare tunnel to port 8770), for the QR
# The bot:
#   ./launch.sh token           set or replace the bot's token (typed hidden, saved in discord-bot/.env)
#   ./launch.sh invite          the link that adds the bot to a Discord server
#   ./launch.sh key             show the stream key (to type into ZyDeck by hand)
#   ./launch.sh tunnel on|off   the bot reachable from anywhere through a Cloudflare quick tunnel (or not)
#   ./launch.sh link            the bot's address from anywhere now (/zydeck link too)
#   ./launch.sh public <URL>    a fixed address in front of the bot (a named Cloudflare tunnel on your domain);
#                               the quick tunnel goes off. ./launch.sh public off: back to none
#   ./launch.sh phone <IP>      give ZyDeck on the phone the bot's address and stream key over the network
#                               (add "tunnel" for the tunnel's address); the QR does this too
# The QR holds the stem token and the stream key: show it only to your own phone.
set -uo pipefail
ZYDEK="$(cd "$(dirname "$0")" && pwd)"
BOT="$ZYDEK/discord-bot"
ENV="$BOT/.env"
STEMS="$ZYDEK/stemserver"
PERMISSIONS=3146752   # View Channels 1024 + Connect 1048576 + Speak 2097152
say() { printf '\033[1;33m%s\033[0m\n' "$*"; }
has_bot() { [ -f "$ENV" ]; }
has_stems() { [ -f "$STEMS/run.sh" ]; }
my_ip() { hostname -I | awk '{print $1}'; }

# ---- the bot's .env -------------------------------------------------------------------------------------
get() { sed -n "s/^$1=//p" "$ENV" 2>/dev/null | head -1; }
put() {   # put NAME VALUE: replaces that line of .env (kept private)
  local tmp
  tmp=$(mktemp "$BOT/.env.XXXXXX")
  grep -v "^$1=" "$ENV" > "$tmp" || true
  printf '%s=%s\n' "$1" "$2" >> "$tmp"
  chmod 600 "$tmp"
  mv "$tmp" "$ENV"
}
bot_port() { local p; p=$(get PORT); echo "${p:-8780}"; }
bot_status() { curl -s -m 3 -H "Authorization: Bearer $(get STREAM_KEY)" "localhost:$(bot_port)/status"; }
tunnel_url() { bot_status | sed -n 's/.*"tunnel":"\([^"]*\)".*/\1/p'; }
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

# ---- the services ---------------------------------------------------------------------------------------
stems_unit() {   # the stem server's user service (it used to be started by hand with stemserver/run.sh)
  local unit=~/.config/systemd/user/zydeck-stems.service
  [ -f "$unit" ] && return
  mkdir -p ~/.config/systemd/user
  cat > "$unit" <<UNITEOF
[Unit]
Description=ZyDeck stem server (GPU stems, port 8770)
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=$STEMS
ExecStart=/usr/bin/env bash $STEMS/run.sh
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
UNITEOF
  systemctl --user daemon-reload
  echo "stems: made the zydeck-stems service"
}
answers() {   # answers URL TRIES: 200, or 401 (it's there, it wants its key)
  local code i
  for i in $(seq 1 "$2"); do
    code=$(curl -s -o /dev/null -m 2 -w '%{http_code}' "$1")
    [ "$code" = 200 ] || [ "$code" = 401 ] && return 0
    sleep 2
  done
  return 1
}
check_stems() {
  if answers http://127.0.0.1:8770/health 90; then
    echo "stems: up (the phone uses $(my_ip):8770)"
  else
    echo "stems: not answering on port 8770; the last lines of its log:"; journalctl --user -u zydeck-stems -n 20 --no-pager; return 1
  fi
}
check_bot() {
  if answers "http://127.0.0.1:$(bot_port)/status" 10; then
    echo "bot: up (port $(bot_port))"
  else
    echo "bot: not answering on port $(bot_port); the last lines of its log:"; journalctl --user -u zydeck-discord -n 15 --no-pager; return 1
  fi
  local pub
  pub=$(get PUBLIC_URL)
  if [ -n "$pub" ]; then
    answers "$pub/status" 3 && echo "bot: up from outside too ($pub)" || echo "bot: $pub doesn't answer: check the zydeck-tunnel app on the NAS"
  fi
}
which_ones() {   # the services a command is for: the one named, or every one installed
  case "${1:-}" in
    stems) has_stems && echo stems || say "The stem server isn't installed here ($STEMS)" >&2 ;;
    bot) has_bot && echo bot || say "The bot isn't installed: bash $ZYDEK/mixxx/tools/zydeck-discord/install.sh" >&2 ;;
    "") has_stems && echo stems; has_bot && echo bot ;;
    *) say "stems or bot, not $1" >&2; exit 1 ;;
  esac
}
svc() { [ "$1" = stems ] && echo zydeck-stems || echo zydeck-discord; }
start() {
  local s ok=0
  for s in $(which_ones "${1:-}"); do
    if [ "$s" = stems ]; then
      stems_unit
      pkill -f "uvicorn server:app --host 0.0.0.0 --port 8770" 2>/dev/null   # one started by hand the old way
    else
      [ -n "$(get DISCORD_TOKEN)" ] || ask_token
    fi
    systemctl --user enable "$(svc "$s")" >/dev/null 2>&1
    systemctl --user restart "$(svc "$s")" && echo "$s: (re)started"
  done
  for s in $(which_ones "${1:-}"); do check_"$s" || ok=1; done
  return $ok
}

# ---- pairing --------------------------------------------------------------------------------------------
# zydeck://pair?name=<this PC>&stems=<address>&st=<stem token>&bot=<address>&bk=<stream key>: what ZyDeck's
# Settings › Connections › Pair by QR sets (it asks first). Built and drawn in Python with the
# secrets passed in its environment, never on a command line.
pair_qr() {
  local where="${1:-home}" stems="" bot=""
  if has_stems; then
    if [ "$where" = anywhere ]; then stems=$(cat "$STEMS/public_url" 2>/dev/null); else stems="$(my_ip):8770"; fi
  fi
  if has_bot; then
    if [ "$where" = anywhere ]; then bot=$(get PUBLIC_URL); [ -n "$bot" ] || bot=$(tunnel_url); else bot="$(my_ip):$(bot_port)"; fi
  fi
  if [ -z "$stems$bot" ]; then
    [ "$where" = anywhere ] && say "No address from anywhere yet: ./launch.sh stems public <URL>, ./launch.sh tunnel on or public <URL>" \
                            || say "Neither the stem server nor the bot is installed here"
    return 1
  fi
  PAIR_NAME="$(hostname -s)" PAIR_STEMS="$stems" PAIR_BOT="$bot" \
  PAIR_ST="$( [ -n "$stems" ] && tr -d '[:space:]' < "$STEMS/token" 2>/dev/null)" \
  PAIR_BK="$( [ -n "$bot" ] && get STREAM_KEY)" \
  python3 -c '
import os, urllib.parse as u
q = {"name": os.environ["PAIR_NAME"]}
for k, e in (("stems", "PAIR_STEMS"), ("st", "PAIR_ST"), ("bot", "PAIR_BOT"), ("bk", "PAIR_BK")):
    if os.environ.get(e): q[k] = os.environ[e]
print("zydeck://pair?" + u.urlencode(q))' | python3 "$ZYDEK/qr.py" || return 1
  [ -n "$stems" ] && echo "  stem server  $stems (and its token)"
  [ -n "$bot" ] && echo "  Discord bot  $bot (and its stream key)"
  say "Scan it in ZyDeck: Settings › Connections › Pair by QR (or with the phone's camera)."
  say "It holds the stem token and the stream key: show it only to your own phone."
}

case "${1:-start}" in
  start|restart) start "${2:-}"; r=$?; [ -n "${2:-}" ] || pair_qr; exit $r ;;
  stop) for s in $(which_ones "${2:-}"); do systemctl --user stop "$(svc "$s")" && echo "$s: stopped (it starts again with the computer, or ./launch.sh)"; done ;;
  status)
    for s in $(which_ones "${2:-}"); do
      systemctl --user --no-pager status "$(svc "$s")" 2>/dev/null | sed -n '1,3p'
      check_"$s"
    done ;;
  logs)
    s=${2:-bot}; which_ones "$s" >/dev/null || exit 1
    exec journalctl --user -u "$(svc "$s")" -f -o cat ;;
  qr) pair_qr "${2:-home}" ;;
  stems)
    case "${2:-}" in
      public)
        [ -n "${3:-}" ] || { say "./launch.sh stems public https://stems.example.com  (or: off)"; exit 1; }
        if [ "$3" = off ]; then rm -f "$STEMS/public_url"; say "No address from anywhere for the stem server";
        else printf '%s\n' "${3%/}" > "$STEMS/public_url"; say "From anywhere: ${3%/} (./launch.sh qr anywhere)"; fi ;;
      start|stop|restart|status|logs) exec "$0" "$2" stems ;;
      *) say "./launch.sh stems public <URL> | off"; exit 1 ;;
    esac ;;
  bot)   # ./launch.sh bot start, etc.
    [ -n "${2:-}" ] || set -- bot start
    exec "$0" "$2" bot ;;
  token) has_bot || { which_ones bot; exit 1; }; ask_token; systemctl --user restart zydeck-discord; sleep 3; journalctl --user -u zydeck-discord -n 3 --no-pager -o cat ;;
  invite) has_bot || { which_ones bot; exit 1; }; invite ;;
  key) has_bot || { which_ones bot; exit 1; }; get STREAM_KEY ;;
  tunnel)
    has_bot || { which_ones bot; exit 1; }
    case "${2:-}" in
      on) [ -x "$HOME/.local/bin/cloudflared" ] || { say "cloudflared isn't installed: bash $ZYDEK/mixxx/tools/zydeck-discord/install.sh"; exit 1; }
          put TUNNEL 1 ;;
      off) put TUNNEL 0 ;;
      *) say "./launch.sh tunnel on | off"; exit 1 ;;
    esac
    systemctl --user restart zydeck-discord
    if [ "$2" = on ]; then
      u=""
      for _ in $(seq 1 20); do sleep 1; u=$(tunnel_url); [ -n "$u" ] && break; done
      [ -n "$u" ] && say "From anywhere: $u" || say "The tunnel is starting: ./launch.sh link in a moment"
    else say "Tunnel off: only on the home network now"; fi
    ;;
  public)
    has_bot || { which_ones bot; exit 1; }
    [ -n "${2:-}" ] || { say "./launch.sh public https://zydeck.example.com  (or: off)"; exit 1; }
    if [ "$2" = off ]; then put PUBLIC_URL ""; else put PUBLIC_URL "${2%/}"; put TUNNEL 0; fi
    systemctl --user restart zydeck-discord; sleep 3
    say "From anywhere: $(tunnel_url || true)"
    ;;
  link)
    has_bot || { which_ones bot; exit 1; }
    u=$(tunnel_url)
    [ -n "$u" ] && echo "$u" || { say "No tunnel address (./launch.sh tunnel on, or it's starting)"; exit 1; }
    ;;
  phone)
    has_bot || { which_ones bot; exit 1; }
    [ -n "${2:-}" ] || { say "./launch.sh phone <the phone's IP, e.g. 192.168.0.123>"; exit 1; }
    addr="$(my_ip):$(bot_port)"
    if [ "${3:-}" = tunnel ]; then addr=$(tunnel_url); [ -n "$addr" ] || { say "No tunnel address yet (./launch.sh tunnel on)"; exit 1; }; fi
    if curl -fsS -m 5 -G "http://$2:8766/api/discord" --data-urlencode "host=$addr" \
        --data-urlencode "key=$(get STREAM_KEY)" >/dev/null; then
      say "ZyDeck on $2 streams to $addr now: Settings › Discord › Go live"
    else
      say "Couldn't reach ZyDeck at $2 (open it on the phone, on the same Wi-Fi)"
      exit 1
    fi
    ;;
  *) sed -n '2,24p' "$0"; exit 1 ;;
esac
