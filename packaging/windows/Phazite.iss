; SPDX-License-Identifier: AGPL-3.0-or-later
; Phazite's installer and uninstaller for Windows, which Inno Setup 6.6 or newer (https://jrsoftware.org/isinfo.php) compiles
; through CMake's installer target, giving it what it packages and how it is named (CMakeLists.txt):
;   cmake --build --preset windows-release --target installer
; - It installs Phazite for the user who runs it, without administrator rights (in %LOCALAPPDATA%\Programs\Phazite), or, with
;   /ALLUSERS, for every user (in Program Files); an update goes where Phazite is, in the same way.
; - Its first page shows Phazite's notice and those of the code and fonts it is built with (NOTICES.txt); it installs their
;   licenses next to the app (licenses\).
; - Phazite's projects (.phazite) open in Phazite, and DAWproject files (.dawproject) offer it in Open With, to import them.
; - Phazite's update check runs it with /SILENT /relaunch=1 as Phazite closes (src/services/Updates.cpp): it waits for
;   Phazite to have closed, installs, and opens Phazite again.
; - The uninstaller takes away the app, its shortcuts and its file associations. Projects, presets, Phazite Sounds and the
;   settings stay where they are (Documents\Phazite, %APPDATA%\Phazite).

#ifndef AppVersion
  #error Build the installer through CMake: cmake --build --preset windows-release --target installer
#endif

[Setup]
; The installation's identity, which updates and the uninstaller find it by: Phazite's bundle ID. Never change it.
AppId=io.github.august-byrne.phazite
AppName=Phazite
AppVersion={#AppVersion}
AppVerName=Phazite {#AppVersion}
AppPublisher=Phazite
AppCopyright={#Copyright}
AppPublisherURL={#SourceUrl}
AppSupportURL={#SourceUrl}/issues
AppUpdatesURL={#SourceUrl}/releases
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Phazite {#AppVersion} installer
DefaultDirName={autopf}\Phazite
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
InfoBeforeFile={#NoticesFolder}\NOTICES.txt
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\Phazite.exe
UninstallDisplayName=Phazite
WizardStyle=modern dynamic
ShowLanguageDialog=no
OutputDir={#OutputFolder}
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes
; Phazite's own [Code] below waits for Phazite to close; this closes a plug-in helper (PhaziteHost.exe) that is still going.
CloseApplications=yes
RestartApplications=no
SetupMutex=PhaziteSetup,Global\PhaziteSetup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; The folders the app ships, put there afresh each time: what an older version had and this one doesn't, goes.
Type: filesandordirs; Name: "{app}\fonts"
Type: filesandordirs; Name: "{app}\licenses"
Type: filesandordirs; Name: "{app}\presets"

[Files]
Source: "{#AppFolder}\Phazite.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppFolder}\PhaziteHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppFolder}\fonts\*"; DestDir: "{app}\fonts"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppFolder}\presets\*"; DestDir: "{app}\presets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#NoticesFolder}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#NoticesFolder}\NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Phazite"; Filename: "{app}\Phazite.exe"
Name: "{autodesktop}\Phazite"; Filename: "{app}\Phazite.exe"; Tasks: desktopicon

[Registry]
; A project (.phazite) opens in Phazite, with Phazite's icon; HKA is the user's classes, or every user's with /ALLUSERS.
Root: HKA; Subkey: "Software\Classes\.phazite"; ValueType: string; ValueName: ""; ValueData: "Phazite.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.phazite\OpenWithProgids"; ValueType: string; ValueName: "Phazite.Project"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\Phazite.Project"; ValueType: string; ValueName: ""; ValueData: "Phazite Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Phazite.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Phazite.exe,0"
Root: HKA; Subkey: "Software\Classes\Phazite.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Phazite.exe"" ""%1"""
; A DAWproject (.dawproject), another DAW's: Phazite in its Open With, to import it, without taking it from that DAW. Its key
; goes with the uninstaller if nothing else is left in it.
Root: HKA; Subkey: "Software\Classes\.dawproject"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.dawproject\OpenWithProgids"; ValueType: string; ValueName: "Phazite.DAWproject"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\Phazite.DAWproject"; ValueType: string; ValueName: ""; ValueData: "DAWproject"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Phazite.DAWproject\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Phazite.exe,0"
Root: HKA; Subkey: "Software\Classes\Phazite.DAWproject\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Phazite.exe"" ""%1"""
; What Phazite opens, for Windows' Open With list.
Root: HKA; Subkey: "Software\Classes\Applications\Phazite.exe"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\Phazite.exe\SupportedTypes"; ValueType: string; ValueName: ".phazite"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\Phazite.exe\SupportedTypes"; ValueType: string; ValueName: ".dawproject"; ValueData: ""

[Run]
Filename: "{app}\Phazite.exe"; Description: "{cm:LaunchProgram,Phazite}"; Flags: nowait postinstall skipifsilent
; After an update that Phazite's update check started, Phazite opens again, as the user who ran it.
Filename: "{app}\Phazite.exe"; Flags: nowait runasoriginaluser; Check: IsUpdateFromPhazite

[Code]
const
  // Phazite holds this mutex while it runs (src/app/Main.cpp): in the session's namespace, or the global one.
  RunningMutexes = 'PhaziteRunning,Global\PhaziteRunning';

// Started by Phazite's update check, as Phazite closes (/relaunch=1: src/services/Updates.cpp).
function IsUpdateFromPhazite: Boolean;
begin
  Result := ExpandConstant('{param:relaunch|0}') = '1';
end;

// Phazite's files can't be replaced or removed while it runs: waits for it to close after an update it started, a minute at
// most, then asks the user to close it while it is still open. False if they cancel.
function WaitForPhaziteToClose(const Purpose: String): Boolean;
var
  Waited: Integer;
begin
  Waited := 0;

  if IsUpdateFromPhazite then
    while CheckForMutexes(RunningMutexes) and (Waited < 600) do
    begin
      Sleep(100);
      Waited := Waited + 1;
    end;

  Result := True;

  while CheckForMutexes(RunningMutexes) do
    if SuppressibleMsgBox('Phazite is open. Close it, saving your work, then click OK to ' + Purpose + '.',
                          mbInformation, MB_OKCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeSetup: Boolean;
begin
  Result := WaitForPhaziteToClose('install it');
end;

function InitializeUninstall: Boolean;
begin
  Result := WaitForPhaziteToClose('uninstall it');
end;
