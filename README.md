# AlzheimerVPN

AlzheimerVPN is a fork of AmneziaVPN with work concentrated around connection reliability, diagnostics, modern Apple platform support, and split tunneling that keeps up with real DNS behavior.

## What Changed

### 🩺 Connection Diagnostics

- The app has explicit connection health states.
  - Lifecycle says whether the app is connecting, connected, disconnecting, or disconnected.
  - Diagnostics say what is actually happening: waiting for handshake, applying routes, checking DNS, checking traffic, recovering, or failing for a specific reason.
- API failures are logged as structured diagnostic events.
  - HTTP status, Qt network error, response body, and endpoint are preserved in logs.
  - Failures like “configuration API returned 500” become visible as concrete diagnostic evidence.
- The connection watchdog turns stuck attempts into a diagnostic state.
  - Connect attempts are bounded by a 30-second watchdog.
  - A stuck connection attempt is reported as diagnostic evidence.

### 🧯 State Correctness

- Transient cleanup states are blocked after terminal connection states.
  - `Checking DNS` is not shown after `Disconnected`.
  - `Checking DNS` does not overwrite a real error after `Error`.
- `VpnConnection` is the source of truth for diagnostic validity.
  - QML and `ConnectionController` only display filtered state.
  - Business logic decides which diagnostics are still valid for the current connection state.
- Recovery and no-traffic handling are explicit.
  - The app can distinguish “connected but no traffic is passing” from a healthy tunnel.
  - Recovery attempts are visible in diagnostics and logs.

### 🧭 Split Tunneling

- Split-tunnel rules are simpler and more predictable.
  - Exact hostnames are supported.
  - Wildcard hostnames with `*` are supported.
  - IP and CIDR rules are supported.
  - The rule parser uses a small, explicit rule language.
- URL-like input is normalized for routing.
  - `https://mail.example.com/inbox` becomes `mail.example.com`.
  - Paths, schemes, query strings, and fragments do not become routing rules.
- Exact domains are re-resolved on connect and reconnect.
  - Rotating DNS answers are refreshed during connection setup.
  - Every resolved IPv4 address is routed.
- Partial route failures get one automatic retry after a fresh DNS resolve.
  - This covers cases where DNS rotated or one route add failed while others succeeded.
- macOS kill switch state is synchronized with bypass routes.
  - The same IPs used for split-tunnel bypass are also added to the kill-switch allow-list.
  - Route state and firewall allow-list state stay aligned.
- macOS wildcard routing can react to system DNS responses.
  - The daemon observes matching DNS answers and adds routes for resolved IPv4 addresses.
  - This is the path that makes wildcard rules useful for hosts with generated subdomains.

### 🍎 Apple Platform Work

- macOS build settings were updated for modern Apple targets.
- App naming is configurable through CMake.
- The app presents as `AmneziaVPN` at runtime.
- Native macOS/iOS glue was cleaned up where it affected build compatibility and app lifecycle behavior.
- The fork keeps the original app identity separate during local live validation so the installed original app does not get overwritten accidentally.

## Diagnostics

The UI separates what the connection **is** from what the connection **is doing**.

Lifecycle text:

```text
Connect
Connecting
Connected
Disconnecting
```

Diagnostic text:

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

The key rule: diagnostics are filtered at the `VpnConnection` source. UI code does not guess whether a message is stale. `VpnConnection` decides that from the current connection state, last error, and diagnostic type.

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

If the repository was already cloned:

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
cmake --build /private/tmp/alzheimervpn-build --target AmneziaVPN -- -j4
```

Upstream build scripts are available:

```bash
deploy/build.sh
deploy/build.sh --installer all
```

## Live Validation

The original app and a fork bundle can use different macOS preference domains. For Premium live validation, the fork must use the same activated profile data as the original app.

Check or copy these keys intentionally:

- `Conf.installationUuid`
- `Servers.serversList`
- `Conf.ExceptSites`
- `Conf.ForwardSites`
- `Conf.routeMode`
- `Conf.appsRouteMode`
- `Conf.sitesSplitTunnelingEnabled`
- `Conf.killSwitchEnabled`
- `Conf.useAmneziaDns`

Before trusting a live installed build, verify that the built, staged, and installed binaries match:

```bash
shasum -a 256 \
  /private/tmp/AmneziaVPNFork.app/Contents/MacOS/AmneziaVPN \
  /Applications/AmneziaVPNFork.app/Contents/MacOS/AmneziaVPN
```

## Upstream Intent

This work can be proposed upstream as one PR, but the changes are easier to review as separate areas:

The first area is diagnostics. It adds connection-health states, structured failure logs, watchdog behavior, and rules that prevent stale UI messages.

The second area is split tunneling. It simplifies rule parsing, re-resolves hostnames, routes all IPv4 answers, retries partial route failures, and keeps kill-switch allow-lists aligned with bypass routes.

The third area is macOS support. It adds DNS observation for wildcard split rules and updates native Apple-platform glue that affects modern builds and runtime behavior.
