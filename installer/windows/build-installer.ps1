<#
  Builds dist\HEARASIDE-Setup-<version>.exe with Inno Setup 6 from an existing build:
    .\build.ps1 -Installer                     # builds, then this
    .\installer\windows\build-installer.ps1    # uses build\Release
  The version comes from CMakeLists.txt. cloudflared is downloaded once into installer\windows\cache
  and checked against the SHA-256 in deps.json; a wrong hash stops the build. Nothing downloaded is
  committed. VST2 is included when the build has it (built with -Vst2Sdk).
  Optional signing: set HEARASIDE_SIGNTOOL to a full signtool command, e.g.
    $env:HEARASIDE_SIGNTOOL = 'signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a $f'
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\..\build\Release'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\..\dist')
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(Hearaside VERSION ([0-9.]+)').Matches[0].Groups[1].Value

# --- Inno Setup ------------------------------------------------------------------------------
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found. Install it: winget install --id JRSoftware.InnoSetup' }

# --- cloudflared, pinned ---------------------------------------------------------------------
$deps = Get-Content (Join-Path $PSScriptRoot 'deps.json') -Raw | ConvertFrom-Json
$cache = Join-Path $PSScriptRoot 'cache'
New-Item -ItemType Directory -Force $cache | Out-Null
$cf = Join-Path $cache "cloudflared-$($deps.cloudflared.version).exe"
function Get-Hash($p) { (Get-FileHash -Algorithm SHA256 $p).Hash.ToLowerInvariant() }
if (-not (Test-Path $cf) -or (Get-Hash $cf) -ne $deps.cloudflared.sha256) {
    "downloading cloudflared $($deps.cloudflared.version)"
    Invoke-WebRequest -Uri $deps.cloudflared.url -OutFile $cf -UseBasicParsing
}
if ((Get-Hash $cf) -ne $deps.cloudflared.sha256) {
    Remove-Item -Force $cf
    throw 'cloudflared: SHA-256 does not match deps.json. Build stopped.'
}
$cfLicense = Join-Path $cache "cloudflared-LICENSE-$($deps.cloudflared.version).txt"
if (-not (Test-Path $cfLicense)) { Invoke-WebRequest -Uri $deps.cloudflared.licenseUrl -OutFile $cfLicense -UseBasicParsing }

# --- what the build produced -----------------------------------------------------------------
$plugins = Join-Path $BuildDir 'plugins'
$hasVst2 = [bool](Get-ChildItem -Path $plugins -Recurse -Filter 'HEARASIDE *.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\VST\\' })
$obsDll = Join-Path $BuildDir 'obs\hearaside-obs\hearaside-obs.dll'
if (-not (Test-Path $obsDll)) { throw "OBS plug-in not built: $obsDll" }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$defs = @("/DAppVersion=$version", "/DBuildDir=$BuildDir", "/DRootDir=$root", "/DCloudflared=$cf", "/DCloudflaredLicense=$cfLicense",
          "/DCloudflaredVersion=$($deps.cloudflared.version)", "/DOutDir=$((Resolve-Path $OutDir).Path)")
if ($hasVst2) { $defs += '/DWithVst2=1' }
if ($env:HEARASIDE_SIGNTOOL) { $defs += "/Ssigntool=$env:HEARASIDE_SIGNTOOL" ; $defs += '/DSigned=1' }

& $iscc @defs (Join-Path $PSScriptRoot 'hearaside.iss')
if ($LASTEXITCODE) { throw 'Inno Setup failed' }

$exe = Join-Path (Resolve-Path $OutDir) "HEARASIDE-Setup-$version.exe"
$hash = Get-Hash $exe
$size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
"installer: $exe ($size MB, VST2 $(if ($hasVst2) { 'included' } else { 'not included' }))"
"sha256:    $hash"

# numbers for the website's /download page (web\site\src\content\release.json)
$rel = Join-Path $root 'web\site\src\content\release.json'
if (Test-Path $rel) {
    $j = [IO.File]::ReadAllText($rel, [Text.Encoding]::UTF8) | ConvertFrom-Json   # UTF-8 (Thai notes)
    $j.version = $version; $j.sizeMb = "$size"; $j.sha256 = $hash; $j.date = (Get-Date -Format 'd MMM yyyy')
    $j.installer = "/downloads/HEARASIDE-Setup-$version.exe"
    $j | Add-Member -NotePropertyName vst2 -NotePropertyValue $hasVst2 -Force
    [IO.File]::WriteAllText($rel, ($j | ConvertTo-Json -Depth 6) + "`n", (New-Object Text.UTF8Encoding $false))
    "release.json updated"
}
