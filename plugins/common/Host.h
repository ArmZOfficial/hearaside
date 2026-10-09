// Which DAW hosts the plug-in, so instructions can show the steps for that DAW only.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace hearaside {

enum class Daw { StudioOne, Cubase, Reaper, FlStudio, Ableton, Other };

inline Daw currentDaw() {
    // $HEARASIDE_DAW (studioone / cubase / reaper / fl / ableton): screenshots and tests
    const auto env = juce::SystemStats::getEnvironmentVariable("HEARASIDE_DAW", {}).toLowerCase();
    if (env == "studioone") return Daw::StudioOne;
    if (env == "cubase") return Daw::Cubase;
    if (env == "reaper") return Daw::Reaper;
    if (env == "fl") return Daw::FlStudio;
    if (env == "ableton") return Daw::Ableton;
    if (env.isNotEmpty()) return Daw::Other;
    const juce::PluginHostType host;
    if (host.isStudioOne()) return Daw::StudioOne;
    if (host.isCubase() || host.isNuendo()) return Daw::Cubase;
    if (host.isReaper()) return Daw::Reaper;
    if (host.isFruityLoops()) return Daw::FlStudio;
    if (host.isAbletonLive()) return Daw::Ableton;
    return Daw::Other;
}

} // namespace hearaside
