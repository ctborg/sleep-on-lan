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

## Installer

After building `sol.exe`, build the Inno Setup installer:

```bat
ISCC.exe installer\windows\SleepOnLan.iss
```

The installer:

- installs `sol.exe` under `C:\Program Files\SleepOnLan`
- creates `C:\ProgramData\SleepOnLan`
- installs or updates the Windows Service
- opens inbound UDP port 9 in Windows Firewall
- starts the service

On uninstall, it stops and removes the service and deletes the firewall rule.

## Foreground Debugging

Use foreground mode while testing:

```bat
sol.exe run
sol.exe --config C:\ProgramData\SleepOnLan\sol.json run
```

When launched by the Service Control Manager, `sol.exe` auto-detects service context. The service registration does not need a `service` subcommand.
