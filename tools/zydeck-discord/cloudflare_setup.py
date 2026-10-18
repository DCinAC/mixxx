#!/usr/bin/env python3
"""ZyDeck on your own domain, through a named Cloudflare tunnel (run it on the Mac, in a terminal):

    python3 cloudflare_setup.py

It asks for your domain and a Cloudflare API token (typed hidden; it's used for this run only, kept nowhere),
then makes or updates:
  - a tunnel named "zydeck" (configured in Cloudflare) sending
        <bot>.<domain>   -> the Discord bot    on the PC, port 8780
        <stems>.<domain> -> the stem server    on the PC, port 8770
  - the two DNS records (CNAMEs to the tunnel, proxied)
  - cloudflared running it, as a TrueNAS app ("zydeck-tunnel") on the NAS
  - the bot's fixed address (launch.sh public), so /zydeck link gives it
  - optionally, other things on the network through the same tunnel (Stash: stash.<domain> -> 192.168.0.108:9999),
    each with Cloudflare Access in front if you like (only the emails you list get through, by a one-time code)
and checks the addresses answer.

The token needs (Cloudflare › My Profile › API Tokens › Create Token › Custom token):
    Account · Cloudflare Tunnel · Edit
    Zone    · DNS · Edit        (Zone Resources: your domain)
    Zone    · Zone · Read       (the same domain)
    Account · Access: Apps and Policies · Edit   (only for Cloudflare Access in front of an extra)
Give it a short TTL, and revoke it when this is done: the tunnel keeps running on its own tunnel token (in the
TrueNAS app), which this token isn't.
"""
import getpass
import json
import shlex
import subprocess
import sys
import time

NAS = "root@192.168.0.108"
PC = "192.168.0.100"
PC_SSH = "zydek-pc"
TUNNEL_NAME = "zydeck"
APP = "zydeck-tunnel"
CLOUDFLARED_IMAGE = "cloudflare/cloudflared:2026.9.3"
API = "https://api.cloudflare.com/client/v4"

TOKEN = ""


def say(text):
    print(f"\033[1;33m{text}\033[0m", flush=True)


def call(method, path, body=None):
    """The Cloudflare API through curl (the system's certificates); the token goes in on stdin, not the command line."""
    config = f'header = "Authorization: Bearer {TOKEN}"\nheader = "Content-Type: application/json"\n'
    args = ["curl", "-sS", "--config", "-", "-X", method, API + path]
    if body is not None:
        args += ["--data-binary", json.dumps(body)]
    out = subprocess.run(args, input=config, capture_output=True, text=True, timeout=60)
    try:
        d = json.loads(out.stdout)
    except ValueError:
        sys.exit(f"Couldn't reach Cloudflare: {out.stderr.strip() or out.stdout[:200]}")
    if not d.get("success"):
        errors = "; ".join(e.get("message", "") for e in d.get("errors", [])) or "no reason given"
        sys.exit(f"Cloudflare refused ({method} {path.split('?')[0]}): {errors}")
    return d["result"]


def ssh(host, command, stdin=None):
    out = subprocess.run(["ssh", "-o", "BatchMode=yes", host, command], input=stdin, capture_output=True,
                         text=True, timeout=300)
    if out.returncode != 0:
        sys.exit(f"{host}: {out.stderr.strip() or out.stdout.strip()}")
    return out.stdout


def http_code(url):
    out = subprocess.run(["curl", "-s", "-o", "/dev/null", "-m", "10", "-w", "%{http_code}", url],
                         capture_output=True, text=True)
    return out.stdout.strip()


def main():
    global TOKEN
    print("ZyDeck on your own domain (a named Cloudflare tunnel)\n")
    domain = input("Your domain (e.g. example.com): ").strip().lower().rstrip(".")
    bot = (input("Name for the Discord bot [zydeck]: ").strip().lower() or "zydeck") + "." + domain
    stems = (input("Name for the stem server [stems]: ").strip().lower() or "stems") + "." + domain
    extras = []   # (hostname, service, protect)
    print("\nAnything else on the network through this tunnel? name=host:port, e.g. stash=192.168.0.108:9999 (empty: no)")
    for item in [x.strip() for x in input("Extras (comma-separated): ").split(",") if x.strip()]:
        name, _, target = item.partition("=")
        if not name or ":" not in target:
            sys.exit(f"{item}: write it as name=host:port")
        extras.append((f"{name.strip().lower()}.{domain}", f"http://{target.strip()}", None))
    emails = []
    if extras:
        print("Cloudflare Access in front of those: only these emails get through (a one-time code by email first)")
        emails = [e.strip() for e in input("Emails (comma-separated; empty: no Access, their own login only): ").split(",") if e.strip()]
    TOKEN = getpass.getpass("Cloudflare API token (hidden): ").strip()
    if not TOKEN:
        sys.exit("No token")

    zones = call("GET", f"/zones?name={domain}")
    if not zones:
        sys.exit(f"The token can't see {domain}: give it Zone · Zone · Read and Zone · DNS · Edit for it")
    zone, account = zones[0]["id"], zones[0]["account"]["id"]
    say(f"{domain}: found")

    tunnels = call("GET", f"/accounts/{account}/cfd_tunnel?name={TUNNEL_NAME}&is_deleted=false")
    if tunnels:
        tunnel = tunnels[0]["id"]
        say(f"Tunnel {TUNNEL_NAME}: already there, updating it")
    else:
        tunnel = call("POST", f"/accounts/{account}/cfd_tunnel", {"name": TUNNEL_NAME, "config_src": "cloudflare"})["id"]
        say(f"Tunnel {TUNNEL_NAME}: made")
    call("PUT", f"/accounts/{account}/cfd_tunnel/{tunnel}/configurations", {"config": {"ingress": [
        {"hostname": bot, "service": f"http://{PC}:8780"},
        {"hostname": stems, "service": f"http://{PC}:8770"},
        *({"hostname": host, "service": service} for host, service, _ in extras),
        {"service": "http_status:404"},
    ]}})
    say(f"Routes: {bot} -> the bot, {stems} -> the stem server" +
        "".join(f", {host} -> {service[7:]}" for host, service, _ in extras))

    for host in (bot, stems, *(h for h, _, _ in extras)):
        record = {"type": "CNAME", "name": host, "content": f"{tunnel}.cfargotunnel.com", "proxied": True,
                  "comment": "ZyDeck (cloudflare_setup.py)"}
        existing = call("GET", f"/zones/{zone}/dns_records?name={host}")
        if existing and existing[0]["type"] != "CNAME":
            sys.exit(f"{host} already has a {existing[0]['type']} record: not touching it (remove it, or pick another name)")
        if existing:
            call("PUT", f"/zones/{zone}/dns_records/{existing[0]['id']}", record)
        else:
            call("POST", f"/zones/{zone}/dns_records", record)
    say("DNS: the names point at the tunnel")

    if extras and emails:   # Cloudflare Access: a login (email + one-time code) before the extras
        policy = call("POST", f"/accounts/{account}/access/policies", {
            "name": "ZyDeck extras: these emails", "decision": "allow",
            "include": [{"email": {"email": e}} for e in emails]})["id"]
        apps = call("GET", f"/accounts/{account}/access/apps")
        for host, _, _ in extras:
            body = {"name": host, "domain": host, "type": "self_hosted", "session_duration": "720h",
                    "policies": [{"id": policy, "precedence": 1}]}
            same = next((a for a in apps if a.get("domain") == host), None)
            if same:
                call("PUT", f"/accounts/{account}/access/apps/{same['id']}", body)
            else:
                call("POST", f"/accounts/{account}/access/apps", body)
        say("Access: " + ", ".join(h for h, _, _ in extras) + " only for " + ", ".join(emails) + " (a code by email, then 30 days)")

    tunnel_token = call("GET", f"/accounts/{account}/cfd_tunnel/{tunnel}/token")
    compose = (
        "services:\n"
        "  cloudflared:\n"
        f"    image: {CLOUDFLARED_IMAGE}\n"
        "    command: tunnel --no-autoupdate run\n"
        "    environment:\n"
        f"      TUNNEL_TOKEN: {json.dumps(tunnel_token)}\n"
        "    restart: unless-stopped\n"
    )
    exists = json.loads(ssh(NAS, f"midclt call app.query {shlex.quote(json.dumps([['name', '=', APP]]))}") or "[]")
    if exists:   # the compose (with the tunnel token) goes in on stdin, not the command line
        ssh(NAS, f'midclt call -j app.update {APP} "$(cat)"', json.dumps({"custom_compose_config_string": compose}))
    else:
        ssh(NAS, 'midclt call -j app.create "$(cat)"',
            json.dumps({"app_name": APP, "custom_app": True, "custom_compose_config_string": compose}))
    tunnel_token = None
    say(f"TrueNAS: the app {APP} runs cloudflared")

    for _ in range(30):
        if call("GET", f"/accounts/{account}/cfd_tunnel/{tunnel}").get("status") == "healthy":
            break
        time.sleep(3)
    else:
        sys.exit("The tunnel didn't come up in 90 s: look at the zydeck-tunnel app's logs in TrueNAS")
    say("Tunnel: healthy")

    ssh(PC_SSH, f"~/Documents/Zydek/launch.sh public https://{bot}")
    say(f"Bot: its address from anywhere is https://{bot} (the quick tunnel is off)")

    time.sleep(5)
    b, s = http_code(f"https://{bot}/status"), http_code(f"https://{stems}/health")
    print(f"\nhttps://{bot}/status    {b} (401 = it's there, and wants the stream key)")
    print(f"https://{stems}/health  {s} (401 = it's there, and wants the stem token)")
    for host, _, _ in extras:
        print(f"https://{host}/  {http_code(f'https://{host}/')} ({'302 = Cloudflare Access asks who you are' if emails else '200/302 = its own page or login'})")
    say("\nDone. Revoke the API token now (My Profile › API Tokens): the tunnel doesn't need it.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("\nStopped")
