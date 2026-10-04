[CmdletBinding()]
param([switch] $Reconfigure, [switch] $NoDeploy)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

$localPaths = Join-Path $PSScriptRoot 'local-paths.ps1'
if (Test-Path $localPaths) { . $localPaths }
foreach ($v in 'VCPKG_ROOT', 'KRI_COMMONLIB_DIR') {
    if (-not (Get-Item "env:$v" -ErrorAction SilentlyContinue)) { throw "$v is not set (see tools\local-paths.example.ps1)." }
}
$DevMod = if ($NoDeploy) { '' } else { $env:KRI_MO2_DEV_MOD }
if ($DevMod -and @(Get-Process -ErrorAction SilentlyContinue | Where-Object ProcessName -eq 'SkyrimSE').Count -gt 0) {
    throw 'Skyrim is running and holds KroosRecycleItems.dll open. Close the game, or use -NoDeploy.'
}

# Enter the MSVC v143 developer environment.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -property installationPath
$toolset = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Where-Object Name -like '14.4*' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolset) { throw 'MSVC v143 (14.4x) is not installed.' }
$env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH  # vcvarsall.bat calls vswhere.exe
$vcvarsall = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
# Full path to cmd.exe: a bare `cmd` can resolve to an extensionless file on PATH (devkitPro msys2 ships one).
foreach ($line in (& $env:ComSpec /c "`"$vcvarsall`" x64 -vcvars_ver=$($toolset.Name.Substring(0, 5)) >nul & set")) {
    if ($line -match '^([^=]+)=(.*)$') { try { Set-Item "env:$($matches[1])" $matches[2] -ErrorAction Stop } catch { } }
}
$env:PATH = (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin') + ';' +
            (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH

$buildDir = Join-Path $RepoRoot 'build\release'
if ($Reconfigure -and (Test-Path $buildDir)) { Remove-Item $buildDir -Recurse -Force }

Push-Location $RepoRoot
# PowerShell 5.1 turns native stderr into terminating errors under 'Stop'; rely on exit codes instead.
$ErrorActionPreference = 'Continue'
try {
    cmake --preset release;        if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    cmake --build --preset release; if ($LASTEXITCODE) { throw 'Build failed.' }
} finally { Pop-Location; $ErrorActionPreference = 'Stop' }

if ($DevMod) {
    $plugins = Join-Path $DevMod 'SKSE\Plugins'
    New-Item -ItemType Directory -Force $plugins | Out-Null
    Copy-Item (Join-Path $buildDir 'KroosRecycleItems.dll') $plugins -Force
    $pdb = Join-Path $buildDir 'KroosRecycleItems.pdb'
    if (Test-Path $pdb) { Copy-Item $pdb $plugins -Force }
    # Plugin, CraftingCategories config, translations.
    Copy-Item (Join-Path $RepoRoot 'data\*') $DevMod -Recurse -Force
    Write-Host "Deployed to $DevMod"
}
