#!/usr/bin/env bash
# =============================================================================
#  Rigel server — one-shot install + start  (Ubuntu/Debian VPS, e.g. Contabo)
#
#    cp .env.example .env && nano .env && sudo ./install.sh
#
#  Idempotent: safe to re-run after editing .env (it re-publishes and restarts).
#  Flags:  --no-build   skip dotnet publish (just re-apply config + restart)
#          --no-firewall  never touch ufw
#          --status     show status and exit
#          --uninstall  stop + remove the service (keeps /opt/rigel data)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

c_ok()   { printf '\033[32m[ok]\033[0m   %s\n' "$*"; }
c_info() { printf '\033[36m[info]\033[0m %s\n' "$*"; }
c_warn() { printf '\033[33m[warn]\033[0m %s\n' "$*"; }
c_err()  { printf '\033[31m[err]\033[0m  %s\n' "$*" >&2; }
die()    { c_err "$*"; exit 1; }

DO_BUILD=1; DO_FIREWALL=1; MODE=install
for a in "$@"; do
  case "$a" in
    --no-build)    DO_BUILD=0 ;;
    --no-firewall) DO_FIREWALL=0 ;;
    --status)      MODE=status ;;
    --uninstall)   MODE=uninstall ;;
    -h|--help)     sed -n '2,14p' "$0"; exit 0 ;;
    *) die "unknown flag: $a" ;;
  esac
done

[ "$(id -u)" -eq 0 ] || die "run as root:  sudo ./install.sh"

# ── config ───────────────────────────────────────────────────────────────────
[ -f .env ] || die ".env not found. Run:  cp .env.example .env && nano .env"
set -a; . ./.env; set +a
: "${PUBLIC_HOST:?set PUBLIC_HOST in .env}"
: "${INSTALL_DIR:=/opt/rigel}"
: "${SERVICE_USER:=rigel}"
SERVICE=rigel-backend

if [ "$MODE" = status ]; then
  systemctl --no-pager status "$SERVICE" || true
  echo; c_info "listening sockets:"; ss -lntup 2>/dev/null | grep -E ":(${PORT_MOTHERSHIP}|${PORT_STATIONDB}|${PORT_EOS}|${PORT_DASHBOARD})\b" || echo "  (none)"
  exit 0
fi
if [ "$MODE" = uninstall ]; then
  systemctl stop "$SERVICE" 2>/dev/null || true
  systemctl disable "$SERVICE" 2>/dev/null || true
  rm -f "/etc/systemd/system/$SERVICE.service"; systemctl daemon-reload
  c_ok "removed $SERVICE (data left in $INSTALL_DIR)"; exit 0
fi

if [ "${MOTHERSHIP_INSECURE:-0}" = "1" ]; then
  c_warn "MOTHERSHIP_INSECURE=1 — ALL identity checks are disabled."
  c_warn "This is an open auth bypass. Never leave it on for a public server."
  read -r -p "Continue anyway? [y/N] " r; [ "${r:-N}" = y ] || exit 1
fi

# ── dependencies ─────────────────────────────────────────────────────────────
c_info "installing dependencies…"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq curl ca-certificates ufw libicu-dev >/dev/null

if ! command -v dotnet >/dev/null 2>&1; then
  c_info "installing .NET 6 runtime…"
  # Microsoft's feed varies by release; the official script is version-stable.
  curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
  bash /tmp/dotnet-install.sh --channel 6.0 --runtime aspnetcore --install-dir /usr/share/dotnet
  ln -sf /usr/share/dotnet/dotnet /usr/bin/dotnet
fi
command -v dotnet >/dev/null || die "dotnet install failed"
c_ok "dotnet $(dotnet --version 2>/dev/null || echo present)"

# ── build ────────────────────────────────────────────────────────────────────
id -u "$SERVICE_USER" >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
mkdir -p "$INSTALL_DIR"

# ── apply .env INTO the source ───────────────────────────────────────────────
# Several values the backend needs are compile-time `const`s, not env vars (the app
# reads them at build time). Without this step you'd edit .env, deploy, and silently
# still be running the old baked-in secrets/app-ids — so patch them in before publish.
apply_env_to_source() {
  local MS=backend/src/AUnrealFeatures.AAMothership/MothershipServer.cs
  local SD=backend/src/AUnrealFeatures.Ares/Servers/A2StationDbServer.cs
  [ -f "$MS" ] && [ -f "$SD" ] || { c_warn "backend sources not found; skipping .env->source"; return; }
  # escape & and / for sed replacement text
  esc() { printf '%s' "$1" | sed -e 's/[&/\]/\\&/g'; }

  sed -i "s/const string SERVER_API_KEY = \"[^\"]*\";/const string SERVER_API_KEY = \"$(esc "$SERVER_API_KEY")\";/" "$MS"
  sed -i "s/const string META_APP_ID     = \"[^\"]*\";/const string META_APP_ID     = \"$(esc "$META_APP_ID")\";/" "$MS"
  sed -i "s/const string META_APP_SECRET = \"[^\"]*\";/const string META_APP_SECRET = \"$(esc "$META_APP_SECRET")\";/" "$MS"
  sed -i "s/INSECURE_TESTING ? \"\" : \"org\.zern\.rigel\"/INSECURE_TESTING ? \"\" : \"$(esc "$QUEST_EXPECTED_PACKAGE")\"/" "$MS"
  sed -i "s/INSECURE_TESTING ? \"\" : \"[0-9a-f]\{64\}\"/INSECURE_TESTING ? \"\" : \"$(esc "$QUEST_EXPECTED_CERT_SHA")\"/" "$MS"
  sed -i "s/private const string BuildVersion = \"[^\"]*\";/private const string BuildVersion = \"$(esc "$BUILD_VERSION")\";/" "$SD"
  sed -i "s/private const int    BuildIdNum   = [0-9]*;/private const int    BuildIdNum   = ${BUILD_VERSION};/" "$SD"

  # verify they actually took (a silently-failed sed is worse than no sed)
  grep -q "SERVER_API_KEY = \"${SERVER_API_KEY}\"" "$MS" && c_ok "applied SERVER_API_KEY" || c_warn "SERVER_API_KEY NOT applied"
  grep -q "BuildVersion = \"${BUILD_VERSION}\""    "$SD" && c_ok "applied BUILD_VERSION=${BUILD_VERSION}" || c_warn "BUILD_VERSION NOT applied"
  [ "${META_APP_SECRET:-CHANGE_ME}" = "CHANGE_ME" ] && c_warn "META_APP_SECRET is still CHANGE_ME — Quest login attestation will fail"

  # never ship a seeded ghost station (this once put a fake OCE_1 at 127.0.0.1 in
  # every player's browser) or stale session state
  local MM=backend/src/AUnrealFeatures.EOSSDK/AUnrealFeatures.EOSSDK/Responses/matchmaking.json
  if [ -f "$MM" ] && grep -q '"sessions"[[:space:]]*:[[:space:]]*\[[[:space:]]*{' "$MM"; then
    c_warn "matchmaking.json seeds a fake session — emptying it"
    printf '{\n    "count": 0,\n    "sessions": []\n}\n' > "$MM"
  fi
  rm -f "$INSTALL_DIR/backend/sessions.json" "$INSTALL_DIR/backend/lobbies.json" 2>/dev/null || true
}

if [ "$DO_BUILD" = 1 ]; then
  [ -d backend/src/AUnrealFeatures.Ares ] || die "backend/src/AUnrealFeatures.Ares not found (run from the bundle root)"
  c_info "applying .env into backend sources…"
  apply_env_to_source
  if ! command -v dotnet >/dev/null || ! dotnet --list-sdks 2>/dev/null | grep -q .; then
    c_info "installing .NET 6 SDK (needed to publish)…"
    bash /tmp/dotnet-install.sh --channel 6.0 --install-dir /usr/share/dotnet 2>/dev/null || \
      { curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh; bash /tmp/dotnet-install.sh --channel 6.0 --install-dir /usr/share/dotnet; }
  fi
  c_info "publishing backend (this takes a minute)…"
  dotnet publish backend/src/AUnrealFeatures.Ares -c Release -o "$INSTALL_DIR/backend" --nologo -v q
  c_ok "published -> $INSTALL_DIR/backend"
fi
chown -R "$SERVICE_USER":"$SERVICE_USER" "$INSTALL_DIR"

# Ports <1024 (90/78/50/80) need this or the service must run as root.
DOTNET_REAL="$(readlink -f "$(command -v dotnet)")"
setcap 'cap_net_bind_service=+ep' "$DOTNET_REAL" || c_warn "setcap failed; low ports may not bind"

# ── systemd unit ─────────────────────────────────────────────────────────────
c_info "writing /etc/systemd/system/$SERVICE.service"
cat > "/etc/systemd/system/$SERVICE.service" <<EOF
[Unit]
Description=Rigel backend (Mothership :${PORT_MOTHERSHIP}, StationDb :${PORT_STATIONDB}, EOS :${PORT_EOS}, Dashboard :${PORT_DASHBOARD})
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${SERVICE_USER}
WorkingDirectory=${INSTALL_DIR}/backend
ExecStart=/usr/bin/dotnet ${INSTALL_DIR}/backend/AUnrealFeatures.Ares.dll
Restart=always
RestartSec=3
Environment=DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
Environment=RIGEL_PUBLIC_HOST=${PUBLIC_HOST}
Environment=RIGEL_SERVER_API_KEY=${SERVER_API_KEY}
Environment=RIGEL_BUILD_VERSION=${BUILD_VERSION}
Environment=META_APP_ID=${META_APP_ID}
Environment=META_APP_SECRET=${META_APP_SECRET}
$([ "${MOTHERSHIP_INSECURE:-0}" = 1 ] && echo "Environment=MOTHERSHIP_INSECURE=1")
$([ "${MOTHERSHIP_ALLOW_NONSTORE:-0}" = 1 ] && echo "Environment=MOTHERSHIP_ALLOW_NONSTORE=1")

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable "$SERVICE" >/dev/null 2>&1 || true

# ── firewall ─────────────────────────────────────────────────────────────────
# NOTE: Contabo ALSO has an external firewall in the customer panel. ufw alone
# is not enough if that is enabled — see docs/CONTABO-PORTS.md.
if [ "$DO_FIREWALL" = 1 ] && [ "${MANAGE_FIREWALL:-1}" = 1 ]; then
  c_info "configuring ufw…"
  ufw --force enable >/dev/null
  ufw allow 22/tcp >/dev/null                                   # keep SSH!
  ufw allow "${GAME_UDP_FIRST}:${GAME_UDP_LAST}/udp" >/dev/null  # game traffic
  if [ -n "${DOMAIN:-}" ]; then
    ufw allow 443/tcp >/dev/null; ufw allow 80/tcp >/dev/null
    c_ok "exposed: 22/tcp, 443+80/tcp, ${GAME_UDP_FIRST}-${GAME_UDP_LAST}/udp"
    c_warn "service ports stay LOCAL (Caddy proxies them) — the secure layout"
  else
    for p in "$PORT_MOTHERSHIP" "$PORT_STATIONDB" "$PORT_EOS" "$PORT_EOS_WS" "$PORT_DASHBOARD"; do
      ufw allow "$p/tcp" >/dev/null
    done
    c_ok "exposed: 22/tcp, ${PORT_MOTHERSHIP},${PORT_STATIONDB},${PORT_EOS},${PORT_EOS_WS},${PORT_DASHBOARD}/tcp, ${GAME_UDP_FIRST}-${GAME_UDP_LAST}/udp"
    c_warn "no DOMAIN set: service ports are exposed in the clear (no TLS)."
    c_warn "orchestrator ${PORT_ORCH_CTL}/${PORT_ORCH_AGENT} deliberately NOT opened — keep them private."
  fi
fi

# ── TLS via Caddy (only when DOMAIN is set) ──────────────────────────────────
if [ -n "${DOMAIN:-}" ]; then
  if ! command -v caddy >/dev/null 2>&1; then
    c_info "installing Caddy…"
    apt-get install -y -qq debian-keyring debian-archive-keyring apt-transport-https >/dev/null
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
    apt-get update -qq && apt-get install -y -qq caddy >/dev/null
  fi
  c_info "writing /etc/caddy/Caddyfile for $DOMAIN"
  cat > /etc/caddy/Caddyfile <<EOF
rigel-ms.${DOMAIN}  { reverse_proxy 127.0.0.1:${PORT_MOTHERSHIP} }
rigel.${DOMAIN}     { reverse_proxy 127.0.0.1:${PORT_STATIONDB} }
rigel-eos.${DOMAIN} { reverse_proxy 127.0.0.1:${PORT_EOS} }
EOF
  systemctl reload caddy 2>/dev/null || systemctl restart caddy
  c_ok "Caddy serving rigel-ms/rigel/rigel-eos.${DOMAIN} (auto-TLS)"
  c_warn "DNS A records for those 3 names must point at ${PUBLIC_HOST} or certs will fail."
fi

# ── start + verify ───────────────────────────────────────────────────────────
c_info "starting $SERVICE…"
systemctl restart "$SERVICE"
sleep 4
systemctl is-active --quiet "$SERVICE" || { journalctl -u "$SERVICE" -n 40 --no-pager; die "$SERVICE failed to start"; }
c_ok "$SERVICE running"

ok=0
for p in "$PORT_MOTHERSHIP" "$PORT_STATIONDB" "$PORT_EOS" "$PORT_DASHBOARD"; do
  if ss -lnt 2>/dev/null | grep -q ":$p\b"; then c_ok "listening on $p"; ok=$((ok+1))
  else c_warn "NOT listening on $p"; fi
done
[ "$ok" -gt 0 ] || c_warn "nothing bound — check: journalctl -u $SERVICE -n 50"

cat <<EOF

────────────────────────────────────────────────────────────────────────────
 Rigel backend installed.

   status   : sudo systemctl status $SERVICE
   logs     : sudo journalctl -u $SERVICE -f
   restart  : sudo systemctl restart $SERVICE
   reconfig : edit .env then  sudo ./install.sh --no-build

 Point your game servers at this box (on the WINDOWS machine that runs
 A2-Win64-Shipping.exe, in HalcyonA2\\tools\\server.config.psd1):

   MothershipHost  = '${PUBLIC_HOST}'
   MothershipPort  = ${PORT_MOTHERSHIP}
   ServerApiKey    = '<SERVER_API_KEY from .env>'
   DashboardApiUrl = 'http://${PUBLIC_HOST}:${PORT_STATIONDB}'   <- REQUIRED FOR QUESTS
   DashboardApiKey = '${DASHBOARD_API_KEY}'

 Contabo also has an EXTERNAL firewall in the customer panel — ufw alone is
 not enough. See docs/CONTABO-PORTS.md.
────────────────────────────────────────────────────────────────────────────
EOF
