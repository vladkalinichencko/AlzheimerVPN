#!/usr/bin/env bash
set -uo pipefail

REPORT="${1:-/private/tmp/alzheimervpn-connectivity-$(date +%Y%m%d-%H%M%S).log}"
umask 077
exec 3>&1
exec >"$REPORT" 2>&1

date
sw_vers
printf '\nVPN processes\n'
# Process arguments contain SOCKS credentials; record only executable paths.
ps -axo pid,lstart,comm | awk '/AlzheimerVPN.app/ && !/awk/'
printf '\nSystem DNS\n'
scutil --dns
printf '\nIPv4 routes\n'
netstat -rn -f inet
printf '\nIPv6 routes\n'
netstat -rn -f inet6
printf '\nProxy settings\n'
scutil --proxy

for host in chatgpt.com www.youtube.com; do
    printf '\nDNS for %s\n' "$host"
    dig "$host" A +tries=1 +time=3
    dig "$host" AAAA +tries=1 +time=3
    for family in -4 -6; do
        printf '\nHTTPS %s %s\n' "$family" "$host"
        curl "$family" -I -sS -o /dev/null --connect-timeout 5 --max-time 10 \
            -w 'ip=%{remote_ip} status=%{http_code} dns=%{time_namelookup} connect=%{time_connect} tls=%{time_appconnect} total=%{time_total}\n' \
            "https://$host/"
    done
    ip=$(dig "$host" A +short +tries=1 +time=3 | awk '/^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ {print; exit}')
    if [[ -n "$ip" ]]; then
        printf '\nRoute to %s\n' "$ip"
        route -n get "$ip"
        printf '\nHTTPS with DNS bypassed for %s\n' "$host"
        curl -4 -I -sS -o /dev/null --connect-timeout 5 --max-time 10 \
            --resolve "$host:443:$ip" \
            -w 'ip=%{remote_ip} status=%{http_code} connect=%{time_connect} tls=%{time_appconnect} total=%{time_total}\n' \
            "https://$host/"
    fi
done
printf 'Diagnostic report: %s\n' "$REPORT" >&3
