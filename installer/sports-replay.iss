; Inno Setup installer for Sports Replay (OBS Studio plugin)
; Author: Systec - https://www.systecinformatica.com.ar

#define MyName "Sports Replay for OBS Studio"
#ifndef MyVersion
  #define MyVersion "1.0.0"
#endif
#define MyPublisher "Systec"
#define MyURL "https://www.systecinformatica.com.ar"

[Setup]
AppName={#MyName}
AppVersion={#MyVersion}
AppPublisher={#MyPublisher}
AppPublisherURL={#MyURL}
DefaultDirName={code:GetOBSDir}
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallDisplayName={#MyName}
UninstallDisplayIcon={app}\bin\64bit\obs64.exe
OutputBaseFilename=sports-replay-{#MyVersion}-windows-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
LicenseFile=LICENSE.txt
WizardStyle=modern
AppSupportURL={#MyURL}

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
spanish.NoOBS=No se encontró OBS Studio en esa carpeta (falta bin\64bit\obs64.exe).%nElegí la carpeta donde está instalado OBS Studio.%n%n¿Continuar de todos modos?
english.NoOBS=OBS Studio was not found in that folder (bin\64bit\obs64.exe is missing).%nChoose the folder where OBS Studio is installed.%n%nContinue anyway?
spanish.UnknownFFmpeg=No se reconoce la versión de FFmpeg de este OBS Studio (no hay avcodec-61.dll ni avcodec-62.dll en bin\64bit), así que Sports Replay no podría cargarse.%n%nActualizá OBS Studio o elegí otra carpeta.
english.UnknownFFmpeg=The FFmpeg version of this OBS Studio is not recognized (neither avcodec-61.dll nor avcodec-62.dll is in bin\64bit), so Sports Replay could not load.%n%nUpdate OBS Studio or choose another folder.

[Files]
; FFmpeg's DLL names carry its major version, and OBS 32.2 moved from FFmpeg 7
; to 8, so the plugin is built twice. The build installed is the one matching
; the FFmpeg this OBS ships, checked by file rather than by OBS version.
Source: "ffmpeg7\sports-replay\bin\64bit\sports-replay.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Check: OBSUsesFFmpeg7
Source: "ffmpeg8\sports-replay\bin\64bit\sports-replay.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion; Check: OBSUsesFFmpeg8
Source: "ffmpeg7\sports-replay\data\locale\*"; DestDir: "{app}\data\obs-plugins\sports-replay\locale"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{app}\data\obs-plugins\sports-replay"

[Code]
{ Detect the OBS Studio install folder from the registry, falling back to the
  default Program Files location. }
function GetOBSDir(Param: String): String;
var
  Path: String;
begin
  Path := '';
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Path) and (Path <> '') then
    Result := Path
  else if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Path) and (Path <> '') then
    Result := Path
  else
    Result := ExpandConstant('{commonpf}\obs-studio');
end;

{ OBS up to 32.1 ships FFmpeg 7 (avcodec-61), OBS 32.2 and newer FFmpeg 8
  (avcodec-62). }
function OBSUsesFFmpeg8: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\bin\64bit\avcodec-62.dll'));
end;

function OBSUsesFFmpeg7: Boolean;
begin
  Result := (not OBSUsesFFmpeg8) and FileExists(ExpandConstant('{app}\bin\64bit\avcodec-61.dll'));
end;

function OBSFFmpegKnown: Boolean;
begin
  Result := OBSUsesFFmpeg7 or OBSUsesFFmpeg8;
end;

{ Check the chosen folder is an OBS install the plugin can load in. }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if not FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) then
    begin
      if SuppressibleMsgBox(CustomMessage('NoOBS'), mbConfirmation, MB_YESNO, IDNO) = IDNO then
        Result := False;
    end
    else if not OBSFFmpegKnown then
    begin
      SuppressibleMsgBox(CustomMessage('UnknownFFmpeg'), mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

{ A silent install skips the folder page: refuse there too rather than leave
  a plugin that cannot load. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) and not OBSFFmpegKnown then
    Result := CustomMessage('UnknownFFmpeg');
end;
