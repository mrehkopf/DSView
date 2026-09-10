; Inno Setup script for the Windows DSView installer.
;
; Unlike the plain portable zip (the other Windows CI artifact), this
; installer also stages the WinUSB driver binding for DreamSourceLab hardware
; (dsl_usb_instruments.inf, in this same directory) via pnputil, so a fresh
; Windows install detects the hardware without any extra manual driver setup.
;
; Expects to be compiled with the working directory at the repo root, e.g.:
;   iscc installer\windows\dsview.iss /DMyAppVersion=1.5.0
; MyAppVersion defaults below if not passed on the command line.
;
; Source paths below are relative to this script's own directory (Inno
; Setup's default SourceDir), so they reach up to the repo-root-relative
; dsview-dist\ folder that the CI workflow's earlier steps already assemble
; (see .github/workflows/build.yml, the "Install" and "Bundle runtime DLLs"
; steps of the windows job).

#ifndef MyAppVersion
  #define MyAppVersion "1.5.0"
#endif
#define MyAppName "DSView"
#define MyAppPublisher "DreamSourceLab"
#define DistDir "..\..\dsview-dist"

[Setup]
AppId={{7E5B6A4E-5B7B-4C7A-9C7B-6A6E3C1B6A4E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; The whole install (including the pnputil driver-staging step below) needs
; to run elevated; this avoids a second, separate UAC prompt mid-install.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
OutputDir=Output
OutputBaseFilename=DSView-windows-x86_64-setup
UninstallDisplayIcon={app}\DSView.exe

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
Source: "dsl_usb_instruments.inf"; DestDir: "{app}\drivers"; Flags: ignoreversion
; Lets MainFrame::show_driver_hint_once() (mainframe.cpp) tell apart an
; installed copy (driver already staged below) from the portable zip (where
; it still points the user at Zadig, since pnputil can't run unelevated).
Source: "installed_via_setup.marker"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\DSView.exe"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\DSView.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Run]
Filename: "{sys}\pnputil.exe"; Parameters: "/add-driver ""{app}\drivers\dsl_usb_instruments.inf"" /install"; \
    StatusMsg: "Installing DreamSourceLab USB driver..."; Flags: runhidden waituntilterminated
Filename: "{app}\DSView.exe"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
