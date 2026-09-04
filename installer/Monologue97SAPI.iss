; Monologue97SAPI.iss - installer for the Monologue '97 SAPI5 voice.
;
; Installs the 1997 First Byte / PrimoVOX engine, the 32-bit speech helper, the
; SAPI5 engine dll for both architectures and the configuration utility, then
; registers 21 voices plus a Custom Voice with SAPI.
;
; Build:  ISCC.exe installer\Monologue97SAPI.iss
; Expects the staged tree in output\ (see build_all.bat).

#define AppName        "Monologue 97 SAPI5"
#define AppVersion     "1.4.1"
#define AppPublisher   "Josh Kennedy"
#define AppURL         "https://github.com/joshknnd1982/monologue97-sapi5"
#define ConfigExe      "Monologue97Config.exe"

[Setup]
AppId={{2C7E4B91-5A38-4D62-9F13-6E8A0B4C7D25}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Monologue97
DefaultGroupName=Monologue 97
; The voices are registered under HKLM so every SAPI host sees them, which
; needs administrator rights. SAPI's voice category resolves to an HKLM path,
; so a per-user install would register cleanly and then never be enumerated.
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=Monologue97-SAPI5-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#ConfigExe}
LicenseFile=..\LICENSE.txt
; Always write a setup log; CopyLogToApp below keeps a copy next to the other
; logs so a user can hand over one folder when something goes wrong.
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut for the Monologue 97 configuration utility"; GroupDescription: "Additional shortcuts:"

[Files]
; --- the 1997 engine ---
Source: "..\output\engine\*"; DestDir: "{app}\engine"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- helper, configuration utility and the 32-bit SAPI dll ---
Source: "..\output\mono_host.exe";           DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "..\output\{#ConfigExe}";            DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "..\output\monologue_sapi_x86.dll";  DestDir: "{app}"; Flags: ignoreversion restartreplace

; --- the 64-bit SAPI dll, only where a 64-bit host can use it ---
Source: "..\output\x64\monologue_sapi_x64.dll"; DestDir: "{app}\x64"; Flags: ignoreversion restartreplace; Check: Is64BitInstallMode

; --- documentation ---
Source: "..\README.md";      DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion
Source: "..\LICENSE.txt";    DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\Monologue 97 Speech Settings"; Filename: "{app}\{#ConfigExe}"
; No shortcut to the log folder: setup runs elevated, so {localappdata} here
; would be the administrator's profile rather than the person who will actually
; use the voice. The configuration utility's "Open log folder" button resolves
; the path at run time, for whoever is running it.
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Monologue 97 Speech Settings"; Filename: "{app}\{#ConfigExe}"; Tasks: desktopicon

[Run]
; regsvr32 is invoked per architecture rather than with Inno's regserver flag:
; the 32-bit dll must land in the 32-bit COM view and the 64-bit dll in the
; native one, and a single regserver would use only the installer's own
; bitness. On 32-bit Windows {syswow64} maps to System32, so this is correct
; there too.
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\monologue_sapi_x86.dll"""; StatusMsg: "Registering the 32-bit voices..."; Flags: runhidden waituntilterminated
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\x64\monologue_sapi_x64.dll"""; StatusMsg: "Registering the 64-bit voices..."; Flags: runhidden waituntilterminated; Check: Is64BitInstallMode
Filename: "{app}\{#ConfigExe}"; Description: "Open the Monologue 97 &configuration utility"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\monologue_sapi_x86.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregX86"
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\x64\monologue_sapi_x64.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregX64"; Check: Is64BitInstallMode

[UninstallDelete]
Type: filesandordirs; Name: "{app}\engine"
Type: dirifempty; Name: "{app}"

[Code]
const
  MonoHostMutex = 'Local\Monologue97HostMutex';

// The helper stays alive while any SAPI host is using a voice, and it holds
// engine\mnvox11.dll open. Ask it to exit before replacing files, so an
// upgrade does not need a reboot.
procedure StopHelper();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{cmd}'), '/c taskkill /f /im mono_host.exe',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopHelper();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopHelper();
  Result := True;
end;

// Keep a copy of the setup log in the install directory. It cannot go to
// {localappdata}: setup is elevated, so that would be the administrator's
// profile, not the profile of the person who will use the voice.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then
    CopyFile(ExpandConstant('{log}'),
             ExpandConstant('{app}\install-{#AppVersion}.log'), False);
end;
