; HEARASIDE installer (prompt 5D). Built by build-installer.ps1, which passes the /D values below.
; One .exe: VST3 + VST2 (when the build has it) + HEARASIDE for OBS + cloudflared.
; AppId never changes, so a newer installer upgrades the old one in place.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BuildDir
  #error BuildDir is required (use build-installer.ps1)
#endif

[Setup]
AppId={{8F2C5B1E-4D7A-4C39-9E6B-3A1D7F0C2B84}
AppName=HEARASIDE
AppVersion={#AppVersion}
AppPublisher=HEARASIDE
DefaultDirName={autopf64}\HEARASIDE
DefaultGroupName=HEARASIDE
DisableProgramGroupPage=yes
DisableDirPage=auto
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
OutputDir={#OutDir}
OutputBaseFilename=HEARASIDE-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=HEARASIDE
LicenseFile={#RootDir}\installer\windows\licenses.txt
; close DAWs / OBS that hold the plug-in files: Inno lists them and asks, it never closes them silently
CloseApplications=yes
RestartApplications=no
CloseApplicationsFilter=*.dll,*.vst3,*.exe
ShowLanguageDialog=auto
UsePreviousLanguage=no
#ifdef Signed
SignTool=signtool $f
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "thai"; MessagesFile: "compiler:Languages\Thai.isl"

[CustomMessages]
english.CompVst3=HEARASIDE VST3 (Hub, Track, App Audio)
english.CompVst2=HEARASIDE VST2 (Hub, Track, App Audio)
english.CompObs=HEARASIDE for OBS (source and control dock)
english.CompCloudflared=cloudflared (links over the internet)
english.Vst2Page=VST2 folder
english.Vst2PageText=Where should the VST2 plug-ins go?
english.Vst2PageHint=Choose the folder your DAW scans for VST2 plug-ins. A HEARASIDE folder is made inside it.
english.NoObs=OBS Studio wasn’t found. HEARASIDE for OBS will work once you install OBS.
english.ReadyTitle=HEARASIDE is ready
english.Next1=Rescan plug-ins in your DAW
english.Next2=Put HEARASIDE Hub on your master, HEARASIDE Track at the end of each track
english.Next3=In OBS, add a source called HEARASIDE
english.OpenObs=Open OBS now
english.OpenGuide=Open the getting started guide
english.RemoveSettings=Also remove my HEARASIDE settings (your recordings are never removed)
thai.CompVst3=HEARASIDE VST3 (Hub, Track, App Audio)
thai.CompVst2=HEARASIDE VST2 (Hub, Track, App Audio)
thai.CompObs=HEARASIDE สำหรับ OBS (source และ control dock)
thai.CompCloudflared=cloudflared (ลิงก์ผ่านอินเทอร์เน็ต)
thai.Vst2Page=โฟลเดอร์ VST2
thai.Vst2PageText=จะวางปลั๊กอิน VST2 ไว้ที่ไหน
thai.Vst2PageHint=เลือกโฟลเดอร์ที่ DAW ใช้สแกนปลั๊กอิน VST2 ตัวติดตั้งจะสร้างโฟลเดอร์ HEARASIDE ไว้ข้างใน
thai.NoObs=ไม่พบ OBS Studio HEARASIDE สำหรับ OBS จะใช้งานได้เมื่อคุณติดตั้ง OBS
thai.ReadyTitle=HEARASIDE พร้อมใช้งานแล้ว
thai.Next1=สแกนปลั๊กอินใหม่ใน DAW
thai.Next2=ใส่ HEARASIDE Hub บน master และ HEARASIDE Track ที่ท้ายทุกแทร็ก
thai.Next3=ใน OBS เพิ่ม source ชื่อ HEARASIDE
thai.OpenObs=เปิด OBS ตอนนี้
thai.OpenGuide=เปิดคู่มือเริ่มต้นใช้งาน
thai.RemoveSettings=ลบการตั้งค่า HEARASIDE ของฉันด้วย (ไฟล์อัดเสียงจะไม่ถูกลบ)

[Types]
Name: "full"; Description: "Full"

[Components]
Name: "vst3"; Description: "{cm:CompVst3}"; Types: full; Flags: fixed
#ifdef WithVst2
Name: "vst2"; Description: "{cm:CompVst2}"; Types: full; Flags: fixed
#endif
Name: "obs"; Description: "{cm:CompObs}"; Types: full; Flags: fixed
Name: "cloudflared"; Description: "{cm:CompCloudflared}"; Types: full; Flags: fixed

[Dirs]
Name: "{commoncf64}\VST3\HEARASIDE"; Components: vst3

[Files]
; VST3 bundles
Source: "{#BuildDir}\plugins\HearasideHub_artefacts\Release\VST3\HEARASIDE Hub.vst3\*"; DestDir: "{commoncf64}\VST3\HEARASIDE\HEARASIDE Hub.vst3"; Flags: recursesubdirs ignoreversion; Components: vst3
Source: "{#BuildDir}\plugins\HearasideTrack_artefacts\Release\VST3\HEARASIDE Track.vst3\*"; DestDir: "{commoncf64}\VST3\HEARASIDE\HEARASIDE Track.vst3"; Flags: recursesubdirs ignoreversion; Components: vst3
Source: "{#BuildDir}\plugins\HearasideAppAudio_artefacts\Release\VST3\HEARASIDE App Audio.vst3\*"; DestDir: "{commoncf64}\VST3\HEARASIDE\HEARASIDE App Audio.vst3"; Flags: recursesubdirs ignoreversion; Components: vst3
Source: "{#RootDir}\external\fonts\OFL.txt"; DestDir: "{commoncf64}\VST3\HEARASIDE"; DestName: "Anuphan-OFL.txt"; Flags: ignoreversion skipifsourcedoesntexist; Components: vst3
#ifdef WithVst2
Source: "{#BuildDir}\plugins\HearasideHub_artefacts\Release\VST\HEARASIDE Hub.dll"; DestDir: "{code:Vst2Dir}\HEARASIDE"; Flags: ignoreversion; Components: vst2
Source: "{#BuildDir}\plugins\HearasideTrack_artefacts\Release\VST\HEARASIDE Track.dll"; DestDir: "{code:Vst2Dir}\HEARASIDE"; Flags: ignoreversion; Components: vst2
Source: "{#BuildDir}\plugins\HearasideAppAudio_artefacts\Release\VST\HEARASIDE App Audio.dll"; DestDir: "{code:Vst2Dir}\HEARASIDE"; Flags: ignoreversion; Components: vst2
#endif
; OBS (same place install.ps1 uses)
Source: "{#BuildDir}\obs\hearaside-obs\hearaside-obs.dll"; DestDir: "{commonappdata}\obs-studio\plugins\hearaside-obs\bin\64bit"; Flags: ignoreversion; Components: obs
; cloudflared (pinned and hash-checked at build time, never committed)
Source: "{#Cloudflared}"; DestDir: "{app}\cloudflared"; DestName: "cloudflared.exe"; Flags: ignoreversion; Components: cloudflared
Source: "{#CloudflaredLicense}"; DestDir: "{app}\cloudflared"; DestName: "LICENSE.txt"; Flags: ignoreversion; Components: cloudflared
Source: "{#RootDir}\installer\windows\licenses.txt"; DestDir: "{app}"; DestName: "licenses.txt"; Flags: ignoreversion

[Registry]
; remember the VST2 folder for upgrades and the uninstaller
Root: HKLM; Subkey: "Software\HEARASIDE"; ValueType: string; ValueName: "Vst2Dir"; ValueData: "{code:Vst2Dir}"; Flags: uninsdeletekey; Components: vst2
Root: HKLM; Subkey: "Software\HEARASIDE"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"; Flags: uninsdeletekey

[Run]
Filename: "{code:ObsExe}"; Description: "{cm:OpenObs}"; Flags: postinstall nowait skipifsilent unchecked; Check: ObsInstalled
Filename: "https://hearaside.vercel.app/guides/getting-started"; Description: "{cm:OpenGuide}"; Flags: postinstall shellexec nowait skipifsilent unchecked

[Code]
var
  Vst2Page: TInputDirWizardPage;

function ObsExe(Param: string): string;
var
  Dir: string;
begin
  Result := '';
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Dir) and FileExists(Dir + '\bin\64bit\obs64.exe') then
    Result := Dir + '\bin\64bit\obs64.exe'
  else if FileExists(ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe')) then
    Result := ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe');
end;

function ObsInstalled: Boolean;
begin
  Result := ObsExe('') <> '';
end;

function DefaultVst2Dir: string;
var
  Dir: string;
begin
  if RegQueryStringValue(HKLM64, 'SOFTWARE\VST', 'VSTPluginsPath', Dir) and (Dir <> '') then
    Result := Dir
  else if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\VST', 'VSTPluginsPath', Dir) and (Dir <> '') then
    Result := Dir
  else
    Result := ExpandConstant('{commonpf64}\VSTPlugins');
end;

function Vst2Dir(Param: string): string;
var
  Saved: string;
begin
  if Assigned(Vst2Page) then
    Result := Vst2Page.Values[0]
  else if RegQueryStringValue(HKLM, 'Software\HEARASIDE', 'Vst2Dir', Saved) then
    Result := Saved
  else
    Result := DefaultVst2Dir;
end;

procedure InitializeWizard;
begin
  Vst2Page := CreateInputDirPage(wpSelectComponents, CustomMessage('Vst2Page'), CustomMessage('Vst2PageText'),
    CustomMessage('Vst2PageHint'), False, '');
  Vst2Page.Add('');
  Vst2Page.Values[0] := DefaultVst2Dir;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if Assigned(Vst2Page) and (PageID = Vst2Page.ID) then
    Result := not WizardIsComponentSelected('vst2');
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoComponentsInfo;
  if WizardIsComponentSelected('obs') and not ObsInstalled then
    Result := Result + NewLine + NewLine + CustomMessage('NoObs');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      CustomMessage('ReadyTitle') + #13#10#13#10 +
      '1. ' + CustomMessage('Next1') + #13#10 +
      '2. ' + CustomMessage('Next2') + #13#10 +
      '3. ' + CustomMessage('Next3');
end;

// --- uninstall: settings are optional, recordings are never touched ---
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Vst2: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    // the VST2 folder was chosen at install time
    if RegQueryStringValue(HKLM, 'Software\HEARASIDE', 'Vst2Dir', Vst2) then
      DelTree(AddBackslash(Vst2) + 'HEARASIDE', True, True, True);
    DelTree(ExpandConstant('{commoncf64}\VST3\HEARASIDE'), True, True, True);
    DelTree(ExpandConstant('{commonappdata}\obs-studio\plugins\hearaside-obs'), True, True, True);
  end;
  if CurUninstallStep = usPostUninstall then
    if not UninstallSilent then
      if MsgBox(CustomMessage('RemoveSettings'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{userappdata}\HEARASIDE'), True, True, True);
end;
