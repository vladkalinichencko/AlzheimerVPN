# AlzheimerVPN

AlzheimerVPN is an AmneziaVPN fork focused on the parts of a VPN client that hurt most when they are almost correct: split tunneling, route state, kill switch interaction, connection recovery, and diagnostics.

The app itself keeps the familiar AmneziaVPN product name. The fork name is for the repository and development line.

## What This Fork Is For

AmneziaVPN already has split tunneling, but real networks are messy. A service can use rotating IP addresses. A browser or mail client can reconnect to a different address. A macOS route can point outside the VPN while firewall state still blocks the same traffic. A connection can be stuck while the UI only says that it is still connecting.

AlzheimerVPN makes those situations explicit. It keeps DNS-derived routes, bypass rules, and kill-switch allow rules closer to the same source of truth, and it gives the UI enough diagnostic state to explain what the connection is doing.

## What Changed

### 🧭 Split Tunneling That Tracks Real Hostnames

The upstream client can exclude sites from the VPN, but that mostly works with static addresses and simple host resolution. AlzheimerVPN extends site split tunneling so the list can contain real web hostnames, URL-like input, and wildcard host rules while the routing layer still receives concrete IP routes.

- URL-like input is reduced to the hostname that routing can actually use.
- Exact host rules are prewarmed on connect and reconnect. The daemon starts resolving the known host list immediately, without making VPN startup wait for the whole list to finish.
- Every IPv4 address returned for a matching hostname is added as a bypass route, instead of keeping only one saved address for the site.
- Later DNS answers are also observed. If a matching hostname resolves to more IPs while the VPN is already connected, those IPs are added to the same bypass set.
- A newer DNS answer for the same hostname does not immediately discard older learned IPs during the current session. This avoids breaking browsers that still hold an existing connection or cached address while DNS rotates.
- Wildcard hostnames are supported on macOS by matching observed system DNS responses. This adds support for rules such as `*.example.com`, which are not practical to represent as a fixed IP list ahead of time.
- DNS-learned routes are cleared when split-tunnel configuration changes or the VPN session is torn down.

There is also an Xray-specific safety net. In `VpnAllExceptSites` mode, Xray receives the same split-domain rules and enables domain sniffing for HTTP, TLS, and QUIC. If a browser connects to an already cached IP before the DNS-route layer sees it, Xray can still recognize the hostname from SNI or HTTP Host and send that flow directly instead of proxying it through the VPN.

The practical result: services with rotating DNS answers, CDN pools, and browser-side connection reuse are handled as changing network targets instead of one fixed IP captured at setup time.

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
- DNS-derived split-tunnel routes are refreshed from observed DNS answers and retained for the active session, so rotated CDN IPs and browser-cached addresses do not break mid-load.
- Xray split tunneling has a domain-routing fallback: sniffed HTTP/TLS/QUIC hostnames that match the split list are sent to a direct outbound even when the kernel route table has not learned the IP yet.
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
- DNS resolver restore is idempotent: the original system resolvers are backed up once per session, and a startup sweep removes stale `127.0.0.1 + DomainName=lan` overrides left behind by a previously crashed daemon. A hard kill no longer leaves the machine without DNS until reboot.
- The kill-switch hole for the VPN server IP is opened once, centrally, in `VpnConnection::connectToVpn` for every protocol. Per-protocol duplication that silently forgot AmneziaWG and produced "tunnel connected, no traffic" is gone.
- Gateway API requests always try the direct endpoint first; a once-successful proxy URL is consulted only inside the bypass fallback. A previously-cached proxy that later stops resolving can no longer brick every gateway call until app restart.
- S3 storage discovery and `lmbd-health` proxy probes run in parallel: total wait is bounded by a single timeout instead of summing across all candidates.
- `shouldBypassProxy()` short-circuits on `QNetworkReply::TimeoutError` before attempting body decryption, so a slow upstream no longer produces a misleading "failed to decrypt the data" log line.
- `onConnectingTimedOut` no longer overwrites the real `704 VpnHandshakeTimeout` with a generic `100 UnknownError`. The post-failure teardown ignores the intermediate `Disconnected` signal and preserves the concrete error code.
- Automatic reconnect recreates the protocol object instead of calling `stop()`/`start()` on the same instance. Reconnect through a stale controller/socket no longer requires manually quitting and reopening the app to recover.
- Connected tunnels recover automatically from DNS or traffic failures instead of requiring an app restart.
- After macOS wakes, reconnection waits for the physical network to return instead of getting stuck until the VPN is toggled manually.
- The inherited macOS IPv6 path no longer sends traffic into a dead tunnel. Unsupported IPv6 is blocked without leaking, and websites fall back to IPv4 immediately.
- Manual disconnect force-finalizes to `Disconnected` after the protocol object is dropped, so a daemon callback that never arrives can no longer leave the UI stuck on `Disconnecting`.
- `DnsFailed` is a dedicated probe path (periodic `QHostInfo` lookup of a stable hostname) and is decoupled from the IP HTTP traffic probe. An IP-only probe success no longer masks an actual DNS-only failure, and a later `Healthy` vote from the traffic probe no longer overrides a still-failing resolver.
- `flushDns` is debounced via a single 500ms timer instead of firing once per resolved split-tunnel site, so large split lists no longer flood the daemon with `killall -HUP mDNSResponder`.
- Defensive `deleteTun` at the start of `XrayProtocol::startTun2Socks` hard-fails when cleanup did not actually free the utun, instead of proceeding into a doomed spawn that exits with `ErrorCode 804`.
- `IpcClient::async` helper replaces synchronous `waitForFinished()` for route, DNS, and connectivity probe paths. The previous nested-event-loop pattern recursed via `QHostInfo` callbacks on large split lists and crashed the client with `SIGBUS` at the stack guard.
- DNS server routes (`dns1`/`dns2`) for `VpnAllExceptSites + VLESS/Xray` are installed through the TUN gateway. Upstream installed `/32` overrides via the LAN gateway and broke VPN-internal CGNAT resolvers such as `100.64.0.1`.
- Amnezia Premium, Free, and external Premium profiles honor the `Use AmneziaDNS` switch. When it is disabled, the connection uses the DNS servers selected in app settings instead of silently preferring subscription-provided `dns1`/`dns2` values.
- The fork installs its own privileged daemon (`AlzheimerVPN-service`) with its own `LaunchDaemon` plist and IPC socket, so the original Amnezia daemon is not replaced during development. Both daemons coexist; switching between apps does not require relaunching either daemon.
- The fork uses its own `AlzheimerVPN-Keychain`. Denying Keychain access no longer looks like a missing key and cannot overwrite the original client's encrypted-settings key.
- Daemon-side service logging is enabled unconditionally at process start, so daemon initialization failures are debuggable from `/var/log/AmneziaVPN/AlzheimerVPN-service.log` without needing the GUI to toggle logging first.

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

Build, package, verify, and install the ARM64 fork:

```bash
./scripts/build-and-install-alzheimervpn.sh
```

To build and verify without replacing the installed fork:

```bash
./scripts/build-and-install-alzheimervpn.sh --no-install
```

The script stages `/private/tmp/AlzheimerVPN.app`, restores the Conan OpenSSL
libraries after `macdeployqt`, verifies that every VPN executable is ARM64-only,
checks bundle signing and external library paths, and starts the GUI binary with
`--version` so unresolved loader symbols fail the build before installation.

## Live Validation

Premium validation needs the same activated profile data that the normal app uses. A fork bundle with only copied split-tunnel lists can show the right rules while still failing account-backed configuration retrieval.

Restore an exported Amnezia `.backup` through **Settings → Backup → Restore
from backup** after the first fork launch. The original client and the fork use
separate Keychain entries, so copying encrypted preference values directly does
not transfer a usable profile.

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

## Upstream 5.0.1.5

The current fork is based on upstream `5.0.1.5`. This release adds AWG 3.1,
custom branding support, native macOS status items, and fixes for imported
configs, VPN routes, split-tunneling subnets, IPC validation, and XRay/Telemt.
Fork-specific hostname split tunneling, diagnostics, service isolation, and the
ARM64-only packaging path remain layered on top.
