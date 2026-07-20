# Windows Installation

Sleep on LAN can run as a Windows Service so it starts automatically after reboot and does not require a signed-in user.

## Build

From a Visual Studio Developer Command Prompt:

```bat
build-msvc.bat
```

Or with MinGW/MSYS2:

```sh
make
```

## Service Commands

Run these commands from an elevated terminal:

```bat
sol.exe install
sol.exe start
sol.exe status
sol.exe stop
sol.exe uninstall
```

`install` creates or updates the `SleepOnLan` service and writes a default config if one does not already exist at:

```text
C:\ProgramData\SleepOnLan\sol.json
```

The installed service runs as `LocalSystem` because Windows sleep requires `SeShutdownPrivilege`.

`install` also creates inbound Windows Firewall rules for UDP ports 7 and 9 using the native Windows Firewall API. `uninstall` removes those rules.

## Installer

After building `sol.exe`, build the WiX MSI installer:

```bat
wix extension add -acceptEula wix7 WixToolset.Firewall.wixext/7.0.0
wix extension add -acceptEula wix7 WixToolset.UI.wixext/7.0.0
wix build -acceptEula wix7 installer\wix\Package.wxs -arch x64 -ext WixToolset.Firewall.wixext -ext WixToolset.UI.wixext -d SourceDir=%CD% -o installer\wix\Output\SleepOnLanSetup.msi
```

The installer:

- installs `sol.exe` under `C:\Program Files\SleepOnLan`
- creates `C:\ProgramData\SleepOnLan`
- installs or updates the Windows Service
- opens inbound UDP ports 7 and 9 in Windows Firewall
- optionally starts the service from the final installer screen

The MSI installer also removes the legacy Inno Setup uninstall registration so upgrades from the old `.exe` installer do not leave a duplicate "Sleep on LAN version 0.1.0" entry in Windows Installed apps.

If you leave "Start Sleep on LAN now" unchecked on the final installer screen, the service remains installed with automatic startup and will run after the next reboot. You can also start it later from the Windows Services app, or from an elevated terminal:

```bat
"C:\Program Files\SleepOnLan\sol.exe" start
```

The HTTP API is disabled by default. To enable it, add `HTTP:8009` or another `HTTP:<port>` entry to `Listeners` in:

```text
C:\ProgramData\SleepOnLan\sol.json
```

Uninstall from Windows Settings or the Start Menu "Uninstall Sleep on LAN" entry. On uninstall, it stops and removes the service and deletes the firewall rules.

For manual installs or development builds, run from an elevated terminal:

```bat
scripts\windows\uninstall.bat
```

The manual uninstaller leaves configuration and logs in place:

```text
C:\ProgramData\SleepOnLan
```

## Foreground Debugging

Use foreground mode while testing:

```bat
sol.exe run
sol.exe --config C:\ProgramData\SleepOnLan\sol.json run
```

When launched by the Service Control Manager, `sol.exe` auto-detects service context. The service registration does not need a `service` subcommand.
