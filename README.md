# sleep-on-lan

A dependency-free C implementation inspired by [SR-G/sleep-on-lan](https://github.com/SR-G/sleep-on-lan).

It listens for Wake-on-LAN style UDP magic packets. If the MAC address encoded in the packet is the reverse of one of the local machine's MAC addresses, it runs the configured default command. It can also expose a tiny HTTP API for triggering commands and sending normal Wake-on-LAN packets.

## Windows Users
You can [download the MSI installer here](https://github.com/ctborg/sleep-on-lan/releases/latest/download/SleepOnLanSetup.msi)

## Mac Users
You can [download the app on the App Store](https://apps.apple.com/app/computer-wake-up/id6761403700)

## Linux Users
You can follow the build instructions here:
### Build

```sh
make
```

The build uses only the C standard library plus operating-system socket/interface APIs.

Linux/macOS:

```sh
make
```

Windows with MinGW-w64/MSYS2:

```sh
make
```

Windows with Visual Studio Developer Command Prompt:

```bat
build-msvc.bat
```

The Windows build links only system libraries provided by Windows: `ws2_32`, `iphlpapi`, `advapi32`, `ole32`, `oleaut32`, and `uuid`.

For Windows Service installation and the MSI installer, see [README-Windows.md](README-Windows.md).

## Run

```sh
sudo ./sol
```

Ports 7 and 9 normally require elevated privileges. For development, use higher ports:

```sh
./sol --config examples/sol-basic-configuration.json
```

## Configuration

The program looks for configuration in this order:

1. `--config <file>` or `-c <file>`
2. `sol.json` next to the current working directory
3. `/etc/sol.json`
4. `/etc/sleep-on-lan.json`
5. built-in defaults

Generate a full default configuration:

```sh
./sol generate-configuration
./sol --config sol.json generate-configuration
```

Supported fields mirror the original project where practical:

- `Listeners`: `UDP`, `UDP:<port>`, `HTTP`, or `HTTP:<port>`; the default configuration listens on UDP ports 7 and 9
- `BroadcastIP`: broadcast address used by `/wol/<mac>`
- `HTTPOutput`: `XML` or `JSON`
- `Auth.Login` and `Auth.Password`: optional HTTP Basic Auth
- `AvoidDualUDPSending`: suppresses duplicate UDP packets during a short window
- `DelayBeforeCommands`: delays HTTP-triggered commands
- `Commands`: external commands exposed through `/<operation>`

## HTTP API

- `GET /`
- `GET /sleep`
- `GET /quit`
- `GET /wol/aa:bb:cc:dd:ee:ff`
- `GET /state/local/online`
- `GET /state/local`
- `GET /state/ip/<ip>`

## Notes

The default sleep command is `systemctl suspend` on Linux, `pmset sleepnow` on macOS, and `rundll32.exe powrprof.dll,SetSuspendState 0,1,0` on Windows.
