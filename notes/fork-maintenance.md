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
