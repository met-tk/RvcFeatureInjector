#ifndef PayloadRoot
  #error PayloadRoot is required
#endif
#ifndef ManifestRoot
  #error ManifestRoot is required
#endif
#ifndef PluginRoot
  #define PluginRoot PayloadRoot + "\RvcFeatureInjector.vst3"
#endif
#ifndef ReleaseVersion
  #define ReleaseVersion "1.1.9"
#endif
#ifndef CleanupHelper
  #define CleanupHelper SourcePath + "\..\build\Release\rvc-state-cleanup.exe"
#endif
#ifndef CleanupHelperSha256
  #error CleanupHelperSha256 is required
#endif

[Setup]
AppId=Taketo.RvcFeatureInjector.Machine
AppName=RVC Feature Injector
AppVersion={#ReleaseVersion}
AppPublisher=Taketo
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallLogMode=append
UninstallDisplayName=RVC Feature Injector
UninstallDisplayIcon={app}\unins000.exe
CloseApplications=no
RestartApplications=no
AlwaysRestart=no
DirExistsWarning=no
DisableDirPage=no
WizardStyle=modern
MinVersion=10.0
SolidCompression=no
DiskSpanning=no
OutputDir=..\dist\installer-{#ReleaseVersion}
SetupLogging=no
InfoBeforeFile=install-info.txt
#ifdef TestRoot
PrivilegesRequired=lowest
DefaultDirName={#TestRoot}\app\RvcFeatureInjector
CreateUninstallRegKey=no
SetupMutex=RvcFeatureInjectorCleanSetupTest
UsePreviousAppDir=no
Compression=zip/1
OutputBaseFilename=RvcFeatureInjector-Test-{#ReleaseVersion}
#else
PrivilegesRequired=admin
DefaultDirName={autopf}\Taketo\RvcFeatureInjector
SetupMutex=RvcFeatureInjectorCleanSetup
UsePreviousAppDir=yes
Compression=lzma2/max
LZMANumBlockThreads=2
LZMABlockSize=262144
OutputBaseFilename=RvcFeatureInjector-{#ReleaseVersion}-Setup
#endif

[Languages]
Name: "zhcn"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
en.RuntimeTitle=Required plug-in inference runtime
en.RuntimeDescription=Choose where to store the files required by the VST3 plug-in
en.RuntimeInstructions=This is not a standalone application. This folder contains the portable Python/CUDA runtime, HuBERT/RMVPE base assets, installation records and uninstaller required by the VST3 plug-in.
en.RuntimeBrowse=Choose a dedicated RvcFeatureInjector folder. The VST3 plug-in folder is selected on the next page.
en.VST3Title=Select VST3 plug-in directory
en.VST3Description=Where should Cubase find the plug-in?
en.VST3Instructions=Choose a VST3 parent folder that your host scans. Setup creates RvcFeatureInjector.vst3 inside it. The default is the standard machine VST3 folder used by Cubase. Administrator access is required for installation.
en.VST3Folder=VST3 parent folder:
en.VST3Summary=VST3 plug-in:
zhcn.RuntimeTitle=插件推理运行库（必需）
zhcn.RuntimeDescription=选择 VST3 插件所需依赖文件的存放位置
zhcn.RuntimeInstructions=这不是独立应用程序。此目录存放 VST3 插件必需的便携 Python/CUDA 运行库、HuBERT/RMVPE 基础模型、安装记录及卸载程序。
zhcn.RuntimeBrowse=请选择以 RvcFeatureInjector 结尾的专属目录。下一页将单独选择 VST3 插件目录。
zhcn.VST3Title=选择 VST3 插件目录
zhcn.VST3Description=将插件安装到 Cubase 扫描的目录
zhcn.VST3Instructions=请选择宿主能扫描的 VST3 父目录，安装器会在其中创建 RvcFeatureInjector.vst3。默认是 Cubase 使用的标准系统 VST3 目录。安装需要管理员权限。
zhcn.VST3Folder=VST3 父目录：
zhcn.VST3Summary=VST3 插件：

#ifdef TestRoot
[Dirs]
Name: "{app}\state"
#endif

#include ManifestRoot + "\runtime-files.iss"

[Files]
Source: "{#PluginRoot}\*"; DestDir: "{code:GetPluginDir}"; Excludes: "runtime-root.txt,state-root.txt,install-owner.txt,\Contents\Resources\models\*,__pycache__,*.pyc,*.pyo"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "install-info.txt"; DestDir: "{app}"; DestName: "INSTALLATION.txt"; Flags: ignoreversion
Source: "{#ManifestRoot}\install-files.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CleanupHelper}"; DestDir: "{app}"; DestName: "rvc-state-cleanup.exe"; Flags: ignoreversion
Source: "{tmp}\install-owner.txt"; DestDir: "{app}"; DestName: ".rvc-install-owner"; ExternalSize: 1024; Flags: ignoreversion external
Source: "{tmp}\install-owner.txt"; DestDir: "{code:GetPluginDir}\Contents\Resources"; ExternalSize: 1024; Flags: ignoreversion external
Source: "{tmp}\runtime-root.txt"; DestDir: "{code:GetPluginDir}\Contents\Resources"; ExternalSize: 1024; Flags: ignoreversion external
Source: "{tmp}\state-root.txt"; DestDir: "{code:GetPluginDir}\Contents\Resources"; ExternalSize: 1024; Flags: ignoreversion external
Source: "{tmp}\plugin-root.txt"; DestDir: "{app}"; ExternalSize: 1024; Flags: ignoreversion external

#include ManifestRoot + "\owned-uninstall.iss"

#ifdef TestRoot
[UninstallDelete]
Type: filesandordirs; Name: "{app}\state"
#endif

[Code]
#include "ownership.iss"
