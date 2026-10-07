# Fetches the third-party sources HEARASIDE builds against into ./external.
# Pinned versions - change them here only.
$ErrorActionPreference = 'Stop'
$ext = Join-Path $PSScriptRoot '..\external'
New-Item -ItemType Directory -Force $ext | Out-Null
Push-Location $ext
try {
    if (-not (Test-Path JUCE)) {
        git -c advice.detachedHead=false clone --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE.git JUCE
    }
    if (-not (Test-Path obs-studio)) {
        # only libobs headers are needed (matches OBS 32.2.x)
        git -c advice.detachedHead=false clone --depth 1 --branch 32.2.2 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git obs-studio
        git -C obs-studio sparse-checkout set libobs cmake/windows
    }
    if (-not (Test-Path speexdsp)) {
        git clone --depth 1 https://github.com/xiph/speexdsp.git speexdsp
    }
    # Fonts (Anuphan, SIL OFL) are committed in external/fonts.
} finally {
    Pop-Location
}
