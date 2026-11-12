[Setup]
AppId={{81988AF7-DB48-43CC-A44E-E5BD20185213}
AppName=KMSwitcher Firmware Setup
AppVersion={#AppVersion}
AppPublisher=Frank Tzanabetis
AppPublisherURL=https://plasticcheese.com/
AppCopyright=Copyright (c) 2026 Frank Tzanabetis
VersionInfoVersion={#NumericVersion}
DefaultDirName={localappdata}\Programs\KMSwitcher Setup
DefaultGroupName=KMSwitcher Setup
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#InstallerOutput}
OutputBaseFilename=KMSwitcher-Firmware-Setup-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=kms-setup.exe,kms-firmware.exe
SetupLogging=yes
#ifdef SignInstaller
SignTool=release
SignedUninstaller=yes
#endif
[InstallDelete]
Type: filesandordirs; Name: "{app}\firmware"
Type: files; Name: "{app}\KMSwitcher.html"
[Files]
Source: "{#AppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\KMSwitcher Firmware Setup"; Filename: "{app}\kms-setup.exe"
Name: "{group}\Uninstall"; Filename: "{uninstallexe}"
[Run]
Filename: "{app}\kms-setup.exe"; Description: "Launch KMSwitcher Firmware Setup"; Flags: nowait postinstall skipifsilent
