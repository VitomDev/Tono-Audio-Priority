; Tono Audio Priority installer (Inno Setup 6).
;
; Per-user install without administrator rights. "Start with Windows" is
; enabled on a fresh install only; upgrades keep whatever the user chose.
; Uninstalling removes the startup entry and the settings.
;
; Built by build.ps1, which passes /DBuildDir=<folder with Tono.exe>.

#define AppVersion "1.0.1"
#define AppName "Tono Audio Priority"
#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
#define StartupApprovedKey "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"

#ifndef BuildDir
  #define BuildDir "..\build\app"
#endif

[Setup]
AppId={{9DC05A86-2A29-4019-A406-25FBA3A4E1B8}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=VitomDev
AppPublisherURL=https://github.com/VitomDev/Tono-Audio-Priority
AppSupportURL=https://github.com/VitomDev/Tono-Audio-Priority/issues
AppCopyright=Copyright (c) 2026 VitomDev
VersionInfoVersion={#AppVersion}
VersionInfoCompany=VitomDev
VersionInfoDescription={#AppName} - Audio output priority Setup
DefaultDirName={localappdata}\Programs\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=TonoSetup
SetupIconFile=..\assets\tono.ico
UninstallDisplayIcon={app}\Tono.exe
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
; Tono is closed by the code below, through its own window.
CloseApplications=no

[Files]
Source: "{#BuildDir}\Tono.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Tono.exe"

[Run]
; The usual "Launch" checkbox on the last page, checked by default. Tono
; starts once Finish is clicked; silent upgrades start it as well.
Filename: "{app}\Tono.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\Tono.exe"; Flags: nowait; Check: WizardSilent

[Code]
var
  IsUpgrade: Boolean;

// Tono closes cleanly (tray icon, timers) when its window gets WM_CLOSE.
procedure CloseRunningTono;
var
  Window: HWND;
  Attempts: Integer;
begin
  Window := FindWindowByClassName('TonoAudioPriorityTray');
  if Window = 0 then Exit;
  PostMessage(Window, 16 { WM_CLOSE }, 0, 0);
  Attempts := 0;
  while (FindWindowByClassName('TonoAudioPriorityTray') <> 0) and (Attempts < 50) do
  begin
    Sleep(100);
    Attempts := Attempts + 1;
  end;
  // Give the process a moment to exit after its window is gone.
  Sleep(300);
end;

function InitializeSetup: Boolean;
begin
  IsUpgrade := RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupSetting("AppId")}_is1');
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningTono;
  Result := '';
end;

function AutostartCommand: String;
begin
  Result := '"' + ExpandConstant('{app}\Tono.exe') + '" --background';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if not IsUpgrade then
      // Fresh install: Start with Windows is on by default.
      RegWriteStringValue(HKCU, '{#RunKey}', '{#AppName}', AutostartCommand)
    else if RegValueExists(HKCU, '{#RunKey}', '{#AppName}') then
      // Upgrade: keep the user's choice, only refresh the path.
      RegWriteStringValue(HKCU, '{#RunKey}', '{#AppName}', AutostartCommand);
  end;
end;

function InitializeUninstall: Boolean;
begin
  CloseRunningTono;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RegDeleteValue(HKCU, '{#RunKey}', '{#AppName}');
    RegDeleteValue(HKCU, '{#StartupApprovedKey}', '{#AppName}');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\{#AppName}');
    // HeadsetControl, if it was downloaded.
    DelTree(ExpandConstant('{localappdata}\{#AppName}'), True, True, True);
  end;
end;
