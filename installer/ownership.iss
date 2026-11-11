const
  ProductOwner = 'RVCFEATUREINJECTOR-OWNED-INSTALL-V1';
  LegacyUserUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\RvcAudioLab.RvcFeatureInjector.User_is1';
  ReparsePoint = $400;
var
  OldManifest: TStringList;
  UninstallStateDirs: TStringList;
  OldManifestBytes: AnsiString;
  OldCleanupHelperBytes: AnsiString;
  HadOldManifest, HadOldCleanupHelper, InstallationStarted, InstallationCommitted, InstallSucceeded: Boolean;
  Vst3DirPage: TInputDirWizardPage;
  UninstallPluginDir, LastAppPath, RequestedVst3Parent: String;
  UninstallCleanupHelper: String;
  UninstallCleanupHandle: THandle;
  RuntimeChecksEnabled: Boolean;
  CheckedRuntimeFiles, UnchangedRuntimeFiles: TStringList;

function GetFileAttributesW(Name: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function CreateFileW(Name: String; Access, Share: Cardinal; Security: NativeInt;
  Disposition, Attributes: Cardinal; Template: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function GetCurrentProcessId: Cardinal;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function ExpandEnvironmentStringsW(Source, Buffer: String; Size: Cardinal): Cardinal;
  external 'ExpandEnvironmentStringsW@kernel32.dll stdcall';

function Canonical(const Path: String): String;
begin
  Result := RemoveBackslashUnlessRoot(ExpandFileName(Path));
end;

function DefaultVst3Parent: String;
begin
#ifdef TestRoot
  Result := '{#TestRoot}\VST3';
#else
  Result := ExpandConstant('{commoncf64}\VST3');
#endif
end;

function ManagedStateConfiguration: String;
begin
#ifdef TestRoot
  Result := ExpandConstant('{app}\state');
#else
  { Resolve this token in the Cubase process, never in the elevated installer. }
  Result := '%LOCALAPPDATA%\Taketo\RvcFeatureInjector\state';
#endif
end;

function LegacyUserInstallExists: Boolean;
var Users: TArrayOfString; I: Integer;
begin
#ifdef TestRoot
  Result := False;
#else
  Result := RegKeyExists(HKCU64, LegacyUserUninstallKey) or RegKeyExists(HKCU32, LegacyUserUninstallKey);
  if Result then exit;
  { UAC may be running under a different administrator's credentials. Inspect
    loaded user hives read-only rather than assuming HKCU is the Cubase user. }
  if RegGetSubkeyNames(HKU64, '', Users) then
    for I := 0 to GetArrayLength(Users) - 1 do
      if Copy(Users[I], 1, 6) = 'S-1-5-' then
        if RegKeyExists(HKU64, Users[I] + '\' + LegacyUserUninstallKey) or
            RegKeyExists(HKU32, Users[I] + '\' + LegacyUserUninstallKey) then begin
          Result := True; exit;
        end;
#endif
end;

function InitializeSetup: Boolean;
begin
  Result := not LegacyUserInstallExists;
  if not Result then
    SuppressibleMsgBox('检测到旧版当前用户安装，请先通过 Windows 已安装的应用卸载旧版，然后重新运行本安装器 / A previous per-user installation was found. Uninstall it from Windows Installed apps before installing this machine-wide version.', mbError, MB_OK, IDOK);
end;

function GetPluginDir(Param: String): String;
begin
  if UninstallPluginDir <> '' then Result := UninstallPluginDir
  else if Vst3DirPage <> nil then Result := AddBackslash(Vst3DirPage.Values[0]) + 'RvcFeatureInjector.vst3'
  else Result := AddBackslash(DefaultVst3Parent) + 'RvcFeatureInjector.vst3';
end;

function AbsoluteDirectory(const Path: String): Boolean;
var I: Integer;
begin
  Result := ((Length(Path) >= 3) and (Copy(Path, 2, 2) = ':\')) or
    ((Length(Path) > 2) and (Copy(Path, 1, 2) = '\\'));
  Result := Result and (Pos('*', Path) = 0) and (Pos('?', Path) = 0) and
    (Pos('"', Path) = 0) and (Pos('<', Path) = 0) and (Pos('>', Path) = 0) and (Pos('|', Path) = 0);
  if Pos(':', Copy(Path, 3, Length(Path))) > 0 then Result := False;
  for I := 1 to Length(Path) do if Ord(Path[I]) < 32 then Result := False;
end;

function ValidVst3Parent(const Path: String): Boolean;
begin
  Result := AbsoluteDirectory(Path);
  if Result then Result := CompareText(ExtractFileExt(Canonical(Path)), '.vst3') <> 0;
end;

function ReadOwner(const FileName: String; var AppPath, PluginPath: String): Boolean;
var Lines: TStringList; Bytes: AnsiString;
begin
  Result := False;
  if not LoadStringFromFile(FileName, Bytes) then exit;
  Lines := TStringList.Create;
  try
    Lines.Text := Trim(UTF8Decode(Bytes));
    if Lines.Count <> 3 then exit;
    if (Lines[0] <> ProductOwner) or not AbsoluteDirectory(Lines[1]) or not AbsoluteDirectory(Lines[2]) then exit;
    AppPath := Canonical(Lines[1]);
    PluginPath := Canonical(Lines[2]);
    if CompareText(ExtractFileName(PluginPath), 'RvcFeatureInjector.vst3') <> 0 then exit;
    Result := True;
  finally Lines.Free; end;
end;

function OwnerPluginPath(const AppPath: String; var PluginPath: String): Boolean;
var OwnerApp: String;
begin
  Result := ReadOwner(AddBackslash(AppPath) + '.rvc-install-owner', OwnerApp, PluginPath);
  if Result then Result := CompareText(OwnerApp, Canonical(AppPath)) = 0;
end;

function InstalledPluginPath(const AppPath: String; var PluginPath: String): Boolean;
var Bytes: AnsiString; Config: String;
begin
  Result := OwnerPluginPath(AppPath, PluginPath);
  if not Result or not FileExists(AddBackslash(AppPath) + 'plugin-root.txt') then exit;
  Result := LoadStringFromFile(AddBackslash(AppPath) + 'plugin-root.txt', Bytes);
  if not Result then exit;
  Config := Trim(UTF8Decode(Bytes));
  if (Length(Config) > 0) and (Config[1] = #$FEFF) then Delete(Config, 1, 1);
  Result := AbsoluteDirectory(Config);
  if Result then Result := CompareText(Canonical(Config), PluginPath) = 0;
end;

procedure SelectPreviousVst3Parent;
var AppPath, PreviousPlugin: String;
begin
  AppPath := Canonical(WizardDirValue);
  if (CompareText(LastAppPath, AppPath) <> 0) and
      (ExpandConstant('{param:VST3DIR|}') = '') and OwnerPluginPath(AppPath, PreviousPlugin) then
    Vst3DirPage.Values[0] := ExtractFileDir(PreviousPlugin);
  LastAppPath := AppPath;
end;

procedure InitializeWizard;
begin
  WizardForm.SelectDirLabel.Caption := CustomMessage('RuntimeInstructions');
  WizardForm.SelectDirBrowseLabel.Caption := CustomMessage('RuntimeBrowse');
  Vst3DirPage := CreateInputDirPage(wpSelectDir,
    CustomMessage('VST3Title'), CustomMessage('VST3Description'),
    CustomMessage('VST3Instructions'), False, 'VST3');
  Vst3DirPage.Add(CustomMessage('VST3Folder'));
  RequestedVst3Parent := ExpandConstant('{param:VST3DIR|}');
  Vst3DirPage.Values[0] := RequestedVst3Parent;
  if Vst3DirPage.Values[0] = '' then Vst3DirPage.Values[0] := DefaultVst3Parent;
  SelectPreviousVst3Parent;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectDir then begin
    WizardForm.PageNameLabel.Caption := CustomMessage('RuntimeTitle');
    WizardForm.PageDescriptionLabel.Caption := CustomMessage('RuntimeDescription');
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then SelectPreviousVst3Parent;
  if CurPageID = Vst3DirPage.ID then begin
    Result := ValidVst3Parent(Vst3DirPage.Values[0]) and
      ((RequestedVst3Parent = '') or ValidVst3Parent(RequestedVst3Parent));
    if not Result then SuppressibleMsgBox('请选择完整的 VST3 父目录路径，不要选择 .vst3 插件包本身 / Select an absolute VST3 parent folder, not a .vst3 bundle.', mbError, MB_OK, IDOK);
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + CustomMessage('VST3Summary') + NewLine +
    Space + GetPluginDir('');
end;

function OwnerText: String;
begin
  Result := ProductOwner + #13#10 + Canonical(ExpandConstant('{app}')) + #13#10 + Canonical(GetPluginDir(''));
end;

function HasOwner(const FileName: String): Boolean;
var AppPath, PluginPath: String;
begin
  Result := ReadOwner(FileName, AppPath, PluginPath);
  if Result then Result := (CompareText(AppPath, Canonical(ExpandConstant('{app}'))) = 0) and
    (CompareText(PluginPath, Canonical(GetPluginDir(''))) = 0);
end;

function HasReparseAncestor(Path: String): Boolean;
var Previous: String; Attributes: Cardinal;
begin
  Result := False;
  Path := Canonical(Path);
  repeat
    Attributes := GetFileAttributesW(Path);
    if (Attributes <> $FFFFFFFF) and ((Attributes and ReparsePoint) <> 0) then begin Result := True; exit; end;
    Previous := Path;
    Path := RemoveBackslashUnlessRoot(ExtractFileDir(Path));
  until (Path = Previous) or (Path = '');
end;

function DirectoryEmpty(const Path: String): Boolean;
var Item: TFindRec;
begin
  Result := True;
  if FindFirst(AddBackslash(Path) + '*', Item) then begin
    try
      repeat
        if (Item.Name <> '.') and (Item.Name <> '..') then begin Result := False; exit; end;
      until not FindNext(Item);
    finally FindClose(Item); end;
  end;
end;

function IsActiveUninstallFile(const Path: String): Boolean;
var Uninstaller: String;
begin
  if IsUninstaller then Uninstaller := ExpandConstant('{uninstallexe}')
  else Uninstaller := ExpandConstant('{app}\unins000.exe');
  Result := (CompareText(Canonical(Path), Canonical(Uninstaller)) = 0) or
    (CompareText(Canonical(Path), Canonical(ChangeFileExt(Uninstaller, '.dat'))) = 0);
end;

function TreeSafeAndUnlocked(const Path: String; var Problem: String): Boolean;
var Item: TFindRec; Child: String; Handle: THandle;
begin
  Result := False;
  if HasReparseAncestor(Path) then begin Problem := '路径包含目录链接 / directory link: ' + Path; exit; end;
  if not DirExists(Path) then begin Result := True; exit; end;
  if FindFirst(AddBackslash(Path) + '*', Item) then begin
    try
      repeat
        if (Item.Name <> '.') and (Item.Name <> '..') then begin
          Child := AddBackslash(Path) + Item.Name;
          if (Item.Attributes and ReparsePoint) <> 0 then begin Problem := '路径包含目录链接 / directory link: ' + Child; exit; end;
          if (Item.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then begin
            if not TreeSafeAndUnlocked(Child, Problem) then exit;
          end else begin
            if not IsActiveUninstallFile(Child) then begin
              Handle := CreateFileW(Child, $40000000, 0, 0, 3, $80, 0);
              if Handle = THandle(-1) then begin
                Problem := '文件正在使用或不可写，请关闭 Cubase 后重试 / close the host: ' + Child; exit;
              end;
              CloseHandle(Handle);
            end;
          end;
        end;
      until not FindNext(Item);
    finally FindClose(Item); end;
  end;
  Result := True;
end;

function DirectoryWritable(Path: String; var Problem: String): Boolean;
var Previous: String; Handle: THandle;
begin
  Result := False;
  Path := Canonical(Path);
  while not DirExists(Path) do begin
    if FileExists(Path) then break;
    Previous := Path;
    Path := RemoveBackslashUnlessRoot(ExtractFileDir(Path));
    if (Path = Previous) or (Path = '') then break;
  end;
  if DirExists(Path) then begin
    Handle := CreateFileW(AddBackslash(Path) + '.taketo-rvc-write-probe-' + IntToStr(GetCurrentProcessId),
      $40000000, 0, 0, 1, $04000100, 0);
    if Handle <> THandle(-1) then begin CloseHandle(Handle); Result := True; exit; end;
  end;
  Problem := '目录不可写，请确认管理员权限及目标目录权限 / ' +
    'Folder is not writable. Check administrator access and target-folder permissions: ' + Path;
end;

function ValidateTargets(RequireOwner: Boolean): String;
var AppPath, PluginPath, Registered, PreviousPlugin: String;
begin
  Result := '';
  if not ValidVst3Parent(ExtractFileDir(GetPluginDir(''))) or
      ((not RequireOwner) and (RequestedVst3Parent <> '') and not ValidVst3Parent(RequestedVst3Parent)) then begin
    Result := '请选择完整的 VST3 父目录路径，不要选择 .vst3 插件包本身 / Select an absolute VST3 parent folder, not a .vst3 bundle.'; exit;
  end;
  AppPath := Canonical(ExpandConstant('{app}'));
  PluginPath := Canonical(GetPluginDir(''));
  if (CompareText(ExtractFileName(AppPath), 'RvcFeatureInjector') <> 0) then begin
    Result := '请选择以 RvcFeatureInjector 结尾的专属目录 / select a dedicated RvcFeatureInjector folder.'; exit;
  end;
  if (Pos(Lowercase(AddBackslash(AppPath)), Lowercase(AddBackslash(PluginPath))) = 1) or
     (Pos(Lowercase(AddBackslash(PluginPath)), Lowercase(AddBackslash(AppPath))) = 1) then begin
    Result := '插件目录和运行环境目录必须分开 / plugin and runtime directories must be separate.'; exit;
  end;
#ifndef TestRoot
  if RegQueryStringValue(HKLM64, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Taketo.RvcFeatureInjector.Machine_is1',
      'InstallLocation', Registered) and (CompareText(Canonical(Registered), AppPath) <> 0) then begin
    Result := '升级请使用原安装目录，或先卸载旧版 / upgrade in the existing directory or uninstall first.'; exit;
  end;
#endif
  if HasReparseAncestor(AppPath) or HasReparseAncestor(PluginPath) then begin
    Result := '安装目录不能包含目录链接 / installation paths must not contain directory links.'; exit;
  end;
  if FileExists(AddBackslash(AppPath) + '.rvc-install-owner') and OwnerPluginPath(AppPath, PreviousPlugin) then begin
    if not InstalledPluginPath(AppPath, PreviousPlugin) then begin
      Result := '插件路径记录不一致，请恢复 plugin-root.txt 后重试 / Plugin path records disagree; restore plugin-root.txt before continuing.'; exit;
    end;
    if CompareText(PreviousPlugin, PluginPath) <> 0 then begin
      Result := '升级不能变更 VST3 目录；如需迁移，请先卸载旧版 / To change the VST3 directory, uninstall the existing version first.'; exit;
    end;
  end;
  if (RequireOwner or (DirExists(AppPath) and not DirectoryEmpty(AppPath))) and
      not HasOwner(AddBackslash(AppPath) + '.rvc-install-owner') then begin
    Result := '此安装目录不属于本安装器，请选择空目录 / this nonempty folder is not owned by this installer.'; exit;
  end;
  if (DirExists(PluginPath) and not DirectoryEmpty(PluginPath)) and
      not HasOwner(AddBackslash(PluginPath) + 'Contents\Resources\install-owner.txt') then begin
    Result := '已存在未受管理的同名插件，请先移走旧手动安装 / move the existing unmanaged VST3 bundle first.'; exit;
  end;
  if not TreeSafeAndUnlocked(AppPath, Result) then exit;
  if not TreeSafeAndUnlocked(PluginPath, Result) then exit;
  if not DirectoryWritable(AppPath, Result) then exit;
  DirectoryWritable(PluginPath, Result);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  RuntimeChecksEnabled := False;
  if CheckedRuntimeFiles <> nil then CheckedRuntimeFiles.Free;
  if UnchangedRuntimeFiles <> nil then UnchangedRuntimeFiles.Free;
  CheckedRuntimeFiles := TStringList.Create;
  CheckedRuntimeFiles.Sorted := True;
  UnchangedRuntimeFiles := TStringList.Create;
  UnchangedRuntimeFiles.Sorted := True;
  Result := ValidateTargets(False);
  if Result <> '' then exit;
  if OldManifest <> nil then OldManifest.Free;
  OldManifest := TStringList.Create;
  HadOldManifest := FileExists(ExpandConstant('{app}\install-files.txt'));
  if HadOldManifest then begin
    if not LoadStringFromFile(ExpandConstant('{app}\install-files.txt'), OldManifestBytes) then begin
      Result := '无法读取旧安装清单 / Cannot read the previous installation manifest.'; exit;
    end;
    OldManifest.LoadFromFile(ExpandConstant('{app}\install-files.txt'));
  end;
  HadOldCleanupHelper := HadOldManifest and FileExists(ExpandConstant('{app}\rvc-state-cleanup.exe'));
  if HadOldCleanupHelper then
    if not LoadStringFromFile(ExpandConstant('{app}\rvc-state-cleanup.exe'), OldCleanupHelperBytes) then begin
      Result := '无法备份旧版缓存清理工具，请关闭宿主后重试 / Cannot back up the previous cache-cleanup helper; close the host and retry.'; exit;
    end;
  if not SaveStringToFile(ExpandConstant('{tmp}\install-owner.txt'), UTF8Encode(OwnerText + #13#10), False) or
     not SaveStringToFile(ExpandConstant('{tmp}\runtime-root.txt'), UTF8Encode(ExpandConstant('{app}\rvc-runtime') + #13#10), False) or
     not SaveStringToFile(ExpandConstant('{tmp}\state-root.txt'), UTF8Encode(ManagedStateConfiguration + #13#10), False) or
     not SaveStringToFile(ExpandConstant('{tmp}\plugin-root.txt'), UTF8Encode(Canonical(GetPluginDir('')) + #13#10), False) then
    Result := '无法生成安装配置 / Cannot stage installation configuration.';
#ifdef TestRoot
  { Exercise Inno's actual missing-source error and rollback after ordinary files were installed. }
  if ExpandConstant('{param:FixtureAbort|0}') = '1' then DeleteFile(ExpandConstant('{tmp}\state-root.txt'));
#endif
  RuntimeChecksEnabled := Result = '';
end;

function RuntimeFileNeedsUpdate(const RelativePath, ExpectedSha256: String): Boolean;
var Path: String;
begin
  Result := True;
  { Check also runs during page/space calculations. Hash only after final target validation. }
  if not RuntimeChecksEnabled then exit;
  if CheckedRuntimeFiles.IndexOf(RelativePath) >= 0 then begin
    Result := UnchangedRuntimeFiles.IndexOf(RelativePath) < 0;
    exit;
  end;
  Path := AddBackslash(ExpandConstant('{app}\rvc-runtime')) + RelativePath;
  if FileExists(Path) and not HasReparseAncestor(Path) then begin
    WizardForm.StatusLabel.Caption := '正在校验已安装的 runtime / Checking installed runtime...';
    try
      Result := CompareText(GetSHA256OfFile(Path), ExpectedSha256) <> 0;
    except
      Log('RVC runtime could not be verified, reinstalling: ' + RelativePath);
    end;
  end;
  CheckedRuntimeFiles.Add(RelativePath);
  if not Result then begin
    UnchangedRuntimeFiles.Add(RelativePath);
    Log('RVC runtime unchanged, skipped: ' + RelativePath);
  end;
end;

function ResolveOwnedFile(const Entry: String): String;
var Root, Relative: String;
begin
  Result := '';
  if Copy(Entry, 1, 4) = 'app|' then begin Root := Canonical(ExpandConstant('{app}')); Relative := Copy(Entry, 5, Length(Entry)); end
  else if Copy(Entry, 1, 7) = 'plugin|' then begin Root := Canonical(GetPluginDir('')); Relative := Copy(Entry, 8, Length(Entry)); end
  else exit;
  if (Relative = '') or (Pos(':', Relative) > 0) or (Pos('..', Relative) > 0) or (Copy(Relative, 1, 1) = '\') then exit;
  Result := Canonical(AddBackslash(Root) + Relative);
  if (Pos(Lowercase(AddBackslash(Root)), Lowercase(Result)) <> 1) or HasReparseAncestor(Result) then Result := '';
end;

procedure CurStepChanged(Step: TSetupStep);
var Current: TStringList; I: Integer; Path, Root, Parent: String;
begin
  if Step = ssInstall then InstallationStarted := True;
  if Step = ssDone then InstallSucceeded := True;
  { Inno has committed the new uninstall metadata before this callback. }
  if Step = ssPostInstall then InstallationCommitted := True;
  if (Step <> ssPostInstall) or (OldManifest = nil) then exit;
  Current := TStringList.Create;
  try
    Current.LoadFromFile(ExpandConstant('{app}\install-files.txt'));
    Current.Sorted := True;
    for I := 0 to OldManifest.Count - 1 do if Current.IndexOf(OldManifest[I]) < 0 then begin
      Path := ResolveOwnedFile(OldManifest[I]);
      if Path <> '' then begin
        if FileExists(Path) and not DeleteFile(Path) then RaiseException('Cannot remove obsolete installed file: ' + Path);
        if Copy(OldManifest[I], 1, 4) = 'app|' then Root := Canonical(ExpandConstant('{app}')) else Root := Canonical(GetPluginDir(''));
        Parent := ExtractFileDir(Path);
        while (CompareText(Parent, Root) <> 0) and (Length(Parent) > Length(Root)) do begin
          if not RemoveDir(Parent) then break;
          Parent := ExtractFileDir(Parent);
        end;
      end;
    end;
  finally Current.Free; end;
end;

procedure DeinitializeSetup;
begin
  { Inno rolls back new files but retains overwritten ones. Keep the old inventory
    after failed upgrades so a later repair can still prune obsolete old files. }
  if InstallationStarted and not InstallSucceeded and HadOldManifest then
    if not SaveStringToFile(ExpandConstant('{app}\install-files.txt'), OldManifestBytes, False) then
      SuppressibleMsgBox('无法恢复安装清单，请重新运行安装程序修复 / Cannot restore the manifest; rerun Setup to repair.', mbError, MB_OK, IDOK);
  { A file-copy failure keeps the old uninstaller and its pinned helper hash.
    Restore that helper too. After metadata commit the new uninstaller instead
    requires the new helper, even if later obsolete-file pruning fails. }
  if InstallationStarted and not InstallationCommitted and HadOldCleanupHelper then
    if not SaveStringToFile(ExpandConstant('{app}\rvc-state-cleanup.exe'), OldCleanupHelperBytes, False) then
      SuppressibleMsgBox('无法恢复旧版缓存清理工具，直接卸载可能受阻；请重新运行安装程序修复 / Cannot restore the previous cache-cleanup helper; direct uninstall may be blocked. Rerun Setup to repair.', mbError, MB_OK, IDOK);
  if OldManifest <> nil then OldManifest.Free;
  if CheckedRuntimeFiles <> nil then CheckedRuntimeFiles.Free;
  if UnchangedRuntimeFiles <> nil then UnchangedRuntimeFiles.Free;
end;

function CheckUninstallState(const Path: String; var IsOwned: Boolean): String;
var ResultCode: Integer;
begin
  Result := '';
  IsOwned := False;
  if not Exec(UninstallCleanupHelper, '--check "' + Path + '"', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then begin
    Result := '无法验证插件缓存，请重试 / Cannot verify plug-in cache: ' + Path; exit;
  end;
  if ResultCode = 0 then IsOwned := True
  else if ResultCode <> 2 then
    Result := '插件缓存正在使用、包含目录链接或归属异常，请关闭宿主后重试 / Plug-in cache is locked, linked or invalid; close the host and retry: ' + Path;
end;

function AddUninstallState(const StatePath: String): String;
var Path: String; IsOwned: Boolean;
begin
  Path := Canonical(StatePath);
  { The native helper validates ownership and holds non-delete-sharing directory
    handles; elevated path-based recursive deletion would have a junction race. }
  Result := CheckUninstallState(Path, IsOwned);
  if (Result = '') and IsOwned and (UninstallStateDirs.IndexOf(Path) < 0) then
    UninstallStateDirs.Add(Path);
end;

function ExpandedProfilePath(const Path: String): String;
var Buffer: String; Count: Cardinal;
begin
  Result := '';
  Buffer := StringOfChar(#0, 32768);
  Count := ExpandEnvironmentStringsW(Path, Buffer, 32768);
  if (Count = 0) or (Count > 32768) then exit;
  SetLength(Buffer, Count - 1);
  if AbsoluteDirectory(Buffer) then Result := Canonical(Buffer);
end;

function CollectUninstallStates: String;
var Item: TFindRec; ProfilePath: String; Profiles: TArrayOfString; I: Integer;
begin
  Result := '';
  if UninstallStateDirs <> nil then UninstallStateDirs.Free;
  UninstallStateDirs := TStringList.Create;
#ifdef TestRoot
  { Simulate registered profiles without reading or modifying real accounts. }
  if FindFirst('{#TestRoot}\profiles\*', Item) then begin
    try
      repeat
        if (Item.Name <> '.') and (Item.Name <> '..') and
            ((Item.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) then begin
          Result := AddUninstallState('{#TestRoot}\profiles\' + Item.Name + '\AppData\Local\Taketo\RvcFeatureInjector\state');
          if Result <> '' then exit;
        end;
      until not FindNext(Item);
    finally FindClose(Item); end;
  end;
#else
  Result := AddUninstallState(ExpandConstant('{localappdata}\Taketo\RvcFeatureInjector\state'));
  if Result <> '' then exit;
  if RegGetSubkeyNames(HKLM64, 'Software\Microsoft\Windows NT\CurrentVersion\ProfileList', Profiles) then begin
    for I := 0 to GetArrayLength(Profiles) - 1 do begin
      if RegQueryStringValue(HKLM64, 'Software\Microsoft\Windows NT\CurrentVersion\ProfileList\' + Profiles[I],
          'ProfileImagePath', ProfilePath) then begin
        ProfilePath := ExpandedProfilePath(ProfilePath);
        if ProfilePath <> '' then begin
          Result := AddUninstallState(AddBackslash(ProfilePath) + 'AppData\Local\Taketo\RvcFeatureInjector\state');
          if Result <> '' then exit;
        end;
      end;
    end;
  end;
#endif
end;

procedure CurUninstallStepChanged(Step: TUninstallStep);
var I, ResultCode: Integer; Path, Problem: String; IsOwned: Boolean;
begin
  if (Step <> usUninstall) or (UninstallStateDirs = nil) then exit;
  { Recheck all known cache roots immediately before any removals. }
  for I := 0 to UninstallStateDirs.Count - 1 do begin
    Path := UninstallStateDirs[I];
    Problem := CheckUninstallState(Path, IsOwned);
    if Problem <> '' then RaiseException(Problem);
    if not IsOwned then
      RaiseException('Cache ownership changed; uninstall stopped: ' + Path);
  end;
  for I := 0 to UninstallStateDirs.Count - 1 do begin
    Path := UninstallStateDirs[I];
    if not Exec(UninstallCleanupHelper, '--remove "' + Path + '"', '', SW_HIDE,
        ewWaitUntilTerminated, ResultCode) then
      RaiseException('Cannot remove the verified plug-in cache; uninstall stopped: ' + Path);
    if ResultCode <> 0 then
      RaiseException('Cache removal did not complete; uninstall stopped: ' + Path);
  end;
end;

function InitializeUninstall: Boolean;
var Problem: String;
begin
  { Never infer a removal destination from current wizard defaults or command-line options. }
  if InstalledPluginPath(Canonical(ExpandConstant('{app}')), UninstallPluginDir) then
    Problem := ValidateTargets(True)
  else
    Problem := '无法验证已安装插件路径，请恢复安装配置后重试 / Cannot verify the installed plug-in path; restore its installation configuration before uninstalling.';
  if Problem = '' then begin
    UninstallCleanupHelper := ExpandConstant('{tmp}\rvc-state-cleanup.exe');
    if not FileExists(ExpandConstant('{app}\rvc-state-cleanup.exe')) then
      Problem := '缺少缓存清理工具，请重新安装修复后卸载 / Cache-cleanup helper is missing; repair installation before uninstalling.'
    else if CompareText(GetSHA256OfFile(ExpandConstant('{app}\rvc-state-cleanup.exe')), '{#CleanupHelperSha256}') <> 0 then
      Problem := '缓存清理工具校验失败，请重新安装修复后卸载 / Cache-cleanup helper checksum mismatch; repair installation before uninstalling.'
    else if not CopyFile(ExpandConstant('{app}\rvc-state-cleanup.exe'), UninstallCleanupHelper, False) then
      Problem := '无法准备安全缓存清理工具，请重新安装修复后卸载 / Cannot prepare the safe cache-cleanup helper; repair installation before uninstalling.';
    if Problem = '' then begin
      { Keep the verified executable immutable until uninstall has finished. }
      UninstallCleanupHandle := CreateFileW(UninstallCleanupHelper, $80000000, 1, 0, 3, $80, 0);
      if UninstallCleanupHandle = THandle(-1) then
        Problem := '无法锁定缓存清理工具，已安全中止 / Cannot lock the cache-cleanup helper; uninstall stopped safely.';
    end;
    { Verify the copy too: a custom runtime directory may be user-writable. }
    if (Problem = '') and (CompareText(GetSHA256OfFile(UninstallCleanupHelper), '{#CleanupHelperSha256}') <> 0) then
      Problem := '临时缓存清理工具校验失败，已安全中止 / Temporary cache-cleanup helper checksum mismatch; uninstall stopped safely.';
  end;
  if Problem = '' then Problem := CollectUninstallStates;
  Result := Problem = '';
  if not Result then SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);
end;

procedure DeinitializeUninstall;
begin
  if (UninstallCleanupHandle <> 0) and (UninstallCleanupHandle <> THandle(-1)) then
    CloseHandle(UninstallCleanupHandle);
  if UninstallStateDirs <> nil then UninstallStateDirs.Free;
end;
