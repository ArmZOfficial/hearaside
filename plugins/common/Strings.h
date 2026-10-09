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
    // Hub header
    MuteOff, MuteOn, ObsConnected, ObsConnecting, ObsNotConnected,
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
    StreamSolo, AdvancedSettings, RenameDisplay,
    Pan, Delay, Stem, StemNone, MonTrim, Close, Ms, Done, Cancel, NewName,
    // settings
    Settings, LanguageWord, ThemeWord, ThemeAuto, ThemeLight, ThemeDark, UiSize, ReduceMotion, GlassOpacity,
    TrackColours, ColoursFromDaw, ColoursMono, BusName, SyncSafety, SyncSafetyHint, Ceiling, StemNames, Blocks,
    // headphone level / viewers delay
    HeadphoneLevel, HeadphoneTip, HeadphoneMaster, HeadphoneMasterTip, ViewersDelay, DelayTip, Approx,
    DawBuffer, Samples, LatencyTip, MasterFx, TrackFx, ChainLatency,
    // App Audio
    AppPick, AppNone, AppStarting, AppRunning, AppNotRunning, AppFailed, AppHint, AppRecordTitle, AppOnly, AppOnlyTip,
    AppSubtitle, AppSystem, AppOff, AppLevel, AppDelay, PowerOn, PowerOff,
    PrintTitle, RecordStart, RecordStop, Recording, FollowRecord, FollowRecordTip, TakeDrag, TakeNone, OpenFolder,
    TakeDropped, PrintHint, SourcesTitle, SourcesSubtitle, RecordTip, PowerTip,
    // auto sync
    SyncButton, SyncTitle, SyncHow, SyncMic, SyncRef, SyncStart, SyncCancel, SyncMeasuringRef, SyncMeasuringMic,
    SyncLate, SyncEarly, SyncInTime, SyncNeedTracks, SyncNoAudio, SyncNoMusic, SyncNoMic, SyncUnsteady, SyncWeakMic, SyncCountdown, SyncOptions,
    // links (like LISTENTO)
    AppLinkIn, AppLinkFrom, AppLinkWaiting, AppLinkOffline, AppLinkAsk, AppLinkPrompt, AppLinkBad, AppLinkReceiving,
    ShareTip, ShareTitle, ShareOn, ShareListen, ShareListenCap, ShareSend, ShareSendCap, Copy, Copied, OpenLink,
    Listeners, SenderOn, SenderOff, TunnelReady, TunnelStarting, TunnelMissing, TunnelFailed, CopyInstall, SharingChip,
    // responsive Hub, onboarding
    PillPrefixYou, PillPrefixViewers, LevelsTitle, LevelsTip, SummaryOnlyViewers, StartTitle, StartStep1, StartStep2, StartStep3,
    StartHide, StartStep1StudioOne, StartStep1Cubase, StartStep1Reaper, StartStep1Fl, StartStep1Ableton,
    SyncTip, LevelsColumn, RowLevels,
    AppStepsStudioOne, AppStepsCubase, AppStepsReaper, AppStepsOther, AppHintNormal, StepsOtherDaws, StepsThisDaw,
    // permanent share links
    SharePermanent, ShareDirOnline, ShareDirRegistering, ShareDirOffline, ShareDirNeedsTunnel, ShareNoBase, ShareBackup,
    ShareBase, ShareBaseHint, PermanentLinks, RestApi, RestApiCap, CopyApiKey,
    // setup check
    SetupTitle, SetupNothing, SetupHubOk, SetupHubFix, SetupTracksOk, SetupTracksBad, SetupTracksYou, SetupObsFix,
    SetupSignalOk, SetupSignalBad, SetupSignalFix, SetupRateOk, SetupRateBad, SetupRateViewers, SetupRateFix,
    SetupBypassOk, SetupBypassBad, SetupBypassYou, SetupBypassFix, SetupAheadOk, SetupAheadBad,
    SetupAppOk, SetupAppYou, SetupAppFix, SetupShareOff, SetupShareLan, SetupShareOk, SetupShareFix,
    CopyReport, CopyReportTip, ViewersSilentBanner,
    Count
};

juce::String tr(Str key);
void setCurrentLanguage(Language);
Language currentLanguage();

// "−3.0 dB" / "−∞" / "+2.5 dB" with a real minus sign.
juce::String formatDb(float db, bool withUnit = true);

} // namespace hearaside
