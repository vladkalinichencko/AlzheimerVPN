#!/usr/bin/env bash
set -u

HOSTS=(
  "ya.ru"
  "sso.kinopoisk.ru"
  "chatgpt.com"
  "imap.gmail.com"
)

URLS=(
  "https://ya.ru/"
  "https://sso.kinopoisk.ru/install"
  "https://chatgpt.com/"
  "imaps://imap.gmail.com:993/"
)

DNS_SERVERS=(
  "100.64.0.1"
  "1.1.1.1"
  "8.8.8.8"
)
EXPECT_APP=""

usage() {
  cat <<'USAGE'
Usage:
  tools/vpn_live_matrix.sh [--expect fork|original] [--host HOST ...] [--url URL ...]

What it checks:
  - running AmneziaVPN / AlzheimerVPN processes
  - macOS resolver state from scutil
  - local DNS listeners on :53
  - system DNS resolution for each host
  - explicit DNS resolution through common VPN/public DNS servers
  - route selected for resolved IPv4 addresses
  - curl connectivity for browser-like URL probes

The script does not connect or disconnect VPNs. Use --expect fork or
--expect original when the run must prove which app owns the active tunnel.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      shift
      [[ $# -gt 0 ]] || { echo "--host needs a value" >&2; exit 2; }
      HOSTS+=("$1")
      ;;
    --url)
      shift
      [[ $# -gt 0 ]] || { echo "--url needs a value" >&2; exit 2; }
      URLS+=("$1")
      ;;
    --expect)
      shift
      [[ $# -gt 0 ]] || { echo "--expect needs fork or original" >&2; exit 2; }
      case "$1" in
        fork|original) EXPECT_APP="$1" ;;
        *) echo "--expect needs fork or original" >&2; exit 2 ;;
      esac
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

section() {
  printf '\n== %s ==\n' "$1"
}

run() {
  printf '$ %s\n' "$*"
  "$@" 2>&1 || true
}

section "Time"
date

section "Processes"
processes="$(ps -axo pid,args | grep -E '/Applications/(AlzheimerVPN|AmneziaVPN)\.app|amneziawg-go|wireguard-go' | grep -v grep || true)"
printf '%s\n' "$processes"

section "Active tunnel owner"
if printf '%s\n' "$processes" | grep -Eq '/Applications/AlzheimerVPN.app/Contents/MacOS/(amneziawg-go|wireguard-go)'; then
  active_owner="fork"
elif printf '%s\n' "$processes" | grep -q '/Applications/AmneziaVPN.app/Contents/MacOS/wireguard-go'; then
  active_owner="original"
else
  active_owner="none"
fi
echo "$active_owner"
if [[ -n "$EXPECT_APP" && "$active_owner" != "$EXPECT_APP" ]]; then
  echo "Expected active tunnel owner '$EXPECT_APP', got '$active_owner'." >&2
  exit 3
fi

section "macOS DNS"
run scutil --dns

section "DNS listeners"
run lsof -nP -iUDP:53 -iTCP:53

section "Hosts: system DNS"
for host in "${HOSTS[@]}"; do
  printf '\n-- %s --\n' "$host"
  run dig +time=3 +tries=1 "$host" A
done

section "Hosts: explicit DNS"
for host in "${HOSTS[@]}"; do
  printf '\n-- %s --\n' "$host"
  for dns in "${DNS_SERVERS[@]}"; do
    run dig +time=3 +tries=1 "$host" A "@$dns"
  done
done

section "Routes for resolved IPv4"
for host in "${HOSTS[@]}"; do
  ips="$(dig +short +time=3 +tries=1 "$host" A 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | sort -u)"
  printf '\n-- %s --\n' "$host"
  if [[ -z "$ips" ]]; then
    echo "No IPv4 addresses resolved"
    continue
  fi
  while IFS= read -r ip; do
    [[ -n "$ip" ]] || continue
    run route -n get "$ip"
  done <<< "$ips"
done

section "URL connectivity"
for url in "${URLS[@]}"; do
  printf '\n-- %s --\n' "$url"
  run curl -L --connect-timeout 5 --max-time 12 -o /dev/null -sS -w 'http_code=%{http_code} remote_ip=%{remote_ip} total=%{time_total} namelookup=%{time_namelookup} connect=%{time_connect}\n' "$url"
done
