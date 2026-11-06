# Signs the Windows build of a release. Run on the Windows machine with the
# Certum code-signing card plugged in, after pushing a vX.Y.Z tag:
#
#   .\packaging\windows\release.ps1 v1.2.3
#
# 1. waits for that tag's CI run and downloads its Windows zip;
# 2. signs the plugin DLL, builds the installer with a signed uninstaller,
#    signs the installer and packs the zip again around the signed DLL (RFC 3161
#    timestamps throughout), and verifies each signature;
# 3. attaches installer and zip to the tag's draft release and adds their
#    checksums to its notes. Publishing the draft is left to you.
#
# CI cannot reach the card, so it leaves the Windows files out of the draft.
# The card may ask for its PIN for each signature.
#
# Needs gh (signed in), signtool (Windows SDK), Inno Setup and proCertumCardManager
# with the card's certificate registered in the Windows certificate store.
#
#   -Thumbprint    SHA-1 thumbprint of the certificate; by default the only valid
#                  "Open Source Developer" certificate from Certum in the
#                  current user's store. Or set SIGN_THUMBPRINT.
#   -TimestampUrl  default http://time.certum.pl
#   -DryRun        sign and verify, but upload nothing
#
# The results stay in packaging\windows\build.
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)] [string] $Tag,
    [string] $Thumbprint = $env:SIGN_THUMBPRINT,
    [string] $TimestampUrl = 'http://time.certum.pl',
    [switch] $DryRun
)
$ErrorActionPreference = 'Stop'

$Repo = 'gleem-gg/obs-gleem'
$Workflow = 'push.yaml'
$Plugin = 'obs-gleem'

function Say([string] $Message) { Write-Host "==> $Message" -ForegroundColor Blue }

# Runs a program; a non-zero exit code is an error (signtool: 1 failed, 2 warning).
# Uses $args, not param(): PowerShell would take gh's -R and -L for its own.
function Invoke-Tool {
    $tool, $rest = $args
    & $tool @rest
    if ($LASTEXITCODE -ne 0) { throw "$tool $($rest -join ' ') failed with exit code $LASTEXITCODE" }
}

# gh's JSON output, one object per array element.
function Get-GhJson {
    $json = (& gh @args) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "gh $($args -join ' ') failed" }
    # In parentheses: Windows PowerShell passes an array on as one object.
    (ConvertFrom-Json $json)
}

if ($Tag -notmatch '^v(\d+\.\d+\.\d+)(-(beta|rc)\d*)?$') { throw 'usage: release.ps1 vX.Y.Z[-betaN|-rcN]' }
$version = $Matches[1]

# ---- Tools -------------------------------------------------------------------
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'gh is not installed' }

$signtool = (Get-Command signtool.exe -ErrorAction SilentlyContinue).Source
if (-not $signtool) {
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object { [version] $_.Directory.Parent.Name } |
        Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $signtool) { throw 'signtool.exe not found: install the Windows SDK' }

if (-not $Thumbprint) {
    $certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object {
        $_.Issuer -like '*Certum*' -and $_.Subject -like '*Open Source Developer*' -and $_.NotAfter -gt (Get-Date)
    })
    if ($certs.Count -ne 1) {
        throw "Found $($certs.Count) valid Certum code-signing certificates in the store. With the card plugged in, register its certificate in proCertumCardManager, or pass -Thumbprint."
    }
    $Thumbprint = $certs[0].Thumbprint
    Say "Signing as $($certs[0].Subject), valid until $($certs[0].NotAfter.ToString('yyyy-MM-dd'))"
}

# Embedded in the signature so Windows can build the chain without fetching
# it: Certum Code Signing 2021 CA, issued by Certum Trusted Network CA 2.
$intermediate = Join-Path $PSScriptRoot 'certs\certum-code-signing-2021-ca.pem'
$description = 'Gleem for OBS'
$url = 'https://gleem.gg'

function Invoke-Sign([string] $File) {
    Invoke-Tool $signtool sign /sha1 $Thumbprint /ac $intermediate /fd sha256 /tr $TimestampUrl /td sha256 /d $description /du $url $File
}

# Chains to a trusted root and carries a timestamp, so it outlives the certificate.
function Assert-Signed([string] $File) {
    Invoke-Tool $signtool verify /pa /tw $File
}

# ---- The tag's build ---------------------------------------------------------
# The run of this tag at the commit it points at now: a moved tag leaves the
# old runs behind.
$commit = (& gh api "repos/$Repo/commits/$Tag" -q .sha)
if ($LASTEXITCODE -ne 0 -or -not $commit) { throw "no tag $Tag on GitHub" }

Say "Waiting for the CI run of $Tag ($commit)"
$run = $null
foreach ($attempt in 1..60) {
    $run = Get-GhJson run list -R $Repo -w $Workflow --commit $commit --event push -L 20 --json 'databaseId,headBranch' |
        Where-Object { $_.headBranch -eq $Tag } | Select-Object -First 1
    if ($run) { break }
    Start-Sleep -Seconds 10
}
if (-not $run) { throw "no CI run for $Tag; was the tag pushed?" }
$runId = "$($run.databaseId)"
Invoke-Tool gh run watch $runId -R $Repo --exit-status | Out-Null

# Before the signing, which takes PINs: a published release stays as it is.
$release = $null
if (-not $DryRun) {
    $release = Get-GhJson release view $Tag -R $Repo --json 'isDraft,body'
    if (-not $release.isDraft) { throw "$Tag is already published" }
}

$build = Join-Path $PSScriptRoot 'build'
$work = Join-Path $build 'work'
if (Test-Path $build) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force $work | Out-Null

Say "Downloading the build of run $runId"
Invoke-Tool gh run download $runId -R $Repo -p "$Plugin-*-windows-x64-*" -D (Join-Path $work 'artifacts')
$zipName = "$Plugin-$version-windows-x64.zip"
$zip = Get-ChildItem (Join-Path $work 'artifacts') -Recurse -Filter $zipName | Select-Object -First 1
if (-not $zip) { throw "the run has no $zipName; does buildspec.json say $version?" }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = Join-Path $work 'stage'
[IO.Compression.ZipFile]::ExtractToDirectory($zip.FullName, $stage)
$pluginDir = Join-Path $stage $Plugin
$dll = Join-Path $pluginDir "$Plugin.dll"
$legacyDll = Join-Path $pluginDir "bin\64bit\$Plugin.dll"
if (-not (Test-Path $dll) -or -not (Test-Path $legacyDll)) { throw "$zipName does not have the expected layout" }

# ---- Sign --------------------------------------------------------------------
Say "Signing $Plugin.dll (the card may ask for its PIN)"
Invoke-Sign $dll
Assert-Signed $dll
Copy-Item -Force $dll $legacyDll

Say 'Building and signing the installer'
# Not interpolated: $q and $f are for Inno Setup.
$signCommand = '$q' + $signtool + '$q sign /sha1 ' + $Thumbprint + ' /ac $q' + $intermediate +
    '$q /fd sha256 /tr ' + $TimestampUrl + ' /td sha256 /d $q' + $description + '$q /du ' + $url + ' $f'
& (Join-Path $PSScriptRoot 'build-installer.ps1') -Stage $pluginDir -Version $version -OutputDir $build -SignCommand $signCommand
$installer = Join-Path $build "$Plugin-$version-windows-x64.exe"
Assert-Signed $installer

$signedZip = Join-Path $build $zipName
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $signedZip)
Say 'Signatures verify'

$assets = @($installer, $signedZip)
$checksums = foreach ($file in $assets) {
    "    $(Split-Path -Leaf $file): $((Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant())"
}
$checksums | ForEach-Object { Write-Host $_ }

if ($DryRun) {
    Say "Dry run: signed files are in $build, nothing uploaded"
    exit 0
}

# ---- Attach to the draft -----------------------------------------------------
Say "Attaching to the draft release $Tag"
Invoke-Tool gh release upload $Tag @assets -R $Repo --clobber

# Lines of an earlier attempt go: signing again gives other checksums.
$kept = @("$($release.body)" -split "`r?`n" | Where-Object {
    $line = $_
    -not ($assets | Where-Object { $line -like "*$(Split-Path -Leaf $_):*" })
})
$notes = Join-Path $work 'notes.md'
[IO.File]::WriteAllText($notes, (($kept -join "`n").TrimEnd() + "`n" + ($checksums -join "`n") + "`n"))
Invoke-Tool gh release edit $Tag -R $Repo --notes-file $notes

Say "Done. Check the draft and publish it: https://github.com/$Repo/releases"
