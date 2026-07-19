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

[Files]
Source: "sports-replay\bin\64bit\sports-replay.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "sports-replay\data\locale\*"; DestDir: "{app}\data\obs-plugins\sports-replay\locale"; Flags: ignoreversion recursesubdirs createallsubdirs

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

{ Warn if the chosen folder doesn't look like an OBS install. }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if not FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) then
    begin
      if MsgBox('No se encontró OBS Studio en esa carpeta (falta bin\64bit\obs64.exe).' + #13#10 +
                'Elegí la carpeta donde está instalado OBS Studio.' + #13#10#13#10 +
                '¿Continuar de todos modos?', mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
    end;
  end;
end;
