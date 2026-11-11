param([string]$RuntimeRoot, [string]$PluginBundle)
$ErrorActionPreference = 'Stop'
$releaseRoot = $PSScriptRoot
$source = if ($PluginBundle) { $PluginBundle } else { Join-Path $releaseRoot 'RvcFeatureInjector.vst3' }
if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw 'Plugin bundle is missing beside this script.' }
if (-not $RuntimeRoot) { $RuntimeRoot = $env:RVC_RUNTIME_ROOT }
if (-not $RuntimeRoot) {
    $localRuntime = Join-Path $releaseRoot 'rvc-runtime'
    $sourceConfig = Join-Path $source 'Contents\Resources\runtime-root.txt'
    if (Test-Path -LiteralPath $localRuntime -PathType Container) { $RuntimeRoot = $localRuntime }
    elseif (Test-Path -LiteralPath $sourceConfig -PathType Leaf) {
        $RuntimeRoot = [IO.File]::ReadAllText($sourceConfig).Trim()
        if (-not [IO.Path]::IsPathRooted($RuntimeRoot)) { $RuntimeRoot = Join-Path (Split-Path $sourceConfig) $RuntimeRoot }
    } else { throw 'Specify -RuntimeRoot with the existing RVC runtime root.' }
}
$resolvedRuntime = (Resolve-Path -LiteralPath $RuntimeRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $resolvedRuntime 'runtime\python.exe'))) { throw 'Runtime root must contain runtime\python.exe.' }
$target = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3\RvcFeatureInjector.vst3'
if (Test-Path -LiteralPath (Join-Path $target 'Contents\Resources\install-owner.txt')) {
    throw 'This plugin is managed by Setup. Use Setup to update it, or uninstall it before a manual installation.'
}
New-Item -ItemType Directory -Force -Path $target | Out-Null
Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $target -Recurse -Force
$config = Join-Path $target 'Contents\Resources\runtime-root.txt'
[IO.File]::WriteAllText($config, $resolvedRuntime + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
Write-Output "Installed: $target"
Write-Output "Runtime: $resolvedRuntime"
Write-Output 'Keep the runtime folder in place, then rescan VST3 plugins in Cubase.'
