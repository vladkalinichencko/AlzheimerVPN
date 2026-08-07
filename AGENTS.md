# AlzheimerVPN repository workflow

After changing this fork, build and verify the macOS app in staging mode. Do not install it or replace the running app automatically.

Leave the verified bundle at `/private/tmp/AlzheimerVPN.app`, then give the user this exact installation command:

```bash
sudo rm -rf /Applications/AlzheimerVPN.app && sudo ditto /private/tmp/AlzheimerVPN.app /Applications/AlzheimerVPN.app
```

The user decides when to run it because replacing the installed VPN interrupts the active client.
