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

## SignPath MSI Signing

Release builds submit the unsigned MSI to SignPath and replace it with the signed MSI before provenance attestation, artifact upload, and GitHub Release attachment.

Configure these GitHub repository values before cutting a release:

- Secret `SIGNPATH_API_TOKEN`: SignPath API token with permission to submit signing requests for the project and policy
- Variable `SIGNPATH_ORGANIZATION_ID`: SignPath organization ID
- Variable `SIGNPATH_PROJECT_SLUG`: SignPath project slug
- Variable `SIGNPATH_SIGNING_POLICY_SLUG`: SignPath signing policy slug

Pull request builds do not submit to SignPath, so external PRs can still validate that the MSI builds without requiring signing credentials.

The SignPath artifact configuration must accept the GitHub Actions artifact uploaded by `actions/upload-artifact`. This workflow uploads `installer/wix/Output/SleepOnLanSetup.msi` as `SleepOnLanSetup-unsigned-msi`; if the artifact configuration treats uploads as ZIP archives, it should sign the MSI inside that archive and return a signed `SleepOnLanSetup.msi`.

Reference: https://docs.signpath.io/trusted-build-systems/github
