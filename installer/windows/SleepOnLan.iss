; Sleep on LAN Windows installer.
; Build after compiling sol.exe for Windows:
;   ISCC.exe installer\windows\SleepOnLan.iss

#define MyAppName "Sleep on LAN"
#define MyAppPublisher "Sleep on LAN"
#define MyAppExeName "sol.exe"
#define MyAppVersion "0.1.0"
#define FirewallRuleName7 "Sleep on LAN UDP 7"
#define FirewallRuleName9 "Sleep on LAN UDP 9"

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
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\..\sol.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\scripts\windows\uninstall.bat"; DestDir: "{app}"; Flags: ignoreversion

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
Filename: "{cmd}"; Parameters: "/C netsh advfirewall firewall delete rule name=""{#FirewallRuleName7}"" protocol=UDP localport=7 >NUL 2>NUL"; StatusMsg: "Refreshing firewall rules..."; Flags: runhidden waituntilterminated
Filename: "{cmd}"; Parameters: "/C netsh advfirewall firewall delete rule name=""{#FirewallRuleName9}"" protocol=UDP localport=9 >NUL 2>NUL"; StatusMsg: "Refreshing firewall rules..."; Flags: runhidden waituntilterminated
Filename: "{cmd}"; Parameters: "/C netsh advfirewall firewall add rule name=""{#FirewallRuleName7}"" dir=in action=allow protocol=UDP localport=7 >NUL"; StatusMsg: "Opening UDP port 7..."; Flags: runhidden waituntilterminated
Filename: "{cmd}"; Parameters: "/C netsh advfirewall firewall add rule name=""{#FirewallRuleName9}"" dir=in action=allow protocol=UDP localport=9 >NUL"; StatusMsg: "Opening UDP port 9..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Parameters: "start"; StatusMsg: "Starting Sleep on LAN..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "stop"; Flags: runhidden waituntilterminated; RunOnceId: "StopSleepOnLanService"
Filename: "{app}\{#MyAppExeName}"; Parameters: "uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UninstallSleepOnLanService"
Filename: "{cmd}"; Parameters: "/C netsh advfirewall firewall delete rule name=""{#FirewallRuleName7}"" protocol=UDP localport=7 >NUL 2>NUL"; Flags: runhidden waituntilterminated; RunOnceId: "DeleteSleepOnLanFirewallRule7"
Filename: "{cmd}"; Parameters: "/C netsh advfirewall firewall delete rule name=""{#FirewallRuleName9}"" protocol=UDP localport=9 >NUL 2>NUL"; Flags: runhidden waituntilterminated; RunOnceId: "DeleteSleepOnLanFirewallRule9"
