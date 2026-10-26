; HEARASIDE installer (prompt 5D). Built by build-installer.ps1, which passes the /D values below.
; One .exe: VST3 + VST2 (when the build has it) + HEARASIDE for OBS + cloudflared.
; AppId never changes, so a newer installer upgrades the old one in place.

#ifndef AppVersion
  #define AppVersion "1.0.10"
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
english.CompKaraoke=HAS Karaoke (standalone room client)
english.TypeFull=Full studio suite (VST3, VST2, OBS, Karaoke, cloudflared)
english.TypeKaraoke=HAS Karaoke only (standalone karaoke app for friends)
english.TypeCustom=Custom installation
english.NextKaraoke=Launch HAS Karaoke and paste a room link to join your friends!
english.OpenKaraoke=Open HAS Karaoke now
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
english.CompStreamlabs=HEARASIDE for Streamlabs Desktop (changes Streamlabs; a Streamlabs update removes it)
english.CloseStreamlabs=Close Streamlabs Desktop, then click Back and Next to go on.
english.StreamlabsFailed=HEARASIDE could not be added to Streamlabs Desktop:
english.TaskStreamlabsRepair=Put HEARASIDE back into Streamlabs Desktop after Streamlabs updates (a hidden check every 15 minutes)
english.StreamlabsUninstallFailed=Streamlabs Desktop could not be restored (close it, then uninstall HEARASIDE again):
thai.CompVst3=HEARASIDE VST3 (Hub, Track, App Audio)
thai.CompVst2=HEARASIDE VST2 (Hub, Track, App Audio)
thai.CompObs=HEARASIDE สำหรับ OBS (source และ control dock)
thai.CompCloudflared=cloudflared (ลิงก์ผ่านอินเทอร์เน็ต)
thai.CompKaraoke=HAS Karaoke (แอปคาราโอเกะเข้าห้องเดี่ยว)
thai.TypeFull=ครบชุดสตูดิโอ (VST3, VST2, OBS, Karaoke, cloudflared)
thai.TypeKaraoke=เฉพาะโปรแกรมคาราโอเกะ (HAS Karaoke สำหรับเพื่อน)
thai.TypeCustom=กำหนดเอง
thai.NextKaraoke=เปิด HAS Karaoke แล้ววางลิงก์ห้องเพื่อร่วมร้องกับเพื่อนได้เลย!
thai.OpenKaraoke=เปิด HAS Karaoke ตอนนี้
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
thai.CompStreamlabs=HEARASIDE สำหรับ Streamlabs Desktop (แก้ไข Streamlabs และจะหายไปเมื่อ Streamlabs อัปเดต)
thai.CloseStreamlabs=ปิด Streamlabs Desktop ก่อน แล้วกด ย้อนกลับ และ ถัดไป เพื่อติดตั้งต่อ
thai.StreamlabsFailed=เพิ่ม HEARASIDE เข้า Streamlabs Desktop ไม่สำเร็จ:
thai.TaskStreamlabsRepair=ใส่ HEARASIDE กลับเข้า Streamlabs Desktop เองหลัง Streamlabs อัปเดต (เช็กเงียบๆ ทุก 15 นาที)
thai.StreamlabsUninstallFailed=คืนค่า Streamlabs Desktop ไม่สำเร็จ (ปิด Streamlabs แล้วถอนการติดตั้ง HEARASIDE อีกครั้ง):

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "karaoke"; Description: "{cm:TypeKaraoke}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "vst3"; Description: "{cm:CompVst3}"; Types: full
#ifdef WithVst2
Name: "vst2"; Description: "{cm:CompVst2}"; Types: full
#endif
Name: "obs"; Description: "{cm:CompObs}"; Types: full
Name: "karaoke"; Description: "{cm:CompKaraoke}"; Types: full karaoke
Name: "cloudflared"; Description: "{cm:CompCloudflared}"; Types: full karaoke
#ifdef WithStreamlabs
; opt-in: it patches Streamlabs itself (hearaside-streamlabs.exe); only offered for the Streamlabs this build fits
Name: "streamlabs"; Description: "{cm:CompStreamlabs}"; Check: StreamlabsSupported
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
#ifdef WithStreamlabs
Name: "streamlabsrepair"; Description: "{cm:TaskStreamlabsRepair}"; Components: streamlabs
#endif

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
; HAS Karaoke (.exe)
Source: "{#BuildDir}\apps\karaoke\HearasideKaraoke_artefacts\Release\HAS Karaoke.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: karaoke
; OBS (same place install.ps1 uses)
Source: "{#BuildDir}\obs\hearaside-obs\hearaside-obs.dll"; DestDir: "{commonappdata}\obs-studio\plugins\hearaside-obs\bin\64bit"; Flags: ignoreversion; Components: obs
; cloudflared (pinned and hash-checked at build time, never committed)
Source: "{#Cloudflared}"; DestDir: "{app}\cloudflared"; DestName: "cloudflared.exe"; Flags: ignoreversion; Components: cloudflared
Source: "{#CloudflaredLicense}"; DestDir: "{app}\cloudflared"; DestName: "LICENSE.txt"; Flags: ignoreversion; Components: cloudflared
Source: "{#RootDir}\installer\windows\licenses.txt"; DestDir: "{app}"; DestName: "licenses.txt"; Flags: ignoreversion
#ifdef WithStreamlabs
; Streamlabs Desktop: the OBS source (it fills Streamlabs' libobs layout itself) and the tool that patches Streamlabs
Source: "{#StreamlabsTool}"; DestDir: "{app}\streamlabs"; Flags: ignoreversion; Components: streamlabs
Source: "{#StreamlabsDll}"; DestDir: "{app}\streamlabs"; Flags: ignoreversion; Components: streamlabs
; also extracted to {tmp} at start-up to check the installed Streamlabs
Source: "{#StreamlabsTool}"; Flags: dontcopy
#endif

[Icons]
Name: "{autoprograms}\HEARASIDE\HAS Karaoke"; Filename: "{app}\HAS Karaoke.exe"; Components: karaoke
Name: "{autodesktop}\HAS Karaoke"; Filename: "{app}\HAS Karaoke.exe"; Components: karaoke; Tasks: desktopicon

[Registry]
; remember the VST2 folder for upgrades and the uninstaller
Root: HKLM; Subkey: "Software\HEARASIDE"; ValueType: string; ValueName: "Vst2Dir"; ValueData: "{code:Vst2Dir}"; Flags: uninsdeletekey; Components: vst2
Root: HKLM; Subkey: "Software\HEARASIDE"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"; Flags: uninsdeletekey
; deep link protocol handler has://join?u=...
Root: HKCR; Subkey: "has"; ValueType: string; ValueData: "URL:HEARASIDE Protocol"; Flags: uninsdeletekey; Components: karaoke
Root: HKCR; Subkey: "has"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Flags: uninsdeletekey; Components: karaoke
Root: HKCR; Subkey: "has\DefaultIcon"; ValueType: string; ValueData: "{app}\HAS Karaoke.exe,0"; Flags: uninsdeletekey; Components: karaoke
Root: HKCR; Subkey: "has\shell\open\command"; ValueType: string; ValueData: """{app}\HAS Karaoke.exe"" ""%1"""; Flags: uninsdeletekey; Components: karaoke
; deep link protocol handler hearaside://join?url=...
Root: HKCR; Subkey: "hearaside"; ValueType: string; ValueData: "URL:HEARASIDE Protocol"; Flags: uninsdeletekey; Components: karaoke
Root: HKCR; Subkey: "hearaside"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Flags: uninsdeletekey; Components: karaoke
Root: HKCR; Subkey: "hearaside\DefaultIcon"; ValueType: string; ValueData: "{app}\HAS Karaoke.exe,0"; Flags: uninsdeletekey; Components: karaoke
Root: HKCR; Subkey: "hearaside\shell\open\command"; ValueType: string; ValueData: """{app}\HAS Karaoke.exe"" ""%1"""; Flags: uninsdeletekey; Components: karaoke

[Run]
Filename: "{app}\HAS Karaoke.exe"; Description: "{cm:OpenKaraoke}"; Flags: postinstall nowait skipifsilent unchecked; Components: karaoke
Filename: "{code:ObsExe}"; Description: "{cm:OpenObs}"; Flags: postinstall nowait skipifsilent unchecked; Check: ObsInstalled; Components: obs
Filename: "https://hearaside.netlify.app/guides/getting-started"; Description: "{cm:OpenGuide}"; Flags: postinstall shellexec nowait skipifsilent unchecked

[Code]
var
  Vst2Page: TInputDirWizardPage;
  StreamlabsChecked: Boolean;
  StreamlabsCheckCode: Integer;

function StreamlabsDir: string;
begin
  Result := ExpandConstant('{commonpf64}\Streamlabs OBS');
end;

// hearaside-streamlabs.exe check: 0 = fits, 2 = fits but running, 3 = other libobs, 4 = not installed
function StreamlabsCheck: Integer;
begin
  Result := 4;
#ifdef WithStreamlabs
  if not FileExists(StreamlabsDir + '\Streamlabs OBS.exe') then exit;
  ExtractTemporaryFile('hearaside-streamlabs.exe');
  if not Exec(ExpandConstant('{tmp}\hearaside-streamlabs.exe'), 'check "' + StreamlabsDir + '"', '', SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := 4;
#endif
end;

function StreamlabsSupported: Boolean;
begin
  if not StreamlabsChecked then
  begin
    StreamlabsCheckCode := StreamlabsCheck;
    StreamlabsChecked := True;
  end;
  Result := (StreamlabsCheckCode = 0) or (StreamlabsCheckCode = 2);
end;

const
  RepairTask = '\HEARASIDE\Streamlabs repair';

// the scheduled task that runs "hearaside-streamlabs repair" at start-up and every 15 minutes, hidden, as SYSTEM
procedure SetStreamlabsRepairTask(Wanted: Boolean);
var
  Script, Exe, Dll: string;
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN "' + RepairTask + '" /F', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if not Wanted then exit;
  Exe := ExpandConstant('{app}\streamlabs\hearaside-streamlabs.exe');
  Dll := ExpandConstant('{app}\streamlabs\hearaside-obs.dll');
  Script := ExpandConstant('{tmp}\streamlabs-repair-task.ps1');
  SaveStringToFile(Script,
    '$a = New-ScheduledTaskAction -Execute ''' + Exe + ''' -Argument ''repair "' + StreamlabsDir + '" "' + Dll + '"''' + #13#10 +
    '$t = @((New-ScheduledTaskTrigger -AtStartup), (New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 15)))' + #13#10 +
    '$p = New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest' + #13#10 +
    '$s = New-ScheduledTaskSettingsSet -Hidden -Priority 7 -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -StartWhenAvailable ' +
        '-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew' + #13#10 +
    'Register-ScheduledTask -TaskPath ''\HEARASIDE\'' -TaskName ''Streamlabs repair'' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null' + #13#10,
    False);
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '"',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
#ifdef WithStreamlabs
  if WizardIsComponentSelected('streamlabs') and (StreamlabsCheck = 2) then
    Result := CustomMessage('CloseStreamlabs');
#endif
end;

// Windows Firewall: the Hub's TCP ports (pages, WebSocket) and its WebRTC UDP ports (libs/ssnet rtc.h
// kHubUdpFirst..kHubUdpLast), only from the local network and Radmin VPN (26.0.0.0/8)
procedure SetHubFirewall(Wanted: Boolean);
var
  Netsh: string;
  Code: Integer;
begin
  Netsh := ExpandConstant('{sys}\netsh.exe');
  Exec(Netsh, 'advfirewall firewall delete rule name="HEARASIDE Hub (TCP)"', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(Netsh, 'advfirewall firewall delete rule name="HEARASIDE Hub (UDP)"', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if not Wanted then exit;
  Exec(Netsh, 'advfirewall firewall add rule name="HEARASIDE Hub (TCP)" dir=in action=allow protocol=TCP localport=47810-47829 remoteip=localsubnet,26.0.0.0/8 profile=any',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(Netsh, 'advfirewall firewall add rule name="HEARASIDE Hub (UDP)" dir=in action=allow protocol=UDP localport=47830-47999 remoteip=localsubnet,26.0.0.0/8 profile=any',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  // friends on the same network or in Radmin VPN connect straight to the Hub
  if CurStep = ssPostInstall then
#ifdef WithVst2
    SetHubFirewall(WizardIsComponentSelected('vst3') or WizardIsComponentSelected('vst2'));
#else
    SetHubFirewall(WizardIsComponentSelected('vst3'));
#endif
#ifdef WithStreamlabs
  if (CurStep = ssPostInstall) and WizardIsComponentSelected('streamlabs') then
    if not Exec(ExpandConstant('{app}\streamlabs\hearaside-streamlabs.exe'),
                'install "' + StreamlabsDir + '" "' + ExpandConstant('{app}\streamlabs\hearaside-obs.dll') + '"',
                '', SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
      SuppressibleMsgBox(CustomMessage('StreamlabsFailed') + ' ' + IntToStr(Code), mbError, MB_OK, IDOK);
  if CurStep = ssPostInstall then
    SetStreamlabsRepairTask(WizardIsComponentSelected('streamlabs') and WizardIsTaskSelected('streamlabsrepair'));
#endif
end;

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
  begin
    if WizardIsComponentSelected('karaoke') and not WizardIsComponentSelected('vst3') then
      WizardForm.FinishedLabel.Caption :=
        CustomMessage('ReadyTitle') + #13#10#13#10 +
        CustomMessage('NextKaraoke')
    else
      WizardForm.FinishedLabel.Caption :=
        CustomMessage('ReadyTitle') + #13#10#13#10 +
        '1. ' + CustomMessage('Next1') + #13#10 +
        '2. ' + CustomMessage('Next2') + #13#10 +
        '3. ' + CustomMessage('Next3');
  end;
end;

// --- uninstall: settings are optional, recordings are never touched ---
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Vst2: string;
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    SetHubFirewall(False);
    // put Streamlabs back before its tool is removed, and stop the task that would patch it again
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN "\HEARASIDE\Streamlabs repair" /F', '', SW_HIDE, ewWaitUntilTerminated, Code);
    if FileExists(ExpandConstant('{app}\streamlabs\hearaside-streamlabs.exe')) then
      if not Exec(ExpandConstant('{app}\streamlabs\hearaside-streamlabs.exe'), 'uninstall "' + StreamlabsDir + '"',
                  '', SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
        if not UninstallSilent then
          MsgBox(CustomMessage('StreamlabsUninstallFailed') + ' ' + IntToStr(Code), mbError, MB_OK);
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
