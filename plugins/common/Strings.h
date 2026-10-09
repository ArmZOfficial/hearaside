// UI text in English (default) and Thai. Keys follow docs/plan.md section 7.8; EN copy matches the design export.
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
    // redesign (prompt 3.9 and the design export)
    YouHearTip, YouDontHearTip, ViewersHearTip, ViewersDontHearTip, YouHearAria, ViewersHearAria, YouDontHear,
    ViewersDontHear, DoubleClickToType, DoubleClickToReset, DoubleClickToResetMs, DoubleClickToRename,
    DoubleClickToCenter, PanTypeTip, PanCenterHint, NameInDaw, Back, BackTip, BackToTrack, Center, More, OptionsFor,
    Manage, Open, Connect, Change, Keep, TracksCount, MsValue, ObsChipTip, ObsOpen, ObsAddSource, ObsConnectedCap,
    SampleWord, Total, AboutMs, LineUpForFriends, ShareAudioTip, SettingsTip, AccountTip, SignInTip,
    ManageTracksTip, ColYou, ColViewers, LevelModeHeadphones, LevelModeViewers, LevelModeTip, RenameHint,
    FineSettingsEllipsis, RenamedToast, RenameRevertToast, ViewersDelayBadge, RecordingBadge, ProgramCaption,
    ChooseProgramAria, CaptureProgram, PrintToFile, StopRecording, RecordWithDaw, OpenRecordings,
    WholeComputerShort, AppOffShort, FriendsTitle, FriendsCount, FriendsCaption, FriendsCaptionNoLineUp,
    FriendsEmptyCaption, FriendLive, FriendSinging, FriendWaiting, FriendNotOpened, FriendOffline, CopyFriendLink,
    CopiedFriendLink, MeasureAgain, MeasureDelayAgain, MeasuredToast, RecordInDaw, AddFriend, RemoveFriend,
    SpreadOut, OnlyViewersLabel, SilentLufs, TabTracks, TabLevels, TabSummary, SlidersSet, ProgramsCount,
    FriendsCountShort, CompactSummary, CompactSummaryMuted, MoreMenuAria, ObsShort, ObsAriaCompact, SharePageTitle,
    SharePageSubtitle, SpreadOutTip, FriendDefaultName, LineUpFriends, LineUpOff, LineUpWaiting, LineUpCaption,
    Measuring, DelayMsLabel, CopyLink, RemoveNamed, Volume, VolumeTip, PanWord, NoFriends, NoFriendsCaption,
    FriendsFooter, SendInLegacy, SendInLegacyCap, ListenLinkCap, ListeningCount, ConnectionTitle, LinksAnywhere,
    LinksAnywhereCap, LinksChangeCap, LinksStarting, WifiOnly, WifiOnlyCap, SharingOff, SharingOffCap, ReceiveTitle,
    ReceiveCap, ReceiveNoApp, LinkToReceive, SettingsSubtitle, SettingsSections, SecAppearance, SecAppearanceCap,
    SecAudio, SecAudioCap, SecConnection, SecConnectionCap, SecAbout, SecAboutCap, SettingsScope, LanguageCap,
    ThemeCap, UiSizeCap, GlassCap, ColoursCap, ReduceMotionCap, CeilingCap, StemNamesCap, StemNameN, StemN,
    LineUpLimit, LineUpLimitCap, ObsRowCap, NotConnected, ConnectedWord, BusNameCap, DelayToObsCap, ShareBaseCap,
    RestApiCapShort, AppTagline, VersionN, GettingStarted, GettingStartedCap, ShowAgain, ResetSettings,
    ResetSettingsCap, ResetWord, SetupCheckCap, SetupMastering, TrackFineTitle, TrackFineSubtitle, PrevTrack,
    NextTrack, ChooseTrack, TrackNOfM, ViewersCard, ViewersCardCap, PanCap, DelayCapFine, MeasureAuto, SoloCap,
    HeadphonesCard, HeadphoneLevelCap, TrackInfo, NameShown, StatusWord, Running, NotRunning, SampleRate,
    MatchesHub, DiffersHub, BypassedWord, SyncSubtitle, SyncStep1, SyncStep2, SyncStep3, SyncCheck1, SyncCheck2,
    SyncCheck3, SyncStartMeasuring, SyncTakes, SyncViewersNothing, SyncViewersUntil, SyncStartingIn, SyncLateBy,
    SyncEarlyBy, SyncFixedMusic, SyncFixedVocal, SyncCheckWith, SyncFailedTitle, CurrentDelays, SetDelaysByHand,
    IfItDoesntWork, TipSilentTitle, TipSilent, TipNoMicTitle, TipNoMic, TipWeakTitle, TipWeak, TipUnsteadyTitle,
    TipUnsteady, TracksPageTitle, TracksPageSubtitle, FilterAll, FilterYou, FilterViewers, FilterSilent, FilterTip,
    ClearSolo, ResetLevelsPan, ColHpLevel, ColVwLevel, ColPan, ColDelay, ColStem, ColSolo, NoTracksFilter,
    TracksFooter, ResetLevelsToast, ProgramsPageTitle, ProgramsPageSubtitle, AppAudioWord, InThisProject, OnChannel,
    ChooseSource, OffChannel, FriendTakeShift, DelayViewers, ProgramOnly, ProgramOnlyCap, AddSourceHint, HowToSetUp,
    RecordedTakes, RecordedTakesCap, RecordingTime, StopTime, TakeName, TakeMetaToday, TakeMetaDay, DragWord,
    DragTakeTip, TakesFolderHint, NoAppAudio, FineSettings, RenameTrack, RenameCap, BusNameTrackCap,
    DawBufferFooter, DawBufferFooterTip, TrackDelayCap, ViewersDelayMsAria, SetUpIn, Detected, OtherDaws,
    StudioOneStep1, StudioOneStep2, StudioOneStep3, StudioOneStep4, CubaseStep1, CubaseStep2, ReaperStep1,
    ReaperStep2, OtherDawStep1, OtherDawStep2, CubaseOther, ReaperOther, StudioOneOther, AppNormalTrackNote,
    AppOffTrack, AppDelayCap, ProgramCapture, AccountEllipsis, DelayInMs, SentInLegacy, SourceThisTrack,
    SourceBringFriend, SourcePasteLink, SourceInviteFriend, SourceNeedsHub, FriendItem, FriendInTrack,
    FriendLevelIn, KeepOwnSound, KeepOwnSoundCap, FriendPaired, FriendNotPaired, FriendStepBelow, FriendStepEnd,
    FriendDawPaused, FriendDawPausedCubase, FriendInDawTrack, FriendBackToHub, MixInDawTrack, BringBackToHub,
    GoToTrack, MixedInDaw, InDawNotHeadphones, SingingInDaw, SetLevelOnTrack, FriendBadge, FriendLinedUp,
    FriendNotLinedUp, FriendThroughTrack, FriendWord, GoesOutThrough, FriendHearSummary, StopBringing, FriendMoved,
    MoveHere, LinkCopiedSendTo, LinkNotHearaside, LinkOtherRoom, LinkListenNotLinedUp, FriendGone, RecordFriendHint,
    MixFriendTitle, MixFriendStep1, MixFriendStep2, MixFriendStep3, RemovedFriendToast, FriendOverLimit, FriendFullToast, LineUpOnToast, LineUpChangedToast, LineUpOffToast, AccountTitle, AccountOptional, SecAccountCap,
    SignInBrowser, CreateAccount, SignOut, SignInEllipsis, NotSignedIn, EditArrow, EnterCode, WaitingAllow,
    CodeExpired, GetNewCode, CodeAt, OpenThePage, CopyCode, AccountPhoto, AccountPhotoCap, ChangeEllipsis,
    PhotoEllipsis, RemoveWord, AccountDisplayName, AccountDisplayNameCap, AccountUsername, UsernameAvailable,
    UsernameRules, UsernameTaken, UsernameFree, AccountAbout, AboutCounter, SyncAppearance, SyncAppearanceCap,
    SyncAppearanceCapShort, HideEmailOnScreen, HideEmailCap, ThisComputer, ThisComputerCap, ManageOnWeb, MoreOnWeb,
    SignOutConfirm, AccountOffline, SignInHereAll, BenefitLinks, BenefitFriends, BenefitAppearance,
    BenefitLinksShort, BenefitFriendsShort, BenefitAppearanceShort, InvitedYou, AppearanceSynced, SignedInToast,
    SignedOutToast,
    DawYours,
    Count
};

juce::String tr(Str key);
// tr() with "%s" / "%d" filled in order: trf(Str::FriendsCaption, { "2", "320" })
juce::String trf(Str key, std::initializer_list<juce::String> args);
void setCurrentLanguage(Language);
Language currentLanguage();

// "−3.0 dB" / "−∞ dB" / "+2.5 dB" with a real minus sign (valuetext::formatDb).
juce::String formatDb(float db, bool withUnit = true);

} // namespace hearaside
