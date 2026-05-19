#!/usr/bin/env bash
#
# Deep AlzheimerVPN split-tunneling diagnostic.
# Run once in the current state (any owner), then again after switching to fork
# with split tunneling enabled. Compare the two outputs.
#
# Safe: read-only. Does NOT touch VPN, DNS, routes, PF, mDNSResponder.
#
# Usage:
#   tools/vpn_deep_diag.sh                              # write to stdout
#   tools/vpn_deep_diag.sh /tmp/diag-original.txt       # capture to file
#   tools/vpn_deep_diag.sh --expect fork OUT.txt        # also assert owner

set -u

EXPECT=""
OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --expect) shift; EXPECT="${1:-}";;
    -h|--help)
      sed -n '1,20p' "$0"; exit 0 ;;
    *) OUT="$1" ;;
  esac
  shift || true
done

if [[ -n "$OUT" ]]; then
  exec > >(tee "$OUT") 2>&1
fi

HOSTS=( chatgpt.com ya.ru sso.kinopoisk.ru imap.gmail.com )

sec() { printf '\n=========== %s ===========\n' "$*"; }
run() { printf '$ %s\n' "$*"; "$@" 2>&1 || true; }

sec "Time / host"
date
sw_vers 2>/dev/null | tr '\n' ' '; echo
id -un; echo

sec "Processes (VPN-related)"
ps -axo pid,etime,user,args \
  | grep -E '/Applications/(AlzheimerVPN|AmneziaVPN)\.app|wireguard-go|amneziawg-go|AlzheimerVPN-service|AmneziaVPN-service' \
  | grep -v grep || true

sec "Active tunnel owner"
procs="$(ps -axo args | grep -E '/Applications/(AlzheimerVPN|AmneziaVPN)\.app/Contents/MacOS/(wireguard-go|amneziawg-go)' | grep -v grep || true)"
if printf '%s' "$procs" | grep -q '/Applications/AlzheimerVPN.app/Contents/MacOS/'; then
  OWNER=fork
elif printf '%s' "$procs" | grep -q '/Applications/AmneziaVPN.app/Contents/MacOS/'; then
  OWNER=original
else
  OWNER=none
fi
echo "owner=$OWNER"
if [[ -n "$EXPECT" && "$OWNER" != "$EXPECT" ]]; then
  echo "WARNING: expected owner=$EXPECT, got owner=$OWNER"
fi

sec "Installed binary hashes"
for f in \
  /Applications/AlzheimerVPN.app/Contents/MacOS/AmneziaVPN \
  /Applications/AlzheimerVPN.app/Contents/MacOS/AlzheimerVPN-service \
  /Applications/AlzheimerVPN.app/Contents/MacOS/wireguard-go \
  /Applications/AlzheimerVPN.app/Contents/MacOS/amneziawg-go; do
  [[ -f "$f" ]] && shasum -a 256 "$f" 2>/dev/null || echo "(missing) $f"
done

sec "utun interfaces"
ifconfig 2>/dev/null | awk '/^utun[0-9]+/{p=1; print; next} /^[a-z]/{p=0} p'

sec "Routing table (default + utun)"
netstat -rn -f inet 2>/dev/null | awk 'NR<=4 || /^(default|utun)/'

sec "scutil --dns (top half)"
scutil --dns 2>/dev/null | sed -n '1,80p'

sec "DNS listeners on :53"
lsof -nP -iUDP:53 -iTCP:53 2>/dev/null || true

sec "Who owns 127.0.0.1:53"
lsof -nP -iUDP@127.0.0.1:53 -iTCP@127.0.0.1:53 2>/dev/null || true

sec "PF status & blockDNS anchor"
run sudo -n pfctl -s info 2>/dev/null || echo "(pfctl needs sudo; skip if no passwordless sudo)"
run sudo -n pfctl -a 310.blockDNS -s rules 2>/dev/null || true

sec "dig: system resolver"
for h in "${HOSTS[@]}"; do
  printf '\n-- %s (system) --\n' "$h"
  run dig +time=3 +tries=1 +noall +answer +stats "$h" A
done

sec "dig: explicit @127.0.0.1 (does local observer answer?)"
for h in "${HOSTS[@]}"; do
  printf '\n-- %s @127.0.0.1 --\n' "$h"
  run dig +time=3 +tries=1 +noall +answer +comments "$h" A @127.0.0.1
done

sec "dig: explicit @100.64.0.1 (VPN DNS)"
for h in "${HOSTS[@]}"; do
  printf '\n-- %s @100.64.0.1 --\n' "$h"
  run dig +time=3 +tries=1 +noall +answer "$h" A @100.64.0.1
done

sec "route -n get (per host, first IP)"
for h in "${HOSTS[@]}"; do
  ip="$(dig +short +time=3 +tries=1 "$h" A 2>/dev/null | grep -E '^[0-9.]+$' | head -1)"
  printf '\n-- %s -> %s --\n' "$h" "${ip:-(none)}"
  [[ -n "$ip" ]] && run route -n get "$ip"
done

sec "curl probes (12s max each)"
for u in https://chatgpt.com/ https://ya.ru/ https://sso.kinopoisk.ru/install; do
  printf '\n-- %s --\n' "$u"
  run curl -L --connect-timeout 5 --max-time 12 -o /dev/null -sS \
    -w 'http=%{http_code} ip=%{remote_ip} t=%{time_total}s namelookup=%{time_namelookup}s\n' "$u"
done

sec "Tail service.log (key lines, last 60)"
grep -E "Starting WireGuard backend|DNS split route observer|Setting DNS config|Split tunnel DNS resolved|Adding exclusion route|Failed to send routing message|allowNets|blockDNS|Connection status|Received handshake" \
  /var/log/AlzheimerVPN/service.log 2>/dev/null | tail -60 || true

sec "Tail service.err (last 40)"
tail -40 /var/log/AlzheimerVPN/service.err 2>/dev/null || true

sec "Done"
date
