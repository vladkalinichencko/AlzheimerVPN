# AlzheimerVPN

AlzheimerVPN is an AmneziaVPN fork focused on the parts of a VPN client that hurt most when they are almost correct: split tunneling, route state, kill switch interaction, connection recovery, and diagnostics.

The app itself keeps the familiar AmneziaVPN product name. The fork name is for the repository and development line.

## What This Fork Is For

AmneziaVPN already has split tunneling, but real networks are messy. A service can use rotating IP addresses. A browser or mail client can reconnect to a different address. A macOS route can point outside the VPN while firewall state still blocks the same traffic. A connection can be stuck while the UI only says that it is still connecting.

AlzheimerVPN makes those situations explicit. It keeps DNS-derived routes, bypass rules, and kill-switch allow rules closer to the same source of truth, and it gives the UI enough diagnostic state to explain what the connection is doing.

## What Changed

### 🧭 Split Tunneling That Tracks Real Hostnames

Split tunneling is built around hostnames resolving to concrete IP routes at connection time.

- URL-like input is reduced to the hostname that routing can actually use.
- Domain rules are resolved on connect and reconnect.
- Every IPv4 address returned by DNS is routed.
- If route installation only partially succeeds, the hostname is resolved again and route installation is retried once.
- Wildcard hostnames can be matched from observed system DNS responses on macOS.

The practical result: services with rotating DNS answers are handled as changing network targets.

### 🧯 Routes And Kill Switch Stay Together

On macOS, split tunneling depends on both route-table state and firewall state. Kill-switch rules must agree with the route table.

AlzheimerVPN synchronizes bypass route IPs with the kill-switch allow-list. If a hostname is excluded from the VPN, the IPs used for that exclusion are also allowed through the firewall path that protects the tunnel.

That avoids the broken middle state where a route points to the physical interface but traffic still gets blocked by VPN safety rules.

### 🩺 Diagnostics For Stuck Or Broken Connections

Connection state is separated into two layers:

- lifecycle: connecting, connected, disconnecting, disconnected;
- health: waiting for handshake, applying routes, checking DNS, checking traffic, recovering, failed because of a concrete problem.

The UI displays diagnostic text only after `VpnConnection` decides that the diagnostic still matches the current lifecycle state. Cleanup work after disconnect cannot leave stale messages on screen, and a transient step cannot overwrite a real failure.

API failures are also logged as structured events, including HTTP status, Qt network error, response body, and endpoint. A generic “configuration retrieval failed” error becomes something a developer can trace.

### 🍎 macOS And Apple Platform Cleanup

This fork updates Apple-platform build and runtime glue around the current implementation line:

- modern macOS build settings;
- CMake-controlled app naming;
- native macOS/iOS lifecycle cleanup where it affects build or runtime behavior;
- separate fork identity for local validation, so the original installed app does not need to be overwritten during development.

### 🛠 Stability Fixes Summary

- URL-like split-tunneling input is normalized to a routable hostname; scheme, path, whitespace and casing no longer break a rule.
- Domain split-tunnel rules are resolved to concrete IPv4 routes at connect time, with a single DNS re-resolution retry on partial route failure.
- DNS-derived split-tunnel routes are refreshed when upstream IPs rotate, instead of going stale.
- macOS route monitor no longer marks a route as added when the kernel route add actually failed.
- Duplicate route add (`already exists`) is treated as success, not as a route failure.
- Bypass route IPs and the kill-switch allow-list are kept in sync.
- The app no longer reports "connected" while DNS or traffic actually do not pass — the lifecycle reaches connected only when traffic verification succeeds.
- The connecting state is bounded by a watchdog and the diagnostic text identifies which stage stalled.
- Vague generic errors (including `No error` and unknown) are replaced with structured, attributable failures.
- Diagnostic text is gated by the current lifecycle state, so cleanup work after disconnect cannot overwrite a real failure.
- DNS, route, backend and traffic failures are surfaced as distinct diagnostic states rather than one generic message.
- API failure logging records HTTP status, Qt network error, response body, and endpoint.
- AmneziaWG on macOS no longer loses its session shortly after handshake — the client-app legacy DNS path no longer races the daemon-side DNS observer.
- Userspace WireGuard / AmneziaWG backend always runs with debug logging, so handshake-level failures are diagnosable in release builds.
- The backend executable is selected from the protocol name, falling back to `wireguard-go` when no AmneziaWG obfuscation parameters are configured.

## Where The Code Changed

The main work is concentrated in these areas:

- `VpnConnection`: connection lifecycle, diagnostics, watchdog, recovery, and health-state filtering.
- Split-tunnel rule parsing and route planning: hostname normalization, wildcard matching, DNS result handling, and retry planning.
- macOS daemon routing/firewall code: route installation, DNS observation, and kill-switch allow-list synchronization.
- UI connection controller/QML: display lifecycle state and diagnostics without deciding whether diagnostics are valid.
- Build configuration: app/service naming, Apple target settings, and dependency/runtime adjustments needed by the fork.

## Diagnostics

Lifecycle text stays simple:

```text
Connect
Connecting
Connected
Disconnecting
```

Diagnostic text explains the actual operation or failure:

```text
Starting VPN protocol
Waiting for VPN handshake
Checking DNS
Applying network routes
Checking traffic
VPN is working
VPN server is not responding
DNS is not resolving domains
VPN is connected, but traffic is not passing
Recovering connection...
```

Structured log example:

```text
AmneziaDiagnostic event=api_failure http_status=500 reply_error=401 response=internal server error
```

## Building From Source

Clone with submodules:

```bash
git clone --recursive <your-fork-url> AlzheimerVPN
cd AlzheimerVPN
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

Install:

- CMake
- Xcode or Xcode Command Line Tools on macOS
- Qt 6.10+
- Conan 2
- Ninja or another CMake-supported generator

Local macOS build:

```bash
export CONAN_HOME="$HOME/.conan2"
cmake -S . -B /private/tmp/alzheimervpn-build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /private/tmp/alzheimervpn-build \
  --target AmneziaVPN AlzheimerVPN-service -- -j4
```

Local macOS staging:

```bash
rm -rf /private/tmp/AlzheimerVPN.app
cp -R /private/tmp/alzheimervpn-build/client/AmneziaVPN.app \
  /private/tmp/AlzheimerVPN.app

cp /private/tmp/alzheimervpn-build/service/server/AlzheimerVPN-service \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/AlzheimerVPN-service
cp /private/tmp/alzheimervpn-build/service/server/amneziawg-go \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/amneziawg-go
cp /private/tmp/alzheimervpn-build/service/server/openvpn \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/openvpn
cp /private/tmp/alzheimervpn-build/service/server/tun2socks \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/tun2socks
cp /private/tmp/alzheimervpn-build/service/server/geoip.dat \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/geoip.dat
cp /private/tmp/alzheimervpn-build/service/server/geosite.dat \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/geosite.dat

chmod 755 \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/AlzheimerVPN-service \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/amneziawg-go \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/openvpn \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/tun2socks
```

The staged bundle is not valid without those service-side runtime files. If
`amneziawg-go` is missing from `Contents/MacOS`, connection attempts fail as
`ErrorCode: 706. Local VPN service failed`.

Repository build scripts:

```bash
deploy/build.sh
deploy/build.sh --installer all
```

## Live Validation

Premium validation needs the same activated profile data that the normal app uses. A fork bundle with only copied split-tunnel lists can show the right rules while still failing account-backed configuration retrieval.

Check these preference keys when validating a fork build against an existing Premium setup:

- `Conf.installationUuid`
- `Servers.serversList`
- `Conf.ExceptSites`
- `Conf.ForwardSites`
- `Conf.routeMode`
- `Conf.appsRouteMode`
- `Conf.sitesSplitTunnelingEnabled`
- `Conf.killSwitchEnabled`
- `Conf.useAmneziaDns`

Before trusting an installed build, compare the built, staged, and installed binaries:

```bash
shasum -a 256 \
  /private/tmp/AlzheimerVPN.app/Contents/MacOS/AmneziaVPN \
  /Applications/AlzheimerVPN.app/Contents/MacOS/AmneziaVPN
```

Also verify the helper files used by the privileged service:

```bash
ls -l /Applications/AlzheimerVPN.app/Contents/MacOS/amneziawg-go \
  /Applications/AlzheimerVPN.app/Contents/MacOS/openvpn \
  /Applications/AlzheimerVPN.app/Contents/MacOS/tun2socks \
  /Applications/AlzheimerVPN.app/Contents/MacOS/geoip.dat \
  /Applications/AlzheimerVPN.app/Contents/MacOS/geosite.dat
```

## Upstream Shape

The useful upstream work can be reviewed in three areas.

First, connection diagnostics: health states, structured failure logs, watchdog behavior, and filtering that keeps stale diagnostics out of the UI.

Second, split tunneling: hostname normalization, DNS refresh on connection setup, all-IPv4 route installation, route retry planning, wildcard DNS observation, and kill-switch synchronization.

Third, Apple platform support: macOS build settings, native lifecycle cleanup, configurable naming, and the daemon-side route/firewall pieces needed for reliable split tunneling.
