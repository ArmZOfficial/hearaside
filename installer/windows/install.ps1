<#
  Copies a HEARASIDE build into the standard plug-in folders (run as Administrator):
    VST3 -> C:\Program Files\Common Files\VST3\HEARASIDE\
    VST2 -> <Vst2Dir>\HEARASIDE\            (only if the build has VST2)
    OBS  -> C:\ProgramData\obs-studio\plugins\hearaside-obs\bin\64bit\
  Close your DAW and OBS first (loaded DLLs cannot be replaced).
    .\installer\windows\install.ps1 -BuildDir build\Release
    .\installer\windows\install.ps1 -Uninstall
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\..\build\Release'),
    [string]$Vst2Dir = 'C:\Program Files\VSTPlugins',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run this script from an elevated (Administrator) PowerShell.'
}

$vst3Dest = Join-Path $env:CommonProgramFiles 'VST3\HEARASIDE'
$vst2Dest = Join-Path $Vst2Dir 'HEARASIDE'
$obsDest  = Join-Path $env:ProgramData 'obs-studio\plugins\hearaside-obs'

if ($Uninstall) {
    foreach ($d in $vst3Dest, $vst2Dest, $obsDest) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d; "removed $d" }
    }
    return
}

$plugins = Join-Path $BuildDir 'plugins'
if (-not (Test-Path $plugins)) { throw "No plug-in build found in $BuildDir (run .\build.ps1 first)" }

New-Item -ItemType Directory -Force $vst3Dest | Out-Null
foreach ($target in 'Track', 'Hub', 'AppAudio') {
    $name = if ($target -eq 'AppAudio') { 'App Audio' } else { $target }
    $bundle = Join-Path $plugins "Hearaside${target}_artefacts\Release\VST3\HEARASIDE $name.vst3"
    if (-not (Test-Path $bundle)) { if ($target -eq 'AppAudio') { continue } else { throw "missing $bundle" } }
    Copy-Item -Recurse -Force $bundle $vst3Dest
    "VST3  HEARASIDE $name -> $vst3Dest"

    $vst2 = Join-Path $plugins "Hearaside${target}_artefacts\Release\VST\HEARASIDE $name.dll"
    if (Test-Path $vst2) {
        New-Item -ItemType Directory -Force $vst2Dest | Out-Null
        Copy-Item -Force $vst2 $vst2Dest
        "VST2  HEARASIDE $name -> $vst2Dest"
    }
}

$obsDll = Join-Path $BuildDir 'obs\hearaside-obs\hearaside-obs.dll'
if (Test-Path $obsDll) {
    $bin = Join-Path $obsDest 'bin\64bit'
    New-Item -ItemType Directory -Force $bin | Out-Null
    Copy-Item -Force $obsDll $bin
    "OBS   hearaside-obs -> $bin"
}

$ofl = Join-Path $PSScriptRoot '..\..\external\fonts\OFL.txt'
if (Test-Path $ofl) { Copy-Item -Force $ofl (Join-Path $vst3Dest 'Anuphan-OFL.txt') }
'Done. Rescan plug-ins in your DAW, then add "HEARASIDE" as an audio source in OBS.'
