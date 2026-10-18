; Inno Setup script for the Windows installer.
; iscc /DAppVersion=<version> /DStageDir=<cmake --install prefix> /DOutputDir=<dir> installer\obs-video-mixes.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\build"
#endif

[Setup]
AppId={{6F0C2D4E-6A1B-4E8F-9C1D-2B7A5E3F9A10}
AppName=Video Mixes for OBS Studio
AppVersion={#AppVersion}
AppVerName=Video Mixes for OBS Studio {#AppVersion}
AppPublisher=AWGshka
AppPublisherURL=https://github.com/AWGshka/obs-video-mixes
AppSupportURL=https://github.com/AWGshka/obs-video-mixes/issues
; OBS 30+ loads per-machine plugins from %ProgramData%\obs-studio\plugins\<name>\bin\64bit + \data
DefaultDirName={commonappdata}\obs-studio\plugins\obs-video-mixes
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=obs-video-mixes-{#AppVersion}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Video Mixes for OBS Studio
; OBS holds this mutex while running; the plugin DLL cannot be replaced then.
AppMutex=OBSStudioCore

[Files]
Source: "{#StageDir}\obs-video-mixes\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Files of older versions that are no longer shipped.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"
