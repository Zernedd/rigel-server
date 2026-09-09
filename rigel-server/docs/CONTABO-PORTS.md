# Opening the ports clients need — Contabo

There are **two** firewalls in front of a Contabo VPS. Opening only one is the usual
reason "the port is open but nobody can connect".

1. **Contabo's external firewall** — in the customer panel, *outside* your VPS.
2. **The VPS's own firewall** — `ufw` inside Ubuntu (`install.sh` handles this).

Both must allow the traffic.

---

## What actually needs to be open

| Port | Proto | Who connects | Must be public? |
|------|-------|--------------|-----------------|
| `7777-7787` | **UDP** | **Quest clients → game servers** | **YES — the game itself** |
| `443` | TCP | clients → HTTPS services (with a domain) | Yes, if using `DOMAIN` |
| `80` | TCP | Let's Encrypt HTTP-01 challenge | Yes, if using `DOMAIN` |
| `22` | TCP | you, over SSH | Yes (restrict to your IP if you can) |
| `90` | TCP | Mothership (login/auth) | Only if **no** domain/TLS |
| `78` | TCP | StationDb (stations, roles, **quests/deployments**) | Only if **no** domain/TLS |
| `50` | TCP | EOS gateway (server list) | Only if **no** domain/TLS |
| `80` | TCP | EOS websocket | Only if **no** domain/TLS |
| `8080` | TCP | Web dashboard | Prefer SSH tunnel instead |
| `9095`, `9100` | TCP | Orchestrator control + agent | **NO — keep private** |

**The one people miss: `7777-7787/UDP`.** The game traffic is UDP, not TCP. If only
TCP is open, players authenticate fine and then fail to connect to any station.

> Your box currently has `90, 78, 50, 80, 8080, 9095, 9100` open to the internet.
> That works, but `9095`/`9100` (orchestrator) being public is a real risk — anything
> that can reach them can ask your fleet to spin up servers. Close them, or restrict
> them to your own IP.

---

## 1. Contabo panel (external firewall)

1. Log in at <https://my.contabo.com>.
2. **Your Services → VPS →** pick the VPS → **Firewall** (some accounts show
   *Networking → Firewalling*).
3. If no firewall is attached, traffic is unfiltered and you can skip to `ufw`.
   If one is attached, **Edit rules** and add **inbound** rules:

| Action | Protocol | Port range | Source |
|--------|----------|-----------|--------|
| Allow | UDP | `7777-7787` | `0.0.0.0/0` |
| Allow | TCP | `443` | `0.0.0.0/0` |
| Allow | TCP | `80` | `0.0.0.0/0` |
| Allow | TCP | `22` | *your IP* (or `0.0.0.0/0`) |

Without a domain, also allow TCP `90`, `78`, `50`, `8080`.

4. **Save / apply.** Changes can take a minute.

> Contabo firewall rules are stateful for the ports you list. You do **not** need
> matching outbound rules.

---

## 2. Inside the VPS (`ufw`)

`install.sh` does this for you, but to do it by hand:

```bash
sudo ufw allow 22/tcp                 # do this FIRST or you lock yourself out
sudo ufw allow 7777:7787/udp          # THE GAME TRAFFIC
sudo ufw allow 443/tcp                # with a domain
sudo ufw allow 80/tcp

# only if you are NOT using a domain/TLS:
sudo ufw allow 90/tcp && sudo ufw allow 78/tcp && sudo ufw allow 50/tcp && sudo ufw allow 8080/tcp

sudo ufw enable
sudo ufw status numbered
```

Keep the orchestrator private:

```bash
sudo ufw deny 9095/tcp
sudo ufw deny 9100/tcp
# or allow just yourself:
# sudo ufw allow from <your.ip> to any port 9095 proto tcp
```

Reach the dashboard without exposing 8080 — tunnel it over SSH:

```bash
ssh -L 8080:127.0.0.1:8080 root@YOUR-VPS-IP
# then browse http://localhost:8080
```

---

## 3. Verify

From the VPS (is the service actually listening?):

```bash
sudo ss -lntup | grep -E ':(90|78|50|8080)\b'
sudo ss -lnup  | grep -E ':(777[0-9]|778[0-7])\b'
```

From your PC (does it get through both firewalls?):

```powershell
Test-NetConnection YOUR-VPS-IP -Port 443
```

`Test-NetConnection` only tests **TCP**. For the UDP game port, the real test is a
client connecting — or from another Linux box:

```bash
sudo nmap -sU -p 7777-7787 YOUR-VPS-IP
```

---

## Troubleshooting

**"Ports show open but clients still can't join."**
Almost always UDP. Confirm `7777-7787/udp` is allowed in **both** firewalls, and that
a game server is actually bound (`ss -lnup | grep 7777`).

**"Stations appear in the browser but joining times out."**
The station registered its **private** IP. Set `PUBLIC_HOST` in `.env` (and
`RegisterIp` on the Windows game server) to the public IP.

**"Login works, no stations listed."**
Not a port issue — `BUILD_VERSION` must equal the client build (`22284`), or every
station is filtered out.

**"Quests never appear."**
Not a port issue either — the game server needs `DashboardApiUrl` (port **78**) and
`DashboardApiKey`. See `docs/GAME-SERVERS.md`.

**Locked out by ufw.** Use Contabo's **VNC/console** in the panel (it bypasses the
network) and run `sudo ufw disable`.
