# Maintainer Notes

## WiX Toolset v7

The Windows installer workflow uses WiX Toolset v7 to build the MSI artifact.

WiX v7 requires acceptance of the Open Source Maintenance Fee EULA before the command-line tools run. The official project workflow accepts the WiX v7 EULA explicitly in CI with:

```powershell
wix extension add -acceptEula wix7 WixToolset.Firewall.wixext/7.0.0
wix extension add -acceptEula wix7 WixToolset.UI.wixext/7.0.0
wix build -acceptEula wix7 ... -arch x64 -ext WixToolset.Firewall.wixext -ext WixToolset.UI.wixext
```

The `-arch x64` flag is intentional. It keeps the MSI and install directory aligned with the 64-bit `sol.exe` produced by the MSVC build step.

This is intentional for the official `ctborg/sleep-on-lan` build workflow. Forks and downstream distributors should make their own decision about WiX v7 EULA acceptance and maintenance-fee obligations.

Reference: https://docs.firegiant.com/wix/osmf/
