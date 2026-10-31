# Regenerates asio_convert.golden from the C++ HasAsio.cpp (needs the MSVC Build Tools; not part of cargo test).
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path "$here\..\..\..\..\..").Path
$tmp = Join-Path $env:TEMP 'hs-asio-golden'
New-Item -ItemType Directory -Force $tmp | Out-Null
# stub header: the declarations of apps/karaoke/HasAsio.h without JUCE (the JUCE half of HasAsio.cpp is #if _WIN32 only)
$h = Get-Content "$repo\apps\karaoke\HasAsio.h" -Raw
$h = $h -replace '#include <juce_audio_devices/juce_audio_devices.h>', "#include <cstdint>`nnamespace juce { struct AudioIODeviceType { virtual ~AudioIODeviceType() = default; }; }"
Set-Content "$tmp\HasAsio.h" $h
Copy-Item "$repo\apps\karaoke\HasAsio.cpp" "$tmp\HasAsio.cpp" -Force   # a quoted #include looks beside the including file first
$vcvars = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022" -Recurse -Filter vcvars64.bat | Select-Object -First 1
cmd /c "`"$($vcvars.FullName)`" >nul 2>&1 & cd /d `"$tmp`" && cl /nologo /EHsc /O2 /std:c++17 /U_WIN32 /I. `"$here\gen_asio_convert.cpp`" HasAsio.cpp /Fe:gen.exe"
& "$tmp\gen.exe" "$here\asio_convert.golden"
