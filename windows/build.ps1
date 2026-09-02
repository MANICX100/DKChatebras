param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$Vs = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $Vs) { throw 'MSVC x64 Build Tools were not found.' }
if ($Clean -and (Test-Path $Build)) { Remove-Item $Build -Recurse -Force }

cmake -S $Root -B $Build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
cmake --build $Build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$Exe = Join-Path $Build "bin\DKChatebras.exe"
if (-not (Test-Path $Exe)) { throw "Expected output was not produced: $Exe" }
Write-Host "Built: $Exe"
