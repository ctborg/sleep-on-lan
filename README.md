# sleep-on-lan

A dependency-free C implementation inspired by [SR-G/sleep-on-lan](https://github.com/SR-G/sleep-on-lan).

It listens for Wake-on-LAN style UDP magic packets. If the MAC address encoded in the packet is the reverse of one of the local machine's MAC addresses, it runs the configured default command. It can also expose a tiny HTTP API for triggering commands and sending normal Wake-on-LAN packets.

## Build

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

The Windows build links only `ws2_32`, `iphlpapi`, and `advapi32`, all provided by Windows.

For Windows Service installation and the Inno Setup installer, see [README-Windows.md](README-Windows.md).

## Run

```sh
sudo ./sol
```

Port 9 normally requires elevated privileges. For development, use higher ports:

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

- `Listeners`: `UDP`, `UDP:<port>`, `HTTP`, or `HTTP:<port>`
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
