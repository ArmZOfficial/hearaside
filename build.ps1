<#
  Builds HEARASIDE with MSVC + Ninja.
    .\build.ps1                       # Release, everything
    .\build.ps1 -Config Debug
    .\build.ps1 -CMakeArgs '-DHEARASIDE_BUILD_OBS=OFF'
    .\build.ps1 -Vst2Sdk C:\SDKs\vst2   # only with a licensed VST 2 SDK
    .\build.ps1 -Test                  # run unit + integration tests after building
    .\build.ps1 -Install               # copy VST3 / OBS plug-in into the system folders (admin)
#>
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string]$Config = 'Release',
    [string[]]$CMakeArgs = @(),
    [string]$Vst2Sdk = '',
    [switch]$Test,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root "build\$Config"

# --- import the MSVC developer environment ----------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools not found' }
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = "$env:ProgramFiles\CMake\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:PATH"

$cfgArgs = @('-S', $root, '-B', $build, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config") + $CMakeArgs
if ($Vst2Sdk) { $cfgArgs += "-DHEARASIDE_VST2_SDK=$Vst2Sdk" }
& cmake @cfgArgs
if ($LASTEXITCODE) { throw 'cmake configure failed' }
& cmake --build $build
if ($LASTEXITCODE) { throw 'build failed' }

if ($Test) {
    & ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw 'tests failed' }
}

if ($Install) {
    & (Join-Path $root 'installer\windows\install.ps1') -BuildDir $build
}


