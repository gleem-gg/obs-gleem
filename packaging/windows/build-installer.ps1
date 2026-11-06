# Compiles obs-gleem.iss into obs-gleem-<version>-windows-x64.exe with Inno
# Setup. Called by Package-Windows.ps1 in CI (unsigned) and by release.ps1
# (signed). Stage is the plugin folder that `cmake --install` produced.
#
# SignCommand is an Inno Setup sign tool command, where $q is a quote and $f
# the file to sign. Inno Setup then signs the uninstaller and the installer.
param(
    [Parameter(Mandatory)] [string] $Stage,
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $OutputDir,
    [string] $SignCommand
)
$ErrorActionPreference = 'Stop'

$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = Get-ChildItem "$env:LOCALAPPDATA\Programs\Inno Setup *\ISCC.exe",
                          "${env:ProgramFiles(x86)}\Inno Setup *\ISCC.exe",
                          "$env:ProgramFiles\Inno Setup *\ISCC.exe" -ErrorAction SilentlyContinue |
        Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $iscc) { throw 'Inno Setup (ISCC.exe) not found: winget install JRSoftware.InnoSetup' }

$stagePath = (Resolve-Path $Stage).Path
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$arguments = @('/Qp', "/DAppVersion=$Version", "/DStage=$stagePath", "/O$((Resolve-Path $OutputDir).Path)")
if ($SignCommand) { $arguments += @('/DSign', "/Sgleem=$SignCommand") }

& $iscc @arguments (Join-Path $PSScriptRoot 'obs-gleem.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
