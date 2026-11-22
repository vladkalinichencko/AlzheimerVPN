# Fork maintenance

## Merge baseline

Branch: `alzheimervpn`. Previous head: `d5b7c0b9`. Upstream: release `5.0.3.0`, commit `de93650a`. Fetch `origin` and merge the latest published stable release tag. Preserve any uncommitted user edits.

The 2026-10-10 merge conflicted only in `deploy/data/macos/AmneziaVPN.plist`. The upstream plist uses `GroupName=amnvpn` without a launchd TCP listener. The staging script generates `Contents/Resources/AlzheimerVPN-service.plist` inside the bundle.

## Fork invariants

| Behavior | Code to check after a merge | Verification |
| --- | --- | --- |
| Separate app, privileged service, IPC socket, instance, Keychain, PF anchor, and runtime directories | `CMakeLists.txt`, `client/cmake/branding`, `version.h.in`, `client/secureQSettings.cpp`, `client/platforms/macos/daemon/macosfirewall.cpp`, staging script | Inspect bundle identifier, generated `version.h`, helper plist, and binary strings. |
| ARM64 app and all VPN helpers | `cmake/platform_settings.cmake`, staging script | The staging script rejects a helper with any other architecture. |
| Wildcard host rules, all DNS answers, prewarming, retained session IPs, synchronized bypass routes and firewall exceptions | `client/core/utils/splitTunnel*`, `client/vpnConnection.cpp`, macOS daemon DNS observer and route monitor | Check that DNS-learned route changes update the same kill-switch allowed IPs. Shared CDN IPs can bypass unrelated hostnames. |
| Xray hostname fallback and VPN routes for internal DNS | `client/core/configurators/xrayConfigurator.cpp`, `client/core/protocols/xrayProtocol.cpp` | Preserve HTTP, TLS, and QUIC sniffing and internal resolver routes. |
| User-selected external DNS on Premium profiles | `client/core/controllers/connectionController.cpp`, `client/vpnConnection.cpp` | With AmneziaDNS off, use configured resolvers and their direct routes. |
| Wake recovery waits for a physical route, recreates protocols, bounds retries, and preserves the diagnostic cause | `client/vpnConnection.cpp`, `client/core/utils/connectionHealth.cpp`, macOS network watcher and route monitor | Trace startup, wake, failed recovery, and explicit disconnect separately. |
| Menu-bar status reflects lifecycle and health | `client/ui/utils/systemTrayNotificationHandler.cpp`, `client/platforms/macos/macosstatusicon.mm` | Paint recovery colour into the image pixels and keep coloured images non-template. Native status-button tinting does not reliably preserve the recovery colour. Preserve opacity and the original red error image. Run `tests/status-icon`. |
| Resolver backup and restore, asynchronous DNS callbacks, direct gateway requests, and attributable errors | `client/platforms/macos/daemon/dnsutilsmacos.cpp`, `client/core/utils/ipcClient.h`, `client/core/controllers/gatewayController.cpp`, `client/vpnConnection.cpp` | Preserve session-scoped resolver backups, avoid nested synchronous DNS callback chains, and retain concrete failures through cleanup. |
| Connection kill switch survives protocol teardown | `service/server/killswitch.cpp`, `ipc/ipc_interface.rep`, `client/vpnConnection.cpp` | Run `tests/killswitch`; initial startup arms protection before protocol cleanup and route waits, errors retain it, and explicit disconnect releases it. |
| Strict kill switch survives explicit disconnect | `service/server/killswitch.cpp` | The regression test checks that strict mode still blocks after release of connection protection. |
| IPv6 cannot escape through the physical interface | macOS WireGuard firewall code and `deploy/data/macos/pf` | Verify `250.blockIPv6` is active when connection protection is armed. |
| Bundled OpenSSL, PF files, isolated helper plist, valid bundle signature | staging script | Run the staging build and its loader, dependency, signature, and architecture checks. |

## Commands

Build without installing or restarting the active client:

```bash
./scripts/build-and-install-alzheimervpn.sh --no-install
```

If the shell lacks Conan, create a temporary build environment:

```bash
python3 -m venv /private/tmp/alzheimervpn-build-tools
/private/tmp/alzheimervpn-build-tools/bin/python -m pip install conan
PATH=/private/tmp/alzheimervpn-build-tools/bin:$PATH ./scripts/build-and-install-alzheimervpn.sh --no-install
```

Conan writes its cache under `~/.conan2`; sandbox failures require an escalated build. The 2026-10-10 build logs are `/private/tmp/alzheimervpn-build-20261010.log`, `/private/tmp/alzheimervpn-build-20261010-retry.log`, and `/private/tmp/alzheimervpn-build-20261010-final.log`. The staging script references QtCore through the Qt library directory so `macdeployqt` records that directory for QML plugin dependencies. Adding a search path alone does not work. The script removes the Homebrew search path from the staged executables after deployment.

Check the staged libraries separately:

```bash
python3 scripts/check-macos-bundle-dependencies.py /private/tmp/AlzheimerVPN.app
```

Run the macOS service regression tests without touching the live firewall:

```bash
cmake -S tests/killswitch -B /private/tmp/alzheimervpn-killswitch-tests
cmake --build /private/tmp/alzheimervpn-killswitch-tests
ctest --test-dir /private/tmp/alzheimervpn-killswitch-tests --output-on-failure
```

The tests compile the production `KillSwitch` implementation with in-memory firewall and settings substitutes. They cover startup, repeated protocol cleanup, explicit disconnect, both split-tunnel modes, DNS updates, strict mode, invalid configuration, and firewall installation failure. They do not verify macOS packet filtering or a real sleep/wake cycle.

If firewall verification fails after rules have been applied, connection protection retains the partial rules until explicit disconnect. Protocol cleanup must not remove the remaining protection because startup failed.

## Installation

The connection-protection IPC method requires the new client and the new service. Close AlzheimerVPN, replace the bundle, then restart the helper. Run these commands only when interruption of the connection is acceptable:

```bash
sudo rm -rf /Applications/AlzheimerVPN.app && sudo ditto /private/tmp/AlzheimerVPN.app /Applications/AlzheimerVPN.app
sudo launchctl bootout system/AlzheimerVPN-service
sudo /bin/sh -c 'rm -f /tmp/local:AlzheimerVpnIpcInterface /tmp/local:AlzheimerVpnIpcInterface_*'
sudo launchctl bootstrap system /Library/LaunchDaemons/AlzheimerVPN.plist
```

Bootout precedes socket cleanup and bootstrap so launchd cannot spawn the new helper before the old socket is gone.

## Current state

2026-11-18: error 1103 recurred because `/Applications/AlzheimerVPN.app` still held the pre-fix build from 2026-11-17 03:31; its baked `PROD_AGW_PUBLIC_KEY_B64` decodes to a PEM with a trailing newline and `tests/agw-key/check_agw_key.py` rejects it, while the staged `/private/tmp/AlzheimerVPN.app` (03:47, current HEAD) bakes the stripped PEM and decrypts the live `endpoints.json` to 6 proxy URLs. The live log shows the exact failure chain: `direct: timeout`, all 14 storage lists `undecodable list`, `proxy list: 0 proxies from cache`, `failover: no proxies to try` → `ApiConfigTimeoutError` (1103). The script now also asserts both baked keys decode to a PEM whose last byte is not `0x0a`, and runs `tests/agw-key/check_agw_key.py` as a build step before staging so a bad key fails the build instead of shipping. Install the staged bundle with the AGENTS.md command to fix the running app.

2026-11-16: upstream `5.1.1.9` (`5aff2ce5`) is merged and pushed as `2db55088` on `fork/alzheimervpn`. The staging script now feeds libagw the upstream `*_B64`/endpoint variables (`PROD_AGW_PUBLIC_KEY_B64`, `SANDBOX_AGW_PUBLIC_KEY_B64`, `SANDBOX_AGW_ENDPOINT`, `SANDBOX_S3_ENDPOINT`, `FREE_V2_ENDPOINT`, `PREM_V1_ENDPOINT`) extracted from `/Applications/AmneziaVPN.app`. `amneziavpn_ru_RU.ts` again contains the live fork strings for connection health, detailed logging, and split-tunnel DNS failures. The verified staging bundle is `/private/tmp/AlzheimerVPN.app`; build log `/private/tmp/alzheimervpn-merge-5.1.1.9-build3.log`. All six MacOS executables are arm64-only, `check-macos-bundle-dependencies.py` checked 203 Mach-O files with 0 unresolved dependencies, `libcrypto.3.dylib` exports `_deflate`, PF rules are staged, and `codesign --verify --deep --strict` passed. If a rebuild stops in the app `POST_BUILD` with `You don’t have permission to save ... Info.plist`, run `chmod u+w /private/tmp/alzheimervpn-build/client/AmneziaVPN.app/Contents/Info.plist /private/tmp/alzheimervpn-build/client/.qt/info_plist/AlzheimerVPN/Info.plist` and rerun `--no-install`. The legacy `DEV_AGW_*`/`DEV_S3_*` test definitions are removed; `tests/logging` and `tests/xray-lifecycle` pass. The installed app and `AlzheimerVPN-service` were not replaced or restarted.

2026-11-17: the merged UI had two QML references to types deleted by the upstream design-system change, which left pages blank while the tab bar still rendered. `PageHome.qml` used `LabelTextType` (now `SmallTextType`), and the fork's `SplitTunnelSiteRow.qml` used `LabelWithButtonType`, removed upstream in `11355e1d`; the row now uses `ListItemType` with the status icon in `trailingContent` and the trash icon in `trailingIconSource`. `PageHome.qml` and `SplitTunnelSiteRow.qml` also referenced palette names removed upstream (`vibrantRed`, `mutedGray`, `paleGray`, `vibrantGreen`); they now use `statusDestructive`, `statusSuccess`, `textTertiary` and `iconSecondary`. `ALZHEIMERVPN_UI_TEST_HOME=1` still skips the start page and `SecureServersRepository::injectUiTestServer` adds an in-memory preview server only in that mode, so the staging app at `/private/tmp/AlzheimerVPN.app` shows a populated Home without a key; nothing is persisted. `scripts/run-staging-ui.sh` launches that bundle with QML logging. `tests/qml-types` runs `qmllint` over all QML plus a style-colour and unresolved-type check (types from C++-registered modules are allowlisted); configure with `cmake -S tests/qml-types -B /private/tmp/alzheimervpn-qml-types-tests` and run `ctest --test-dir /private/tmp/alzheimervpn-qml-types-tests --output-on-failure`. Verified red/green against a bogus `LabelTextType` reference.

2026-11-17 (AGW): error 1103 on every gateway request was caused by `pem_to_b64` in `scripts/build-and-install-alzheimervpn.sh` keeping the trailing newline of the extracted PEM. Upstream embeds the key without it and libagw derives the proxy-list AES key/iv from `sha512` of the exact PEM bytes, so the baked `PROD_AGW_PUBLIC_KEY_B64` produced `undecodable list` on every storage endpoint and no failover proxies. The script now strips the trailing newline; the PROD/SANDBOX assignment is unchanged (block 2 decrypts the live `endpoints.json`). `tests/agw-key` fetches the real `endpoints.json` and decrypts it with the baked key — configure with `cmake -S tests/agw-key -B /private/tmp/alzheimervpn-agw-key-tests` and run `ctest --test-dir /private/tmp/alzheimervpn-agw-key-tests --output-on-failure`, or `python3 tests/agw-key/check_agw_key.py [b64]` directly.

2026-10-23: `tests/status-icon` calls the production macOS icon implementation with an in-memory status bar, without creating a visible status item. It checks yellow recovery pixels, transparency, non-template recovery rendering, the unchanged error image, and connected/disconnected template and opacity settings. The test fails on `3769f10b` because the recovery pixels remain untinted. Run `cmake -S tests/status-icon -B /private/tmp/alzheimervpn-status-icon-tests -DCMAKE_PREFIX_PATH=/opt/homebrew`, then `cmake --build /private/tmp/alzheimervpn-status-icon-tests -j4` and `ctest --test-dir /private/tmp/alzheimervpn-status-icon-tests --output-on-failure`.

2026-10-12: preserve the existing site rows and show only a green check or red cross beside the trash icon. Saved IPs and fixed IP/subnet rules show green immediately. A failed lookup or an unchecked exact hostname without saved addresses shows red. Pending lookups without saved IPs and dynamic masks have no icon. Use the existing check.svg, close.svg and LabelWithButtonType icon slot. No extra captions or Check/Retry buttons. `IpSplitTunnelingController` owns session-only checks; `IpSplitTunnelingModel` reads them and emits row updates. Exact hosts are checked on addition and on imports without saved IPs. Imports with IPs remain unchecked. Wildcards and IP rules do not start lookups. A failed refresh keeps cached IPs. Removal, replacement imports, mode changes, and destruction cancel pending lookups; results must match the current lookup ID before they can save addresses. These states do not report route installation or service reachability. Runtime daemon addresses remain separate from the exported settings cache.

The focused tests compile the production controller, repository, and model with in-memory settings. Run them without touching the active VPN:

```bash
cmake -S tests/split-tunneling -B /private/tmp/alzheimervpn-split-tests -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build /private/tmp/alzheimervpn-split-tests -j4
ctest --test-dir /private/tmp/alzheimervpn-split-tests --output-on-failure
SPLIT_TEST_LIVE_DNS=1 /private/tmp/alzheimervpn-split-tests/split_tunneling
```

The optional live check exercises successful and failed system lookups. All five test cases passed with it enabled. The preview uses the production `SplitTunnelSiteRow.qml` with sample states and the app's Basic control style; it creates an offscreen window without VPN objects:

```bash
/opt/homebrew/opt/qt/bin/lrelease client/translations/amneziavpn_ru_RU.ts -qm /private/tmp/alzheimervpn-site-status-ru.qm
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software /private/tmp/alzheimervpn-split-tests/split_preview "$PWD" /private/tmp/alzheimervpn-site-status-preview.png /private/tmp/alzheimervpn-site-status-ru.qm
```

The verified staging bundle is `/private/tmp/AlzheimerVPN.app`. All 203 Mach-O files have bundled or system dependencies, the client is arm64, and deep strict signature verification passed. The staging log is `/private/tmp/alzheimervpn-site-status-build.log`. Final UI cleanup was compiled in `/private/tmp/alzheimervpn-site-status-final-compile.log`; the freshly linked client was copied into the staged bundle with its existing dependency paths, then dependencies and signature were checked again. Conan required escalation to update `~/.conan2`. Use `./scripts/build-and-install-alzheimervpn.sh --no-install`; do not replace or restart the active client automatically.

2026-10-10: upstream 5.0.3.0 merged in `70b84590`. The connection-protection regression tests passed; removing the protection guard in a temporary copy made the recovery test fail. The staging build completed at `/private/tmp/AlzheimerVPN.app`; all six executables are arm64, deep signature verification passed, and all 203 Mach-O files have bundled or system dependencies. The build log is `/private/tmp/alzheimervpn-build-20261010-final.log`. The version smoke check reached the existing-instance guard, so it does not verify the new GUI. Active app and service have not been replaced or restarted. Real packet capture across sleep/wake remains a post-install check requiring permission to interrupt the connection.

2026-11-21: fixed the post-sleep blackout where PF connection protection stayed armed for hours after auto-recovery exhausted. `VpnConnection` now tracks `m_connectionProtectionActive` and releases it through `releaseConnectionProtection()` on any terminal `Error`/`Disconnected` transition, not only on explicit disconnect; strict kill switch and always-blocked sites still apply daemon-side. `MacosRouteMonitor` no longer deletes exclusion routes when the physical default route flaps — routes are re-pointed with `rtmReplaceRoute` (RTM_CHANGE, RTM_ADD fallback) on default-route updates, route fetches, and repeated `addExclusionRoute` calls, so a missed route message cannot leave the VPN endpoint routed into the dead tunnel. `DnsUtilsMacos::restoreResolvers` now calls `flushSystemDns()` like `updateResolvers` does. Build log `/private/tmp/alzheimervpn-fix-build.log`; staged bundle `/private/tmp/AlzheimerVPN.app`; `tests/killswitch` passes. No unit harness covers `VpnConnection` recovery or the route monitor; real sleep/wake verification remains a post-install check.

2026-11-22. The cached VPN-only list sync timer now follows the VPN controller onto its worker thread. The WireGuard and AmneziaWG daemon accepts DNS-based physical route exceptions only in bypass mode. The macOS DNS observer uses the configured resolvers and restores direct resolution on upstream timeout. The last client disconnect tears down the fork's DNS, routes, and firewall, including any remaining connection or strict protection.

The staging bundle is `/private/tmp/AlzheimerVPN.app` and the build log is `/private/tmp/alzheimervpn-session-stage.log`. The kill-switch, split-tunneling, and Xray lifecycle suites passed. A local DNS fixture with alternate ports verified forwarding, route notification, transaction ID restoration, and timeout. Packet filter syntax, bundle dependencies, and signature passed. Live connect, disconnect, backend crash, and app quit still need a test against the installed fork.

2026-11-22, 21:59–22:00 MSK. The installed fork completed its handshake, then timed out on TCP probes to literal IP addresses and on DNS. The DNS observer switched to direct resolvers after four seconds. The idle site-block anchor was already off when the connection started. After DNS recovery began, the daemon sent the previous controller's `disconnected` response to the replacement controller. Commit `57da63b3` delayed this broadcast until teardown finished, which exposed the race. The stock client subsequently used the same peer and endpoint, and all four TCP targets answered through its tunnel. The first data-path failure remains unresolved.

The daemon now replies to `deactivate` on the requesting socket. The two-socket test in `/private/tmp/alzheimervpn-stop-reply-test` reproduces the broadcast with the previous code and passes with the fix. Actual backend failure notifications still reach the replacement. The staging build is `/private/tmp/AlzheimerVPN.app`, with build output in `/private/tmp/alzheimervpn-stop-reply-stage.log`. The current VPN stayed connected during testing.

The next AmneziaWG attempt has an always-saved `NetTrace` timeline in the hourly service log. Match it to the hourly client log by timestamp and read the route and firewall snapshots after route setup, at handshake, and 7 and 23 seconds later. The trace records the interface and flags macOS selects for the VPN endpoint, DNS servers, and each TCP probe. It also records firewall counters, effective DNS scope, routing socket errors, peer byte counts, and DNS forwarding totals. Read-only route lookup replies from other processes are ignored by the route monitor so the trace itself cannot trigger exclusion-route updates.

The 23:25 MSK trace showed DNS and all four TCP probes switching from `utun8` to `en0` between route installation and the first post-handshake snapshot. Both IPv4 tunnel routes had initially installed successfully. The second attempt kept transferring tunnel data while all probes timed out. Exclusion routes now use `RTM_ADD` and the monitor tracks successful installs for cleanup or gateway changes. It no longer issues `RTM_CHANGE` when applying the bypass list. An offline routing test passed with 211 bypass routes, a gateway change, a conflicting route, and teardown. The staging build log is `/private/tmp/alzheimervpn-route-fix-stage.log`. The post-fix kernel routes still need a live check.
