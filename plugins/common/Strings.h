// UI text in Thai (default) and English. Keys follow docs/plan.md section 7.8.
#pragma once

#include <juce_core/juce_core.h>

namespace hearaside {

enum class Language;

enum class Str {
    // shared vocabulary
    MonLabel, StrLabel, StateOn, StateOff, None,
    // Track
    TrackWord, MonRowTitle, MonRowCaption, StrRowTitle, StrRowCaption, InputSignal, ViewersLevel,
    SumBoth, SumViewersOnly, SumYouOnly, SumSilent,
    HubConnected, HubMissingChip, HubMissingBanner, HubBypassedChip, SlotsFull, BusError,
    BypassWarning, RateMismatchTrack, PanicActiveChip,
    // Hub header / scenes
    SceneSinging, SceneTalking, SceneBrb, SceneCustom, MuteOff, MuteOn, ObsConnected, ObsConnecting, ObsNotConnected,
    // Hub tracks card
    TracksTitle, TracksSubtitle, ColTrack, BannerPreview, BannerPanic, NoTracks,
    Inactive, InactiveTip, RateMismatch, AheadWarning, BypassedTip, SoloBadge,
    // Hub stream card
    StreamTitle, StreamSubtitle, Loudness, LoudSilent, LoudMuted, Master, Limiter, LimiterCaption,
    PreviewOff, PreviewOn,
    // Hub summary card
    SummaryTitle, SummaryYou, SummaryViewers, SummaryPreviewing, SummaryPanic, LatencyToObs, ObsHint,
    // Hub other states
    SecondHubTitle, SecondHubBody, HubBusError,
    // context menus / advanced
    StreamSolo, AdvancedSettings, RenameDisplay, SaveToScene, RenameScene, ClearScene,
    Pan, Delay, Stem, StemNone, MonTrim, Close, Ms, Done, Cancel, NewName,
    // settings
    Settings, LanguageWord, ThemeWord, ThemeAuto, ThemeLight, ThemeDark, UiSize, ReduceMotion, GlassOpacity,
    TrackColours, ColoursFromDaw, ColoursMono, BusName, SyncSafety, SyncSafetyHint, Ceiling, StemNames, Blocks,
    // headphone level / viewers delay
    HeadphoneLevel, HeadphoneTip, HeadphoneMaster, HeadphoneMasterTip, ViewersDelay, DelayTip, SaveAsNewScene, Approx, SceneHint,
    // mastering for viewers
    Mastering, MasteringNone, MasteringCount, Manage, AddPlugin, SearchPlugins, PluginLoadFailed, PluginInstrument,
    MasteringFull, MasteringHint, Bypass, NoPluginsFound, Loading,
    Count
};

juce::String tr(Str key);
void setCurrentLanguage(Language);
Language currentLanguage();

// "−3.0 dB" / "−∞" / "+2.5 dB" with a real minus sign.
juce::String formatDb(float db, bool withUnit = true);

} // namespace hearaside
