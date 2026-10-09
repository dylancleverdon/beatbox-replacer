; Inno Setup 6 script for the Windows installer (BeatboxReplacer-Setup.exe).
;
; Installs the VST3 bundle into the common VST3 folder for all users and gives the Users group
; modify rights on the bundle, so the in-plugin updater (Source/plugin/Updater.h) can replace
; files in it without admin rights. {app} only holds the uninstaller and a readme.
;
; CI: iscc /DAppVersion=1.0.42 /DBundleDir=<path\to\BeatboxReplacer.vst3> /O<output dir> BeatboxReplacer.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#ifndef BundleDir
  #define BundleDir SourcePath + "..\..\build\BeatboxReplacer_artefacts\Release\VST3\BeatboxReplacer.vst3"
#endif

#if !FileExists(AddBackslash(BundleDir) + "Contents\x86_64-win\BeatboxReplacer.vst3")
  #error "BundleDir does not contain Contents\x86_64-win\BeatboxReplacer.vst3 -- pass /DBundleDir=<path to BeatboxReplacer.vst3>"
#endif

#define VST3Dir "{commoncf64}\VST3\BeatboxReplacer.vst3"

[Setup]
; Never change AppId: it ties upgrades and the uninstaller to earlier installs.
AppId={{E43AF074-7782-44D7-935A-B317C7FA2858}
AppName=Beatbox Replacer
AppVersion={#AppVersion}
AppVerName=Beatbox Replacer {#AppVersion}
AppPublisher=Dylan Cleverdon
AppPublisherURL=https://github.com/dylancleverdon/beatbox-replacer
AppSupportURL=https://github.com/dylancleverdon/beatbox-replacer/issues
AppUpdatesURL=https://github.com/dylancleverdon/beatbox-replacer/releases/latest
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Beatbox Replacer
VersionInfoDescription=Beatbox Replacer Setup
DefaultDirName={commonpf64}\Beatbox Replacer
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableReadyPage=no
DisableFinishedPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\output
OutputBaseFilename=BeatboxReplacer-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Restart Manager: if Live has the plug-in loaded, offer to close it. The default filter
; (*.exe,*.dll,*.chm) would miss the plug-in DLL, which is named *.vst3.
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3
RestartApplications=no
UninstallDisplayName=Beatbox Replacer
SetupLogging=yes

[Dirs]
; Users may modify the bundle (inherited by everything created inside it later).
Name: "{#VST3Dir}"; Permissions: users-modify
Name: "{#VST3Dir}\Contents"; Permissions: users-modify
Name: "{#VST3Dir}\Contents\x86_64-win"; Permissions: users-modify
Name: "{#VST3Dir}\Contents\Resources"; Permissions: users-modify

[InstallDelete]
; Leftovers of in-plugin updates; files that are still loaded are skipped.
Type: files; Name: "{#VST3Dir}\Contents\x86_64-win\*.old-*"
Type: files; Name: "{#VST3Dir}\Contents\x86_64-win\*.new"
Type: files; Name: "{#VST3Dir}\Contents\Resources\*.old-*"
Type: files; Name: "{#VST3Dir}\Contents\Resources\*.new"

[Files]
Source: "{#BundleDir}\*"; DestDir: "{#VST3Dir}"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete; Permissions: users-modify
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
; Also removes files the updater added after installation.
Type: filesandordirs; Name: "{#VST3Dir}"

[Messages]
WelcomeLabel2=This will install the Beatbox Replacer VST3 plug-in ([name/ver]) for all users, in the common VST3 folder (Program Files\Common Files\VST3).%n%nIf Ableton Live is running with the plug-in loaded, Setup will offer to close it.
FinishedLabelNoIcons=Setup has finished installing [name].%n%nOpen Ableton, rescan plug-ins, and load BeatboxReplacer on a MIDI track.
FinishedLabel=Setup has finished installing [name].%n%nOpen Ableton, rescan plug-ins, and load BeatboxReplacer on a MIDI track.

[Code]
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'VST3 plug-in:' + NewLine + Space + ExpandConstant('{#VST3Dir}') + NewLine + NewLine +
            'Uninstaller and readme:' + NewLine + Space + ExpandConstant('{app}');
end;
