#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef AppSource
  #error AppSource is required
#endif
[Setup]
AppId={{A507B117-4FDB-48DD-9E41-383D3D55214E}
AppName=KMSwitcher Companion
AppVersion={#AppVersion}
AppPublisher=Frank Tzanabetis
AppPublisherURL=https://plasticcheese.com/
AppCopyright=Copyright (c) 2026 Frank Tzanabetis
VersionInfoVersion={#NumericVersion}
DefaultDirName={localappdata}\Programs\KMSwitcher
DefaultGroupName=KMSwitcher
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#InstallerOutput}
OutputBaseFilename=KMSwitcher-Companion-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\kmswitcher-gui.exe
; Older tray apps ignore close requests. Only registered KMSwitcher executables
; may be force-closed after the graceful quit request below.
CloseApplications=force
CloseApplicationsFilter=kmswitcher*.exe
RestartApplications=no
ChangesEnvironment=yes
SetupLogging=yes
#ifdef SignInstaller
SignTool=release
SignedUninstaller=yes
#endif

[InstallDelete]
Type: files; Name: "{app}\KMSwitcher.html"
Type: files; Name: "{group}\Set up or update KMSwitcher devices.lnk"
Type: filesandordirs; Name: "{app}\firmware"
Type: files; Name: "{app}\kms-firmware.exe"
Type: files; Name: "{app}\kms-setup.exe"

[Files]
Source: "{#AppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\KMSwitcher Companion"; Filename: "{app}\kmswitcher-gui.exe"
Name: "{group}\Uninstall KMSwitcher"; Filename: "{uninstallexe}"

[Tasks]
Name: "startup"; Description: "Start KMSwitcher when I sign in"; Flags: unchecked
Name: "path"; Description: "Add the command-line tool to my user PATH"; Flags: unchecked

[Run]
Filename: "{app}\kmswitcher.exe"; Parameters: "startup enable"; Flags: runhidden; Tasks: startup
Filename: "{app}\kmswitcher-gui.exe"; Parameters: "ui"; Description: "Launch KMSwitcher Companion"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\kmswitcher.exe"; Parameters: "quit"; Flags: runhidden; RunOnceId: "StopCompanion"
Filename: "{app}\kmswitcher.exe"; Parameters: "startup disable"; Flags: runhidden; RunOnceId: "DisableStartup"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var ResultCode: Integer; Companion: String;
begin
  Result := '';
  Companion := ExpandConstant('{app}\kmswitcher.exe');
  if FileExists(Companion) then begin
    Log('Requesting KMSwitcher tray and helper shutdown before replacing files');
    if not Exec(Companion, 'quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      Log('Unable to request graceful shutdown; Restart Manager will close remaining KMSwitcher copies')
    else if ResultCode <> 0 then
      Log('KMSwitcher is still open; Restart Manager will close remaining KMSwitcher copies');
  end;
end;

function PathContains(Value: String; Directory: String): Boolean;
begin
  Result := Pos(';' + Lowercase(Directory) + ';', ';' + Lowercase(Value) + ';') > 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var Value: String;
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('path') then begin
    RegQueryStringValue(HKCU, 'Environment', 'Path', Value);
    if not PathContains(Value, ExpandConstant('{app}')) then begin
      if Value <> '' then Value := Value + ';';
      RegWriteExpandStringValue(HKCU, 'Environment', 'Path', Value + ExpandConstant('{app}'));
      RegWriteDWordValue(HKCU, 'Software\KMSwitcher\Installer', 'AddedPath', 1);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Value, NewValue, Part: String; Index: Integer; Added: Cardinal;
begin
  if (CurUninstallStep = usPostUninstall) and
     RegQueryDWordValue(HKCU, 'Software\KMSwitcher\Installer', 'AddedPath', Added) and (Added = 1) then begin
    RegQueryStringValue(HKCU, 'Environment', 'Path', Value);
    NewValue := '';
    while Value <> '' do begin
      Index := Pos(';', Value);
      if Index = 0 then begin Part := Value; Value := ''; end
      else begin Part := Copy(Value, 1, Index - 1); Delete(Value, 1, Index); end;
      if Lowercase(Part) <> Lowercase(ExpandConstant('{app}')) then begin
        if NewValue <> '' then NewValue := NewValue + ';';
        NewValue := NewValue + Part;
      end;
    end;
    RegWriteExpandStringValue(HKCU, 'Environment', 'Path', NewValue);
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\KMSwitcher\Installer');
  end;
end;
