; Sleep on LAN Windows installer.
; Build after compiling sol.exe for Windows:
;   ISCC.exe installer\windows\SleepOnLan.iss

#define MyAppName "Sleep on LAN"
#define MyAppPublisher "Sleep on LAN"
#define MyAppExeName "sol.exe"
#define MyAppVersion "0.1.0"

[Setup]
AppId={{9D94EF61-BF2A-41E3-A9E4-B2EAC1E08FE6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\SleepOnLan
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputBaseFilename=SleepOnLanSetup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=no
RestartApplications=no

[Files]
Source: "..\..\sol.exe"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{commonappdata}\SleepOnLan"
Name: "{commonappdata}\SleepOnLan\logs"

[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Services\EventLog\Application\SleepOnLan"; ValueType: string; ValueName: "EventMessageFile"; ValueData: "{sys}\EventCreate.exe"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Services\EventLog\Application\SleepOnLan"; ValueType: dword; ValueName: "TypesSupported"; ValueData: "7"; Flags: uninsdeletekey

[Icons]
Name: "{group}\Uninstall Sleep on LAN"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "install"; StatusMsg: "Installing Windows Service..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Parameters: "start"; StatusMsg: "Starting Sleep on LAN..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "stop"; Flags: runhidden waituntilterminated; RunOnceId: "StopSleepOnLanService"
Filename: "{app}\{#MyAppExeName}"; Parameters: "uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UninstallSleepOnLanService"
