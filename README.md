# AlzheimerVPN

AlzheimerVPN is an experimental fork of AmneziaVPN focused on making split tunneling less forgetful.

The repository is renamed for the fork, but the application itself still builds and presents as **AmneziaVPN**. That is intentional: the user-facing app name, titlebar, logs, and upstream identity stay familiar, while this fork documents and isolates the split-tunneling work.

## What changed

This fork focuses on one practical problem: split tunneling should behave predictably when sites resolve to changing IP addresses, when wildcard hostnames are used, and when kill switch rules interact with bypass routes.

The main changes are:

- Wildcard hostname rules with `*`, for example `*.example.com`, `api.*.example.com`, or `a*b.example.com`.
- URL input normalization: schemes and paths are stripped, so `https://mail.example.com/inbox` becomes `mail.example.com`.
- Removed special `regex:` and `suffix:` rule modes from the new split-rule parser. The fork keeps the rule language small: exact hostnames, wildcard hostnames, and IP/CIDR rules.
- Re-resolve exact domain rules on connect and reconnect.
- Add routes for all resolved IPv4 addresses, not only the first DNS result.
- Retry route add once after a partial route failure by resolving the hostname again.
- Keep macOS kill switch allow-list entries synchronized with the same IPs used for split-tunnel bypass routes.
- Add a macOS DNS observer path for wildcard rules. When normal system DNS sees a matching hostname, the daemon can add the resolved IPv4 routes and matching kill-switch allow-list entries.
- Add connection diagnostics for slow or broken connection states, including API failures, local service problems, handshake timeout, DNS failure, route mismatch, no-traffic state, and recovery attempts.
- Prevent stale transient diagnostics from leaking into the UI after terminal states. For example, cleanup code can no longer show `Checking DNS` after a disconnect or after a real error.
- Reduce the connect watchdog from 45 seconds to 30 seconds.

## What this does not claim

AlzheimerVPN does not make every possible DNS or browser path observable.

Wildcard routing depends on seeing DNS queries. Normal system DNS is covered by the macOS observer path. Browser DoH/DoT, private DNS inside an app, a pinned IP, or an already cached connection can bypass that observer. Exact hostname rules are still re-resolved on connect/reconnect, but mid-session IP rotation is only handled when the system exposes the lookup.

In other words: this fork makes the common split-tunneling path much more deterministic, but it does not pretend that encrypted app-private DNS can be observed from the system DNS path.

## Useful examples

For Apple Mail with Gmail IMAP, an exact rule like this should be handled by connect/reconnect resolution:

```text
imap.gmail.com
```

Gmail may return multiple IPv4 addresses. This fork routes all returned IPv4 addresses instead of silently choosing only one.

For wildcard hosts, use:

```text
*.example.com
api.*.example.com
```

`*.example.com` does not automatically include `example.com`; add both if both are needed.

## Diagnostics

The UI now separates lifecycle state from diagnostic state.

Lifecycle answers the button-level question:

```text
Connect
Connecting
Connected
Disconnecting
```

Diagnostics answer what the connection is doing or why it failed:

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

The important architectural rule is that QML does not decide whether a diagnostic is truthful. `ConnectionController` and QML only display the filtered state from `VpnConnection`. Transient cleanup diagnostics are blocked at the source after `Disconnected` and `Error`.

API failures are also written as structured diagnostic log lines, for example:

```text
AmneziaDiagnostic event=api_failure http_status=500 reply_error=401 ...
```

## Building from source

Clone with submodules:

```bash
git clone --recursive <your-fork-url> AlzheimerVPN
cd AlzheimerVPN
```

If the repository was already cloned without submodules:

```bash
git submodule update --init --recursive
```

Install the build requirements:

- CMake
- Xcode or Xcode Command Line Tools on macOS
- Qt 6.10+
- Conan 2
- Ninja or another CMake-supported generator

On macOS with Homebrew-style tools, the local build used during development was:

```bash
export CONAN_HOME="$HOME/.conan2"
cmake -S . -B /private/tmp/alzheimervpn-build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /private/tmp/alzheimervpn-build --target AmneziaVPN -- -j4
```

For focused diagnostic tests:

```bash
cmake --build /private/tmp/alzheimervpn-build --target test_connection_health -- -j4
/private/tmp/alzheimervpn-build/client/tests/test_connection_health
```

The project also keeps the upstream deploy scripts:

```bash
deploy/build.sh
deploy/build.sh --installer all
```

## Live testing notes

The original app and a forked test bundle can use different macOS preference domains. If a fork bundle is used for live Premium testing, copy the activated profile data intentionally and back up the fork preferences first.

At minimum compare the original and fork values for:

- `Conf.installationUuid`
- `Servers.serversList`
- `Conf.ExceptSites`
- `Conf.ForwardSites`
- `Conf.routeMode`
- `Conf.appsRouteMode`
- `Conf.sitesSplitTunnelingEnabled`
- `Conf.killSwitchEnabled`
- `Conf.useAmneziaDns`

If only the split-tunneling lists are copied, the UI can show the right rules while Premium API config retrieval still fails because the fork is using a different activated profile.

To verify the installed fork binary is not stale:

```bash
shasum -a 256 \
  /private/tmp/AmneziaVPNFork.app/Contents/MacOS/AmneziaVPN \
  /Applications/AmneziaVPNFork.app/Contents/MacOS/AmneziaVPN
```

The hashes must match before trusting a live UI test.

## Upstream intent

The upstreamable part of this fork is not the joke name. The useful upstream work is:

- simpler split-tunnel rule parsing;
- route planning for changing DNS answers;
- kill-switch and bypass-route consistency;
- macOS DNS observation for wildcard hostnames;
- clearer connection diagnostics;
- tests around split rules, route planning, DNS parsing, and connection health.

Those pieces can be proposed to AmneziaVPN as one PR or split into smaller PRs if maintainers prefer a staged review.

## License

This fork follows the upstream AmneziaVPN licensing model. See `LICENSE` and third-party license files in the repository.
