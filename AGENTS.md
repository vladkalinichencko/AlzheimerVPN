# AlzheimerVPN repository workflow

After changing this fork, build and verify the macOS app in staging mode. Do not install it or replace the running app automatically.

Leave the verified bundle at `/private/tmp/AlzheimerVPN.app`, then give the user this exact installation command:

```bash
sudo launchctl bootout system/AlzheimerVPN-service; sudo rm -f "/tmp/local:AlzheimerVpnIpcInterface"; sudo rm -rf /Applications/AlzheimerVPN.app && sudo ditto /private/tmp/AlzheimerVPN.app /Applications/AlzheimerVPN.app && sudo launchctl bootstrap system /Library/LaunchDaemons/AlzheimerVPN.plist
```

The command restarts the privileged `AlzheimerVPN-service` daemon together with the bundle. A daemon left running from the old bundle rejects a client with a changed IPC interface (`ipc/ipc_interface.rep`) and the app fails with error 603 "Local VPN service failed".

The user decides when to run it because replacing the installed VPN interrupts the active client.
