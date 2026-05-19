#!/usr/bin/env bash
#
# AlzheimerVPN fork test — verifies the latest backend-selection / debug-log
# changes. Run in two phases:
#
#   1) tools/vpn_fork_test.sh prepare      (needs sudo)
#   2) <connect the fork in the UI as usual, wait 15-20s>
#   3) tools/vpn_fork_test.sh collect /tmp/fork-test.txt
#
# Then share /tmp/fork-test.txt.
#
# Safe: phase "collect" is fully read-only. Phase "prepare" only restarts the
# fork service via launchd and truncates fork log files. Does NOT touch the
# original AmneziaVPN.

set -u

EXPECTED_APP_HASH="bbe8b35da62ccc73254d720a3a2e0542e7bcc70721d2819cc719aaad9c2ca37e"
EXPECTED_SVC_HASH="c642131bbebc5b8cb26d5d1d7d4200d405ba83a02dec84532cd02e752189766d"

CLIENT_LOG="$HOME/Library/Application Support/AmneziaVPN.ORG/AmneziaVPN/log/AmneziaVPN.log"
SVC_LOG="/var/log/AlzheimerVPN/service.log"
SVC_ERR="/var/log/AlzheimerVPN/service.err"

phase="${1:-}"
out="${2:-/dev/stdout}"

usage() {
  sed -n '1,16p' "$0"
  exit "${1:-0}"
}

if [[ "$phase" != "prepare" && "$phase" != "collect" ]]; then
  usage 2
fi

sec() { printf '\n=========== %s ===========\n' "$*"; }
run() { printf '$ %s\n' "$*"; "$@" 2>&1 || true; }

# -------- prepare --------
if [[ "$phase" == "prepare" ]]; then
  echo "== Verifying installed binary hashes =="
  app_h=$(shasum -a 256 /Applications/AlzheimerVPN.app/Contents/MacOS/AmneziaVPN | awk '{print $1}')
  svc_h=$(shasum -a 256 /Applications/AlzheimerVPN.app/Contents/MacOS/AlzheimerVPN-service | awk '{print $1}')
  echo "  app    : $app_h"
  echo "  service: $svc_h"
  if [[ "$app_h" != "$EXPECTED_APP_HASH" ]]; then
    echo "  WARNING: app hash mismatch. Expected $EXPECTED_APP_HASH"
  fi
  if [[ "$svc_h" != "$EXPECTED_SVC_HASH" ]]; then
    echo "  WARNING: service hash mismatch. Expected $EXPECTED_SVC_HASH"
  fi

  echo ""
  echo "== Truncating fork logs (needs sudo) =="
  sudo truncate -s 0 "$SVC_LOG" "$SVC_ERR"
  : > "$CLIENT_LOG"
  echo "  done"

  echo ""
  echo "== Restarting fork service (needs sudo) =="
  sudo launchctl kickstart -k system/AlzheimerVPN-service
  sleep 2
  ps -axo pid,etime,user,args | grep AlzheimerVPN-service | grep -v grep
  echo ""
  echo "== Restarting fork app =="
  pkill -f '/Applications/AlzheimerVPN.app/Contents/MacOS/AmneziaVPN$' 2>/dev/null || true
  sleep 1
  open -a /Applications/AlzheimerVPN.app

  echo ""
  echo "PREPARED. Now in the UI:"
  echo "  - make sure the original AmneziaVPN app is fully closed (Cmd+Q)"
  echo "  - hit Connect in the fork (with split tunneling as usual)"
  echo "  - wait 20 seconds"
  echo "  - then run: tools/vpn_fork_test.sh collect /tmp/fork-test.txt"
  exit 0
fi

# -------- collect --------
if [[ "$out" != "/dev/stdout" ]]; then
  exec > >(tee "$out") 2>&1
fi

sec "Time"
date

sec "Processes (VPN-related)"
ps -axo pid,etime,user,args | grep -E '/Applications/(AlzheimerVPN|AmneziaVPN)\.app|wireguard-go|amneziawg-go' | grep -v grep || true

sec "Active tunnel owner"
procs=$(ps -axo args | grep -E '/Applications/(AlzheimerVPN|AmneziaVPN)\.app/Contents/MacOS/(wireguard-go|amneziawg-go)' | grep -v grep || true)
if printf '%s' "$procs" | grep -q '/Applications/AlzheimerVPN.app/Contents/MacOS/'; then
  OWNER=fork
elif printf '%s' "$procs" | grep -q '/Applications/AmneziaVPN.app/Contents/MacOS/'; then
  OWNER=original
else
  OWNER=none
fi
echo "owner=$OWNER"

sec "Which backend the fork actually launched"
grep -E "Starting WireGuard backend" "$SVC_LOG" 2>/dev/null | tail -5

sec "AWG params the UI sent to daemon"
grep -E "AWG params to daemon" "$CLIENT_LOG" 2>/dev/null | tail -3 || echo "(no AWG-params log line — client may not have reached activate())"

sec "AWG params loaded into the backend (UAPI)"
grep -E "UAPI: Updating (private|junk|s[0-9]|h[0-9]|i[0-9])" "$SVC_LOG" 2>/dev/null | tail -20 || echo "(no UAPI lines — backend never received config)"

sec "Handshake state"
grep -E "Sending handshake|Received handshake|Handshake did not|Retrying handshake|unknown type|invalid mac|Receiving keepalive|Sending keepalive" "$SVC_LOG" 2>/dev/null | tail -30 || true

sec "Connection lifecycle"
grep -E "Activating interface|Connection status|Configuring peer|Created wireguard interface|backendFailure" "$SVC_LOG" 2>/dev/null | tail -15 || true

sec "Route table (default + utun)"
netstat -rn -f inet 2>/dev/null | awk 'NR<=4 || /^(default|utun|0\/1|128\/1|100\.)/'

sec "scutil --dns (top)"
scutil --dns 2>/dev/null | sed -n '1,40p'

sec "dig probes"
for h in chatgpt.com ya.ru sso.kinopoisk.ru; do
  printf '\n-- %s system -- \n' "$h"; run dig +time=3 +tries=1 +noall +answer +stats "$h" A
  printf '\n-- %s @100.64.0.1 -- \n' "$h"; run dig +time=3 +tries=1 +noall +answer "$h" A @100.64.0.1
done

sec "curl probes (5s connect, 12s total)"
for u in https://chatgpt.com/ https://ya.ru/; do
  printf '\n-- %s --\n' "$u"
  run curl -L --connect-timeout 5 --max-time 12 -o /dev/null -sS \
    -w 'http=%{http_code} ip=%{remote_ip} t=%{time_total}s namelookup=%{time_namelookup}s\n' "$u"
done

sec "Legacy-IPC noise from service.err (should be EMPTY after fix)"
tail -30 "$SVC_ERR" 2>/dev/null || true

sec "Done"
date
