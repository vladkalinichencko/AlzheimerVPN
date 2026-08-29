#!/usr/bin/env bash
# Deterministic build + install for the AlzheimerVPN fork (macOS arm64).
#
# The app bundle and privileged helper are isolated from upstream Amnezia:
#   - Bundle:          /Applications/AlzheimerVPN.app  (org.alzheimervpn.AlzheimerVPN)
#   - Service binary:  AlzheimerVPN-service
#   - LaunchDaemon:    /Library/LaunchDaemons/AlzheimerVPN.plist  (label AlzheimerVPN-service)
#   - IPC socket:      local:AlzheimerVpnIpcInterface
#   - Daemon runtime:  /var/run/alzheimervpn, /var/run/alzheimerwg
# The original /Applications/AmneziaVPN.app and /Library/LaunchDaemons/AmneziaVPN.plist
# are intentionally left untouched.
#
# Flags:
#   --reconfigure   Force CMake reconfigure even if CMakeCache.txt is valid.
#   --no-install    Build + stage only; skip the /Applications copy and the
#                   LaunchDaemon bootstrap.
#   --no-test       Compatibility flag; tests are not built from this repo after
#                   upstream moved the client tests out of the main tree.
#   --uninstall     Stop+remove only the AlzheimerVPN helper plist and delete
#                   /Applications/AlzheimerVPN.app. Does not touch original Amnezia.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="/private/tmp/alzheimervpn-build"
STAGE_APP="/private/tmp/AlzheimerVPN.app"
TARGET_APP="/Applications/AlzheimerVPN.app"
ORIGINAL_APP="/Applications/AmneziaVPN.app"
KEY1="/private/tmp/agw_key_1.pem"
KEY2="/private/tmp/agw_key_2.pem"
KEY1_ESC="/private/tmp/agw_key_1.escaped"
KEY2_ESC="/private/tmp/agw_key_2.escaped"
CONAN_WRAPPER="/private/tmp/conan-wrapper"

FORK_SERVICE_NAME="AlzheimerVPN-service"
FORK_KEYCHAIN_NAME="AlzheimerVPN-Keychain"
FORK_PLIST_PATH="/Library/LaunchDaemons/AlzheimerVPN.plist"
FORK_LAUNCHCTL_LABEL="system/AlzheimerVPN-service"
FORK_IPC_URL="local:AlzheimerVpnIpcInterface"
FORK_INSTANCE_NAME="AlzheimerVPNInstance"
FORK_DAEMON_RUN_DIR="/var/run/alzheimervpn"
FORK_WG_RUNTIME_DIR="/var/run/alzheimerwg"
FORK_DAEMON_TMP_SOCKET="/tmp/alzheimervpn.socket"
FORK_XRAY_TUN_NAME="utun22"

DO_RECONFIGURE=0
DO_INSTALL=1
DO_TEST=0
DO_BUILD=1
DO_UNINSTALL=0
for arg in "$@"; do
  case "$arg" in
    --reconfigure) DO_RECONFIGURE=1 ;;
    --no-install)  DO_INSTALL=0 ;;
    --no-test)     DO_TEST=0 ;;
    --uninstall)   DO_BUILD=0; DO_INSTALL=0; DO_TEST=0; DO_UNINSTALL=1 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

step() { printf '\n=== %s ===\n' "$*"; }

extract_original_string() {
  local pattern="$1"
  local value
  set +o pipefail
  value="$(strings -a "$ORIGINAL_APP/Contents/MacOS/AmneziaVPN" | awk "$pattern")"
  set -o pipefail
  printf '%s' "$value"
}

# --- Uninstall fast path: bootout daemon, remove plist + app bundle. ---
if [[ $DO_UNINSTALL -eq 1 ]]; then
  step "Uninstall AlzheimerVPN fork"
  cat >/private/tmp/uninstall_alzheimervpn.sh <<EOF
#!/bin/bash
set -euo pipefail
launchctl bootout system "$FORK_PLIST_PATH" 2>/dev/null || true
rm -f "$FORK_PLIST_PATH"
rm -rf "$TARGET_APP"
rm -f "/tmp/$FORK_IPC_URL" "/tmp/${FORK_IPC_URL}_"*
pkill -f "$TARGET_APP/Contents/MacOS/AmneziaVPN" 2>/dev/null || true
pkill -f "$TARGET_APP/Contents/MacOS/$FORK_SERVICE_NAME" 2>/dev/null || true
EOF
  chmod +x /private/tmp/uninstall_alzheimervpn.sh
  /usr/bin/osascript -e 'do shell script "/private/tmp/uninstall_alzheimervpn.sh" with administrator privileges'
  echo "Removed fork app and helper. Original /Applications/AmneziaVPN.app untouched."
  exit 0
fi

# --- 1. Extract AGW public keys from the installed original binary. ---
step "AGW keys"
if [[ ! -s "$KEY1" || ! -s "$KEY2" ]]; then
  [[ -x "$ORIGINAL_APP/Contents/MacOS/AmneziaVPN" ]] || {
    echo "Original Amnezia bundle missing at $ORIGINAL_APP; cannot extract AGW keys." >&2
    exit 1
  }
  rm -f /private/tmp/agw_key_*.pem
  set +o pipefail
  strings -a "$ORIGINAL_APP/Contents/MacOS/AmneziaVPN" | awk '
    BEGIN{block=0}
    /-----BEGIN PUBLIC KEY-----/{block++; out="/private/tmp/agw_key_" block ".pem"}
    block>0{print > out}
    /-----END PUBLIC KEY-----/{if(block>=2) exit}
  '
  set -o pipefail
fi
[[ -s "$KEY1" && -s "$KEY2" ]] || { echo "Failed to extract two PEM blocks." >&2; exit 1; }

# Encode each .pem block as a single-line string with literal `\n` between
# lines and NO trailing `\n`. The original Amnezia binary's macro string is
# a C string literal without a trailing newline, so SHA512(PROD_AGW_PUBLIC_KEY)
# differs if we append one — and that hash drives AES-decrypt of the S3 proxy
# fallback list (and indirectly the gateway crypto handshake). Empirically
# verified against a captured S3 response: only the no-trailing-newline form
# yields printable JSON. See gatewayController.cpp::getProxyUrlsAsync.
escape_pem() {
  awk 'BEGIN{first=1} {if(!first) printf "\\n"; printf "%s", $0; first=0}' "$1"
}
escape_pem "$KEY1" > "$KEY1_ESC"
escape_pem "$KEY2" > "$KEY2_ESC"

# PROD/DEV assignment. The order in `strings -a` is layout-dependent and was
# the opposite of what we initially assumed: experimentally, block #2 is the
# real PROD (its SHA512-derived AES key successfully decrypts the captured S3
# proxy list payload from the gateway). Set AGW_KEY_SWAP=1 to flip if a future
# original-binary update changes this layout again.
if [[ "${AGW_KEY_SWAP:-0}" == "1" ]]; then
  PROD_AGW_PUBLIC_KEY="$(cat "$KEY1_ESC")"
  DEV_AGW_PUBLIC_KEY="$(cat "$KEY2_ESC")"
  echo "  AGW_KEY_SWAP=1: PROD<-block1, DEV<-block2"
else
  PROD_AGW_PUBLIC_KEY="$(cat "$KEY2_ESC")"
  DEV_AGW_PUBLIC_KEY="$(cat "$KEY1_ESC")"
fi
DEV_AGW_ENDPOINT="http://gw.amnezia.org:80/"
DEV_S3_ENDPOINT="${DEV_S3_ENDPOINT:-$(extract_original_string '/^https:\/\/s3\.eu-north-1\.amazonaws\.com\/amnezia-dev\/$/ { print; exit }')}"
PROD_S3_ENDPOINT="${PROD_S3_ENDPOINT:-$(extract_original_string '/^https:\/\/s3\.eu-north-1\.amazonaws\.com\/amnezia\/, / { print; exit }')}"
FALLBACK_S3_ENDPOINT="${FALLBACK_S3_ENDPOINT:-}"

[[ ${#PROD_AGW_PUBLIC_KEY} -gt 400 ]] || { echo "PROD key too short" >&2; exit 1; }
[[ ${#DEV_AGW_PUBLIC_KEY}  -gt 400 ]] || { echo "DEV key too short"  >&2; exit 1; }
[[ -n "$PROD_S3_ENDPOINT" ]] || { echo "Failed to extract production S3 endpoints from original Amnezia binary." >&2; exit 1; }
[[ -n "$DEV_S3_ENDPOINT" ]] || { echo "Failed to extract development S3 endpoint from original Amnezia binary." >&2; exit 1; }

# --- 2. Conan wrapper (the project's CMake invokes `conan` on PATH). ---
step "Conan wrapper"
mkdir -p "$CONAN_WRAPPER"
if [[ ! -x "$CONAN_WRAPPER/conan" ]]; then
  cat >"$CONAN_WRAPPER/conan" <<'CONAN'
#!/bin/sh
exec python3 -c "import sys; from conan.cli.cli import main; main(sys.argv[1:])" "$@"
CONAN
  chmod +x "$CONAN_WRAPPER/conan"
fi
export PATH="$CONAN_WRAPPER:$PATH"
export CONAN_HOME="${CONAN_HOME:-$HOME/.conan2}"

# --- 3. Decide whether to (re)configure CMake.
# Cache must contain non-truncated AGW keys AND the fork's service name; if any
# is wrong (e.g. you upgraded the script after a prior build) force reconfigure.
step "CMake configure"
NEEDS_CONFIGURE=1
if [[ $DO_RECONFIGURE -eq 1 ]]; then
  NEEDS_CONFIGURE=1
elif [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  NEEDS_CONFIGURE=1
else
  NEEDS_CONFIGURE=0
  CACHED_PROD_LEN=$(awk -F= '/^PROD_AGW_PUBLIC_KEY:/{print length($2)}' "$BUILD_DIR/CMakeCache.txt")
  CACHED_DEV_LEN=$(awk -F= '/^DEV_AGW_PUBLIC_KEY:/{print length($2)}'  "$BUILD_DIR/CMakeCache.txt")
CACHED_SERVICE=$(awk -F= '/^AMNEZIA_SERVICE_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_KEYCHAIN=$(awk -F= '/^AMNEZIA_KEYCHAIN_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_APP_NAME=$(awk -F= '/^AMNEZIA_APPLICATION_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_ORGANIZATION=$(awk -F= '/^AMNEZIA_ORGANIZATION_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_CLIENT_SERVICE=$(awk -F= '/^CLIENT_SERVICE_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_CLIENT_KEYCHAIN=$(awk -F= '/^CLIENT_KEYCHAIN_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_CLIENT_INSTANCE=$(awk -F= '/^CLIENT_APP_INSTANCE_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_BUNDLE=$(awk -F= '/^AMNEZIA_BUNDLE_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_IPC=$(awk -F= '/^AMNEZIA_IPC_SERVICE_URL:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_RUN_DIR=$(awk -F= '/^AMNEZIA_DAEMON_RUN_DIR:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_WG_DIR=$(awk -F= '/^AMNEZIA_WG_RUNTIME_DIR:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_TMP_SOCKET=$(awk -F= '/^AMNEZIA_DAEMON_TMP_SOCKET:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
CACHED_XRAY_TUN=$(awk -F= '/^AMNEZIA_XRAY_TUN_NAME:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
  CACHED_PROD_S3=$(awk -F= '/^PROD_S3_ENDPOINT:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
  CACHED_DEV_S3=$(awk -F= '/^DEV_S3_ENDPOINT:/{print $2}' "$BUILD_DIR/CMakeCache.txt")
  if [[ -z "${CACHED_PROD_LEN:-}" || -z "${CACHED_DEV_LEN:-}" \
        || $CACHED_PROD_LEN -lt 400 || $CACHED_DEV_LEN -lt 400 \
        || "$CACHED_SERVICE" != "$FORK_SERVICE_NAME" \
        || "$CACHED_KEYCHAIN" != "$FORK_KEYCHAIN_NAME" \
        || "$CACHED_APP_NAME" != "AmneziaVPN" \
        || "$CACHED_ORGANIZATION" != "AmneziaVPN.ORG" \
        || "$CACHED_CLIENT_SERVICE" != "$FORK_SERVICE_NAME" \
        || "$CACHED_CLIENT_KEYCHAIN" != "$FORK_KEYCHAIN_NAME" \
        || "$CACHED_CLIENT_INSTANCE" != "$FORK_INSTANCE_NAME" \
        || "$CACHED_BUNDLE" != "AlzheimerVPN" \
        || "$CACHED_IPC" != "$FORK_IPC_URL" \
        || "$CACHED_RUN_DIR" != "$FORK_DAEMON_RUN_DIR" \
        || "$CACHED_WG_DIR" != "$FORK_WG_RUNTIME_DIR" \
        || "$CACHED_TMP_SOCKET" != "$FORK_DAEMON_TMP_SOCKET" \
        || "$CACHED_XRAY_TUN" != "$FORK_XRAY_TUN_NAME" \
        || -z "$CACHED_PROD_S3" \
        || -z "$CACHED_DEV_S3" ]]; then
    NEEDS_CONFIGURE=1
  fi
fi

if [[ $NEEDS_CONFIGURE -eq 1 ]]; then
  rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
  mkdir -p "$BUILD_DIR"
  cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DAMNEZIA_APPLE_ARCHITECTURES=arm64 \
    -DCMAKE_PREFIX_PATH="$BUILD_DIR/conan" \
    -DCONAN_INSTALL_ARGS='--build=missing;-nr' \
    -DAMNEZIA_BUNDLE_NAME=AlzheimerVPN \
    -DAMNEZIA_SERVICE_NAME="$FORK_SERVICE_NAME" \
    -DAMNEZIA_KEYCHAIN_NAME="$FORK_KEYCHAIN_NAME" \
    -DCLIENT_APPLICATION_NAME=AmneziaVPN \
    -DCLIENT_SERVICE_NAME="$FORK_SERVICE_NAME" \
    -DCLIENT_ORGANIZATION_NAME=AmneziaVPN.ORG \
    -DCLIENT_KEYCHAIN_NAME="$FORK_KEYCHAIN_NAME" \
    -DCLIENT_APP_INSTANCE_NAME="$FORK_INSTANCE_NAME" \
    -DBUILD_OSX_APP_IDENTIFIER=org.alzheimervpn.AlzheimerVPN \
    -DAMNEZIA_INSTANCE_SERVER_NAME="$FORK_INSTANCE_NAME" \
    -DAMNEZIA_IPC_SERVICE_URL="$FORK_IPC_URL" \
    -DAMNEZIA_DAEMON_RUN_DIR="$FORK_DAEMON_RUN_DIR" \
    -DAMNEZIA_WG_RUNTIME_DIR="$FORK_WG_RUNTIME_DIR" \
    -DAMNEZIA_DAEMON_TMP_SOCKET="$FORK_DAEMON_TMP_SOCKET" \
    -DAMNEZIA_XRAY_TUN_NAME="$FORK_XRAY_TUN_NAME" \
    -DPROD_AGW_PUBLIC_KEY="$PROD_AGW_PUBLIC_KEY" \
    -DPROD_S3_ENDPOINT="$PROD_S3_ENDPOINT" \
    -DFALLBACK_S3_ENDPOINT="$FALLBACK_S3_ENDPOINT" \
    -DDEV_AGW_PUBLIC_KEY="$DEV_AGW_PUBLIC_KEY" \
    -DDEV_AGW_ENDPOINT="$DEV_AGW_ENDPOINT" \
    -DDEV_S3_ENDPOINT="$DEV_S3_ENDPOINT"
else
  echo "Cache OK, skipping reconfigure (use --reconfigure to force)."
fi

awk -F= '/^(PROD_AGW_PUBLIC_KEY|DEV_AGW_PUBLIC_KEY|PROD_S3_ENDPOINT|FALLBACK_S3_ENDPOINT|DEV_AGW_ENDPOINT|DEV_S3_ENDPOINT|AMNEZIA_BUNDLE_NAME|AMNEZIA_APPLICATION_NAME|AMNEZIA_ORGANIZATION_NAME|AMNEZIA_SERVICE_NAME|AMNEZIA_KEYCHAIN_NAME|AMNEZIA_IPC_SERVICE_URL|AMNEZIA_DAEMON_RUN_DIR|AMNEZIA_WG_RUNTIME_DIR|AMNEZIA_DAEMON_TMP_SOCKET|AMNEZIA_XRAY_TUN_NAME|CLIENT_APPLICATION_NAME|CLIENT_ORGANIZATION_NAME|CLIENT_SERVICE_NAME|CLIENT_KEYCHAIN_NAME|CLIENT_APP_INSTANCE_NAME):/ { split($1,a,":"); print a[1] "=" (length($2) > 60 ? "length=" length($2) : $2) }' "$BUILD_DIR/CMakeCache.txt"

# --- 4. Build.
step "Build"
TARGETS=(AmneziaVPN "$FORK_SERVICE_NAME")
[[ $DO_TEST -eq 1 ]] && TARGETS+=(test_connection_health)
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" -- -j4

if [[ $DO_TEST -eq 1 ]]; then
  step "test_connection_health"
  "$BUILD_DIR/client/tests/test_connection_health"
fi

# --- 5. Stage /private/tmp/AlzheimerVPN.app with all helper binaries.
step "Stage bundle"
rm -rf "$STAGE_APP"
cp -R "$BUILD_DIR/client/AmneziaVPN.app" "$STAGE_APP"
chmod u+w "$STAGE_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleName AlzheimerVPN' "$STAGE_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleDisplayName AlzheimerVPN' "$STAGE_APP/Contents/Info.plist" 2>/dev/null \
  || /usr/libexec/PlistBuddy -c 'Add :CFBundleDisplayName string AlzheimerVPN' "$STAGE_APP/Contents/Info.plist"
cp "$BUILD_DIR/service/server/$FORK_SERVICE_NAME" "$STAGE_APP/Contents/MacOS/$FORK_SERVICE_NAME"
cp "$BUILD_DIR/service/server/amneziawg-go"       "$STAGE_APP/Contents/MacOS/amneziawg-go"
cp "$BUILD_DIR/service/server/openvpn"            "$STAGE_APP/Contents/MacOS/openvpn"
cp "$BUILD_DIR/service/server/tun2socks"          "$STAGE_APP/Contents/MacOS/tun2socks"
cp "$BUILD_DIR/service/server/geoip.dat"          "$STAGE_APP/Contents/MacOS/geoip.dat"
cp "$BUILD_DIR/service/server/geosite.dat"        "$STAGE_APP/Contents/MacOS/geosite.dat"
# Upstream 5.0 uses amneziawg-go for both protocols. Keep the fork's
# protocol-specific filename while using the same ARM64 backend.
cp "$BUILD_DIR/service/server/amneziawg-go"        "$STAGE_APP/Contents/MacOS/wireguard-go"

QT_BIN_DIR="$(qtpaths --query QT_INSTALL_BINS)"
QT_LIB_DIR="$(qtpaths --query QT_INSTALL_LIBS)"
"$QT_BIN_DIR/macdeployqt" "$STAGE_APP" \
  -executable="$STAGE_APP/Contents/MacOS/$FORK_SERVICE_NAME" \
  -qmldir="$REPO_DIR/client/ui/qml" -libpath="$QT_LIB_DIR" -no-codesign -always-overwrite

# macdeployqt can otherwise substitute Homebrew OpenSSL for the Conan build
# used at link time. That mismatch is the source of the macOS 26.6 _deflate
# launch failure.
OPENSSL_PACKAGE_DIR="$(sed -n 's/^set(openssl_PACKAGE_FOLDER_RELEASE "\(.*\)")/\1/p' "$BUILD_DIR/conan/OpenSSL-Targets-release.cmake")"
[[ -d "$OPENSSL_PACKAGE_DIR/lib" ]] || { echo "Conan OpenSSL package not found." >&2; exit 1; }
cp "$OPENSSL_PACKAGE_DIR/lib/libssl.3.dylib" "$STAGE_APP/Contents/Frameworks/"
cp "$OPENSSL_PACKAGE_DIR/lib/libcrypto.3.dylib" "$STAGE_APP/Contents/Frameworks/"

for executable in AmneziaVPN "$FORK_SERVICE_NAME"; do
  executable="$STAGE_APP/Contents/MacOS/$executable"
  if otool -l "$executable" | awk '/cmd LC_RPATH/{getline; getline; print $2}' | grep -x /opt/homebrew/lib >/dev/null; then
    install_name_tool -delete_rpath /opt/homebrew/lib "$executable"
  fi
  if otool -L "$executable" | grep '/opt/homebrew/' >/dev/null; then
    echo "$executable still links to Homebrew libraries." >&2
    exit 1
  fi
done
nm -gU "$STAGE_APP/Contents/Frameworks/libcrypto.3.dylib" | grep ' _deflate$' >/dev/null \
  || { echo "Staged libcrypto does not satisfy the app's zlib symbols." >&2; exit 1; }

# PF (packet filter) rules — MacOSFirewall expects them at
# $BUNDLE/Contents/MacOS/pf/amn.conf + amn.NNN.*.conf. Upstream Amnezia ships
# them via `install(DIRECTORY .../pf ...)`, but we don't run `cmake --install`
# (we manually stage). Without these the daemon spams pfctl errors
# ("No such file or directory") and kill-switch / route exclusion rules don't
# apply.
rm -rf "$STAGE_APP/Contents/MacOS/pf"
cp -R "$REPO_DIR/deploy/data/macos/pf" "$STAGE_APP/Contents/MacOS/pf"

for bin in AmneziaVPN "$FORK_SERVICE_NAME" amneziawg-go wireguard-go openvpn tun2socks; do
  chmod 755 "$STAGE_APP/Contents/MacOS/$bin"
  [[ "$(lipo -archs "$STAGE_APP/Contents/MacOS/$bin")" == "arm64" ]] \
    || { echo "$bin is not ARM64-only." >&2; exit 1; }
done
[[ -f "$STAGE_APP/Contents/MacOS/update-resolv-conf.sh" ]] && chmod 755 "$STAGE_APP/Contents/MacOS/update-resolv-conf.sh"

# Stage the fork LaunchDaemon plist inside the bundle so the install step can
# pick it up from a single known location.
cat >"$STAGE_APP/Contents/Resources/$FORK_SERVICE_NAME.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$FORK_SERVICE_NAME</string>
    <key>ProgramArguments</key>
    <array>
        <string>$TARGET_APP/Contents/MacOS/$FORK_SERVICE_NAME</string>
    </array>
    <key>GroupName</key>
    <string>amnvpn</string>
    <key>KeepAlive</key>
    <true/>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
EOF
xattr -cr "$STAGE_APP"
codesign --force --deep --sign - "$STAGE_APP"
codesign --verify --deep --strict "$STAGE_APP"
"$STAGE_APP/Contents/MacOS/AmneziaVPN" --version >/dev/null

echo "Staged contents:"
find "$STAGE_APP/Contents/MacOS" -maxdepth 1 -type f -print | sort
/usr/libexec/PlistBuddy -c 'Print :CFBundleName' -c 'Print :CFBundleExecutable' -c 'Print :CFBundleIdentifier' "$STAGE_APP/Contents/Info.plist"

if [[ $DO_INSTALL -eq 0 ]]; then
  echo "--no-install set; bundle staged at $STAGE_APP."
  exit 0
fi

# --- 6. Install bundle to /Applications AND bootstrap the fork helper.
# Single admin prompt for both: copy bundle, write/bootstrap the fork
# LaunchDaemon from this staged bundle. The original Amnezia helper plist is
# not touched here.
step "Install to /Applications + bootstrap $FORK_SERVICE_NAME"
INSTALL_SCRIPT="/private/tmp/install_alzheimervpn.sh"
cat >"$INSTALL_SCRIPT" <<EOF
#!/bin/bash
set -euo pipefail

STAGED_APP="$STAGE_APP"
TARGET_APP="$TARGET_APP"
FORK_PLIST="$FORK_PLIST_PATH"
FORK_LABEL="$FORK_LAUNCHCTL_LABEL"
FORK_SERVICE="$FORK_SERVICE_NAME"

# Stop only the fork helper and prior fork GUI/service processes.
launchctl bootout "\$FORK_LABEL" 2>/dev/null || true
pkill -f "\$TARGET_APP/Contents/MacOS/AmneziaVPN"  >/dev/null 2>&1 || true
pkill -f "\$TARGET_APP/Contents/MacOS/\$FORK_SERVICE" >/dev/null 2>&1 || true

# PF rule amn.400.allowPIA.conf references group { amnvpn }. If the group is
# missing, pfctl rejects the whole anchor with "unknown group amnvpn".
if ! dscl . -read /Groups/amnvpn >/dev/null 2>&1; then
  next_gid=\$(dscl . -list /Groups PrimaryGroupID 2>/dev/null | awk '{print \$2}' | sort -n | awk '\$1>=500{g=\$1} END{print (g?g+1:501)}')
  dscl . -create /Groups/amnvpn
  dscl . -create /Groups/amnvpn PrimaryGroupID "\$next_gid"
  dscl . -create /Groups/amnvpn RealName "Amnezia VPN Service Group"
fi

# Clear stale IPC sockets owned by previous runs.
rm -f "/tmp/$FORK_IPC_URL" /tmp/${FORK_IPC_URL}_* 2>/dev/null || true

# Replace bundle.
rm -rf "\$TARGET_APP"
ditto "\$STAGED_APP" "\$TARGET_APP"

for bin in AmneziaVPN \$FORK_SERVICE amneziawg-go wireguard-go openvpn tun2socks; do
  chmod 755 "\$TARGET_APP/Contents/MacOS/\$bin"
done
[[ -f "\$TARGET_APP/Contents/MacOS/update-resolv-conf.sh" ]] && chmod 755 "\$TARGET_APP/Contents/MacOS/update-resolv-conf.sh"
xattr -cr "\$TARGET_APP"
codesign --force --deep --sign - "\$TARGET_APP"

# Install the fork helper plist and start it.
cp "\$TARGET_APP/Contents/Resources/\$FORK_SERVICE.plist" "\$FORK_PLIST"
chown root:wheel "\$FORK_PLIST"
chmod 644 "\$FORK_PLIST"

launchctl enable "\$FORK_LABEL"
launchctl bootstrap system "\$FORK_PLIST"
# Do NOT also kickstart -k here: bootstrap already starts the daemon (the plist
# has RunAtLoad=true). Adding kickstart -k racy-kills that brand-new daemon and
# launchd respawns it before the previous instance has had a chance to unlink
# the fork IPC socket. The respawn then can't QLocalServer::listen()
# on a still-existing socket file and ends up zombified: process alive (KeepAlive
# keeps it that way), no IPC socket. GUI sees "Service is not running" forever
# until next reboot or manual launchctl kickstart -k. Observed in production at
# 18:13 install → 18:24 user test.
# Wait briefly so caller's Verify step sees a bound socket.
for i in 1 2 3 4 5 6 7 8 9 10; do
  if [[ -S "/tmp/$FORK_IPC_URL" ]]; then break; fi
  sleep 0.2
done
EOF
chmod +x "$INSTALL_SCRIPT"
/usr/bin/osascript -e 'do shell script "/private/tmp/install_alzheimervpn.sh" with administrator privileges'

# --- 7. Verify install + daemon health.
step "Verify"
find "$TARGET_APP/Contents/MacOS" -maxdepth 1 -type f -print | sort
/usr/libexec/PlistBuddy -c 'Print :CFBundleName' -c 'Print :CFBundleExecutable' -c 'Print :CFBundleIdentifier' "$TARGET_APP/Contents/Info.plist"
shasum -a 256 "$STAGE_APP/Contents/MacOS/AmneziaVPN" "$TARGET_APP/Contents/MacOS/AmneziaVPN"
echo "Active fork helper plist:"
/usr/libexec/PlistBuddy -c 'Print :Label' -c 'Print :ProgramArguments:0' -c 'Print :GroupName' "$FORK_PLIST_PATH"

echo
echo "Done. Launch with:  open -a $TARGET_APP"
echo
echo "Fork helper runtime:"
echo "  $FORK_SERVICE_NAME ($FORK_PLIST_PATH)"
echo "Uninstall fork with:  $(basename "$0") --uninstall"
