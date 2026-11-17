#!/bin/sh
set -eu

APP="/private/tmp/AlzheimerVPN.app"
EXE="$APP/Contents/MacOS/AmneziaVPN"
LOG="/private/tmp/alzheimervpn-ui-staging.log"
PIDFILE="/private/tmp/alzheimervpn-ui-staging.pid"

usage() {
    echo "usage: $0 [--logs|--stop]" >&2
    exit 2
}

pid_of() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && ps -p "$pid" -o command= | grep -qF "$EXE"; then
            printf '%s\n' "$pid"
            return 0
        fi
    fi
    pgrep -f "$EXE" | head -n 1 || true
}

case "${1:-}" in
    --logs)
        [ -f "$LOG" ] || { echo "no log at $LOG" >&2; exit 1; }
        exec tail -n 120 -f "$LOG"
        ;;
    --stop)
        pid=$(pid_of || true)
        if [ -n "$pid" ]; then
            kill -TERM "$pid" 2>/dev/null || true
            echo "stopped $pid"
        else
            echo "not running"
        fi
        : > "$PIDFILE" 2>/dev/null || true
        exit 0
        ;;
    "")
        ;;
    *)
        usage
        ;;
esac

[ -x "$EXE" ] || { echo "staged app not found at $EXE; run scripts/build-and-install-alzheimervpn.sh --no-install first" >&2; exit 1; }

pid=$(pid_of || true)
if [ -n "$pid" ]; then
    echo "already running pid=$pid"
    echo "log=$LOG"
    exit 0
fi

: > "$LOG"
nohup env ALZHEIMERVPN_UI_TEST_HOME=1 QT_LOGGING_RULES='qt.qml.debug=true;*.warning=true;*.critical=true' "$EXE" >"$LOG" 2>&1 &
pid=$!
printf '%s\n' "$pid" > "$PIDFILE"
sleep 2
if ! kill -0 "$pid" 2>/dev/null; then
    echo "staged app exited during startup; see $LOG" >&2
    exit 1
fi

echo "started pid=$pid"
echo "log=$LOG"
tail -n 40 "$LOG" || true
