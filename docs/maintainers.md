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

## Self-Signed MSI Signing

The Windows installer workflow self-signs the MSI with an ephemeral code-signing certificate created on the GitHub Actions runner. The self-signed MSI is then used for provenance attestation, artifact upload, and GitHub Release attachment.

No GitHub secrets or external signing service are required.

The self-signed certificate is intentionally short-lived and build-local:

- It is generated with `New-SelfSignedCertificate -Type CodeSigningCert`.
- It signs the MSI with Windows SDK `signtool.exe`.
- It is not trusted by Windows by default because the certificate chain does not terminate at a public trusted root.
- It can still help show the MSI has not changed since the workflow signed it.

To verify locally on Windows:

```powershell
Get-AuthenticodeSignature .\SleepOnLanSetup.msi | Format-List
```

Expect the signature status to indicate an untrusted root unless the build certificate has been installed into a trusted certificate store.
