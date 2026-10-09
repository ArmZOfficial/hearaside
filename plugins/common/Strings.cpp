#include "Strings.h"
#include "Settings.h"
#include "ValueText.h"

#include <array>
#include <atomic>

namespace hearaside {

namespace {

std::atomic<int> gLanguage { 1 };   // 0 = Thai, 1 = English (the default)

struct Entry { const char* th = nullptr; const char* en = nullptr; };
using Table = std::array<Entry, size_t(Str::Count)>;

Table build() {
    Table t {};
    auto s = [&t](Str k, const char* th, const char* en) { t[size_t(k)] = { th, en }; };

    s(Str::MonLabel, "คุณได้ยิน", "You hear");
    s(Str::StrLabel, "คนดูได้ยิน", "Viewers hear");
    s(Str::StateOn, "ได้ยิน", "On");
    s(Str::StateOff, "ไม่ได้ยิน", "Off");
    s(Str::None, "ไม่มี", "Nothing");

    s(Str::TrackWord, "แทร็ก", "Track");
    s(Str::MonRowTitle, "คุณได้ยินแทร็กนี้", "You hear this track");
    s(Str::MonRowCaption, "ในหูฟังของคุณ", "In your headphones");
    s(Str::StrRowTitle, "คนดูได้ยินแทร็กนี้", "Viewers hear this track");
    s(Str::StrRowCaption, "ส่งไปไลฟ์ผ่าน OBS", "Sent to your stream via OBS");
    s(Str::InputSignal, "สัญญาณเข้า", "Input");
    s(Str::ViewersLevel, "ระดับเสียงฝั่งคนดู", "Viewers level");
    s(Str::SumBoth, "ทั้งคุณและคนดูได้ยินแทร็กนี้", "Both you and your viewers hear this track.");
    s(Str::SumViewersOnly, "คุณจะไม่ได้ยินแทร็กนี้ในหูฟัง แต่คนดูได้ยินตามปกติ",
      "You won’t hear this track in your headphones, but your viewers will.");
    s(Str::SumYouOnly, "คุณได้ยินคนเดียว คนดูจะไม่ได้ยินแทร็กนี้", "Only you hear this track. Viewers won’t.");
    s(Str::SumSilent, "แทร็กนี้เงียบทั้งสองฝั่ง แต่ DAW ยังอัดเสียงได้ตามปกติ",
      "This track is silent on both sides, but your DAW still records it.");
    s(Str::HubConnected, "เชื่อมกับ Hub แล้ว", "Connected to Hub");
    s(Str::HubMissingChip, "ยังไม่มี Hub", "No Hub yet");
    s(Str::HubMissingBanner, "ใส่ HEARASIDE Hub ที่ Master เพื่อส่งเสียงไปหาคนดู",
      "Add HEARASIDE Hub on your master bus to send audio to viewers.");
    s(Str::HubBypassedChip, "Hub ถูก bypass", "Hub bypassed");
    s(Str::SlotsFull, "ใส่ได้สูงสุด 64 แทร็กต่อ Bus", "A bus holds up to 64 tracks.");
    s(Str::BusError, "เปิดช่องสื่อสารกับ Hub ไม่ได้ ลองปิดแล้วเปิดปลั๊กอินใหม่",
      "Could not open the HEARASIDE bus. Try removing and re-adding the plug-in.");
    s(Str::BypassWarning, "ปลั๊กอินถูก bypass อยู่ คุณจะได้ยินแทร็กนี้แม้ปิด \"คุณได้ยิน\"",
      "The plug-in is bypassed, so you hear this track even with “You hear” off.");
    s(Str::RateMismatchTrack, "Sample rate ไม่ตรงกับ Hub แทร็กนี้จึงไม่ถูกส่งไปหาคนดู",
      "Sample rate differs from the Hub, so viewers don’t get this track.");
    s(Str::PanicActiveChip, "ตัดเสียงคนดูอยู่", "Stream muted");

    s(Str::MuteOff, "ตัดเสียงคนดู", "Mute stream");
    s(Str::MuteOn, "กดเพื่อคืนเสียง", "Unmute stream");
    s(Str::ObsConnected, "OBS เชื่อมต่อแล้ว", "OBS connected");
    s(Str::ObsConnecting, "กำลังเชื่อม OBS", "Connecting to OBS");
    s(Str::ObsNotConnected, "OBS ยังไม่เชื่อม", "OBS not connected");

    s(Str::TracksTitle, "แทร็กทั้งหมด", "All tracks");
    s(Str::TracksSubtitle, "เลือกแยกกันได้ ว่าแทร็กไหนคุณได้ยิน และแทร็กไหนคนดูได้ยิน",
      "Choose separately what you hear and what your viewers hear.");
    s(Str::ColTrack, "แทร็ก", "Track");
    s(Str::BannerPreview, "คุณกำลังฟังแบบคนดู หูฟังจะได้ยินเหมือนเสียงที่ส่งไป OBS",
      "You’re hearing the viewers’ mix. Your headphones get exactly what goes to OBS.");
    s(Str::BannerPanic, "ตัดเสียงไปหาคนดูอยู่ คนดูจะไม่ได้ยินอะไรจนกว่าคุณจะกดคืนเสียง",
      "The stream is muted. Viewers hear nothing until you unmute.");
    s(Str::NoTracks, "ยังไม่มีแทร็กที่ใส่ HEARASIDE Track ใส่ที่ท้าย FX ของแทร็กที่ต้องการ แล้วจะขึ้นที่นี่เอง",
      "No tracks yet. Put HEARASIDE Track last in a track’s FX chain and it shows up here.");
    s(Str::Inactive, "ไม่ทำงาน", "Inactive");
    s(Str::InactiveTip, "DAW ไม่ได้ประมวลผลแทร็กนี้อยู่", "The DAW isn’t processing this track right now.");
    s(Str::RateMismatch, "Sample rate ไม่ตรงกับ Hub แทร็กนี้จึงไม่ถูกส่งไปหาคนดู",
      "Sample rate differs from the Hub, so viewers don’t get this track.");
    s(Str::AheadWarning, "DAW ประมวลผลแทร็กนี้ล่วงหน้า เสียงอาจไม่ตรงกับแทร็กอื่น",
      "The DAW processes this track ahead of time; it may be out of sync with other tracks.");
    s(Str::BypassedTip, "ปลั๊กอินในแทร็กนี้ถูก bypass คุณจะได้ยินแทร็กนี้เสมอ",
      "The plug-in on this track is bypassed, so you always hear it.");
    s(Str::SoloBadge, "Solo", "Solo");

    s(Str::StreamTitle, "เสียงที่คนดูได้ยิน", "What viewers hear");
    s(Str::StreamSubtitle, "ผ่านปลั๊กอินบน Master ที่อยู่เหนือ Hub", "Through the master plug-ins above the Hub");
    s(Str::Loudness, "ความดัง (LUFS)", "Loudness (LUFS)");
    s(Str::LoudSilent, "เงียบ", "Silent");
    s(Str::LoudMuted, "ตัดอยู่", "Muted");
    s(Str::Master, "ระดับรวมไปหาคนดู", "Stream level");
    s(Str::Limiter, "กันเสียงพีค", "Peak protection");
    s(Str::LimiterCaption, "ไม่ให้เสียงแตกไปถึงคนดู", "Keeps clipping away from viewers");
    s(Str::PreviewOff, "ฟังแบบคนดู", "Hear viewers’ mix");
    s(Str::PreviewOn, "กลับไปฟังแบบปกติ", "Back to my mix");

    s(Str::SummaryTitle, "สรุปตอนนี้", "Right now");
    s(Str::SummaryYou, "คุณได้ยินในหูฟัง", "In your headphones");
    s(Str::SummaryViewers, "คนดูได้ยิน", "Viewers hear");
    s(Str::SummaryPreviewing, "เหมือนที่คนดูได้ยิน (กำลังฟังแบบคนดู)", "Same as viewers (previewing)");
    s(Str::SummaryPanic, "ไม่มี เพราะกำลังตัดเสียงอยู่", "Nothing - the stream is muted");
    s(Str::LatencyToObs, "เสียงถึง OBS ช้ากว่าจริง", "Delay to OBS");
    s(Str::ObsHint, "OBS ยังไม่เชื่อม เปิด OBS แล้วเพิ่ม Source ชื่อ HEARASIDE",
      "OBS isn’t connected. Open OBS and add a source called HEARASIDE.");

    s(Str::SecondHubTitle, "มี Hub ทำงานอยู่แล้ว", "Another Hub is active");
    s(Str::SecondHubBody, "มี Hub ทำงานอยู่แล้วในโปรเจกต์นี้ ตัวนี้จะปล่อยเสียงผ่านอย่างเดียว ใช้ Hub ตัวเดียวบน Master ก็พอ",
      "A Hub is already running in this project. This one only passes audio through; one Hub on the master bus is enough.");
    s(Str::HubBusError, "เปิดช่องสื่อสารไม่ได้ Hub จะปล่อยเสียงผ่านอย่างเดียว",
      "Could not open the HEARASIDE bus. The Hub only passes audio through.");

    s(Str::StreamSolo, "Solo ฝั่งคนดู", "Solo for viewers");
    s(Str::AdvancedSettings, "ตั้งค่าละเอียด", "Fine settings");
    s(Str::RenameDisplay, "เปลี่ยนชื่อที่แสดง", "Rename");
    s(Str::Pan, "แพน (ฝั่งคนดู)", "Pan (viewers)");
    s(Str::Delay, "หน่วงเวลา (ฝั่งคนดู)", "Delay (viewers)");
    s(Str::Stem, "ส่งเป็น Stem", "Send to stem");
    s(Str::StemNone, "ไม่ส่ง", "None");
    s(Str::MonTrim, "ปรับระดับฝั่งหูฟัง", "Headphone trim");
    s(Str::Close, "ปิด", "Close");
    s(Str::Ms, "ms", "ms");
    s(Str::Done, "ตกลง", "OK");
    s(Str::Cancel, "ยกเลิก", "Cancel");
    s(Str::NewName, "ชื่อใหม่", "New name");

    s(Str::Settings, "ตั้งค่า", "Settings");
    s(Str::LanguageWord, "ภาษา", "Language");
    s(Str::ThemeWord, "ธีม", "Theme");
    s(Str::ThemeAuto, "ตามระบบ", "System");
    s(Str::ThemeLight, "สว่าง", "Light");
    s(Str::ThemeDark, "มืด", "Dark");
    s(Str::UiSize, "ขนาดหน้าจอ", "UI size");
    s(Str::ReduceMotion, "ลดการเคลื่อนไหว", "Reduce motion");
    s(Str::GlassOpacity, "ความขุ่นของกระจก", "Glass opacity");
    s(Str::TrackColours, "สีแทร็ก", "Track colours");
    s(Str::ColoursFromDaw, "สีจาก DAW", "From DAW");
    s(Str::ColoursMono, "โมโนโครม", "Monochrome");
    s(Str::BusName, "ชื่อ Bus", "Bus name");
    s(Str::SyncSafety, "เผื่อเวลา sync", "Sync safety");
    s(Str::SyncSafetyHint, "เพิ่มเมื่อบางแทร็กในหูฟังเหลื่อมกัน (ทุก 1 block = หูฟังช้าลง 1 buffer)",
      "Raise if some tracks drift apart in your headphones (each block adds one buffer of delay there)");
    s(Str::Ceiling, "เพดานกันเสียงพีค", "Peak ceiling");
    s(Str::StemNames, "ชื่อ Stem", "Stem names");
    s(Str::Blocks, "block", "block");
    s(Str::HeadphoneLevel, "ระดับในหูฟัง", "Headphone level");
    s(Str::HeadphoneTip, "ปรับเสียงแทร็กนี้ที่คุณได้ยินในหูฟังเท่านั้น คนดูยังได้ยินเท่าเดิม และไม่ต้องแตะ fader ใน DAW",
      "Changes only what you hear in your headphones. Viewers hear the same, and your DAW faders stay put.");
    s(Str::HeadphoneMaster, "ระดับหูฟังรวม", "Headphone master");
    s(Str::HeadphoneMasterTip, "ลดเสียงทั้งหมดที่คุณได้ยิน โดยไม่กระทบเสียงที่ส่งไปหาคนดู (ไม่มีผลตอน export)",
      "Turns down everything you hear without touching what viewers get");
    s(Str::ViewersDelay, "หน่วงเวลาฝั่งคนดู", "Viewers delay");
    s(Str::DelayTip, "เหมือน Sync Offset ใน OBS แต่ทำทีละแทร็ก เช่น ปลั๊กอินร้องหน่วง 40 ms ให้ตั้งแทร็กดนตรีเป็น 40 ms เสียงร้องกับดนตรีจะตรงกันฝั่งคนดู หูฟังของคุณไม่ช้าลง",
      "Like OBS sync offset, per track. If your vocal chain adds 40 ms, set the backing track to 40 ms so voice and music line up for viewers. Your headphones are not delayed.");
    s(Str::Approx, "ประมาณ", "about");
    s(Str::MasterFx, "ปลั๊กอิน Master", "Master plug-ins");
    s(Str::TrackFx, "ปลั๊กอินแทร็ก", "Track plug-ins");
    s(Str::AppPick, "เลือกโปรแกรมที่จะดึงเสียง", "Choose a program");
    s(Str::AppNone, "ยังไม่ได้เลือกโปรแกรม", "No program chosen");
    s(Str::AppStarting, "กำลังเชื่อม...", "Connecting…");
    s(Str::AppRunning, "รับเสียงอยู่", "Capturing");
    s(Str::AppNotRunning, "โปรแกรมนี้ยังไม่เปิด จะเริ่มรับเสียงเองเมื่อเปิด", "This program isn’t running; capture starts when it opens");
    s(Str::AppFailed, "รับเสียงไม่ได้ (ต้องใช้ Windows 10 2004 ขึ้นไป)", "Can’t capture (needs Windows 10 2004 or newer)");
    s(Str::AppHint, "ใส่ปลั๊กอินนี้ที่ช่อง Input แล้วให้แทร็กรับเสียงจาก Input นั้น ปุ่มอัดของ DAW จะอัดเสียงโปรแกรมได้เลย",
      "Put this plug-in on an input channel and record a track from that input: the DAW’s own Record button records the program.");
    s(Str::AppStepsStudioOne, "Studio One:\n1. Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo) ตั้งชื่อ App Audio\n2. ใน Console เปิดช่อง Inputs แล้วใส่ปลั๊กอินนี้ที่ช่อง App Audio\n3. เปิด \"เฉพาะเสียงโปรแกรม\" ข้างบน\n4. ตั้งขาเข้าของแทร็ก Stereo เป็น App Audio แล้วกดอัด",
      "Studio One:\n1. Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo), name it App Audio\n2. In the Console show the Inputs and put this plug-in on App Audio\n3. Turn on “Program only” above\n4. Set a stereo track’s input to App Audio and record");
    s(Str::AppStepsCubase, "Cubase:\n1. ใส่ปลั๊กอินนี้ที่ Input Channel ใน MixConsole\n2. เปิด \"เฉพาะเสียงโปรแกรม\" ข้างบน\n3. ตั้งขาเข้าของแทร็กเป็น Input นั้นแล้วกดอัด",
      "Cubase:\n1. Put this plug-in on an Input Channel in the MixConsole\n2. Turn on “Program only” above\n3. Set a track’s input to that input and record");
    s(Str::AppStepsReaper, "Reaper:\n1. ใส่ปลั๊กอินนี้ใน Input FX ของแทร็กที่จะอัด\n2. เปิด \"เฉพาะเสียงโปรแกรม\" ข้างบน\n3. Arm แทร็กแล้วกดอัด",
      "Reaper:\n1. Put this plug-in in the Input FX of the track you record on\n2. Turn on “Program only” above\n3. Arm the track and record");
    s(Str::AppStepsOther, "DAW นี้: ถ้ามีช่อง Input ให้ใส่ปลั๊กอินนี้ที่นั่น เปิด \"เฉพาะเสียงโปรแกรม\" แล้วอัดแทร็กจาก Input นั้น",
      "This DAW: if it has input channels, put this plug-in there, turn on “Program only” and record a track from that input");
    s(Str::AppHintNormal, "ถ้าใส่ในแทร็กปกติ คุณและคนดูได้ยินเสียงโปรแกรม แต่ DAW ไม่อัด", "On a normal track you and the viewers hear it, but the DAW doesn’t record it.");
    s(Str::StepsOtherDaws, "ดูวิธีของ DAW อื่น", "Steps for other DAWs");
    s(Str::StepsThisDaw, "ดูเฉพาะ DAW นี้", "Only this DAW");
    s(Str::SharePermanent, "ลิงก์ถาวร ส่งครั้งเดียวใช้ได้ทุกไลฟ์", "Permanent link - send it once, use it every stream");
    s(Str::ShareDirOnline, "ลิงก์ถาวรออนไลน์ ส่งครั้งเดียวใช้ได้ทุกไลฟ์", "Permanent link online - send it once, use it every stream");
    s(Str::ShareDirRegistering, "กำลังเชื่อมลิงก์ถาวร...", "Connecting the permanent link…");
    s(Str::ShareDirOffline, "ลิงก์ถาวรใช้ไม่ได้ชั่วคราว ใช้ลิงก์สำรองไปก่อน กำลังลองใหม่...", "The permanent link is unavailable right now - use the backup link; retrying…");
    s(Str::ShareDirNeedsTunnel, "ลิงก์ถาวรต้องใช้ cloudflared: ติดตั้งตามข้างล่าง แล้วปิดเปิดแชร์ใหม่ ระหว่างนี้ใช้ได้เฉพาะ WiFi เดียวกัน",
      "The permanent link needs cloudflared: install it (below), then switch sharing off and on. Wi-Fi only meanwhile");
    s(Str::ShareNoBase, "ลิงก์นี้เปลี่ยนทุกครั้งที่เปิดแชร์ ตั้ง \"ที่อยู่เว็บแชร์\" ในการตั้งค่า (ปุ่มเฟือง) เพื่อได้ลิงก์ถาวร",
      "This link changes every time you share. Set the “Share web address” in Settings (gear) for a permanent link");
    s(Str::ShareBackup, "ลิงก์สำรองของวันนี้ (ใช้ตอนลิงก์ถาวรมีปัญหา)", "Today’s backup link (if the permanent one has trouble)");
    s(Str::ShareBase, "ที่อยู่เว็บแชร์", "Share web address");
    s(Str::ShareBaseHint, "เว็บ Vercel ของคุณ (web/share-vercel)", "Your Vercel site (web/share-vercel)");
    s(Str::PermanentLinks, "ลิงก์ถาวร", "Permanent links");
    s(Str::RestApi, "REST API (Stream Deck, สคริปต์)", "REST API (Stream Deck, scripts)");
    s(Str::RestApiCap, "สั่ง Hub จากโปรแกรมในเครื่องนี้ (docs/rest-api.md)", "Control the Hub from programs on this computer (docs/rest-api.md)");
    s(Str::CopyApiKey, "คัดลอก API key", "Copy API key");
    s(Str::SetupTitle, "ตรวจระบบ", "Setup check");
    s(Str::SetupNothing, "ไม่ได้ยินอะไรจาก DAW", "nothing from the DAW");
    s(Str::SetupHubOk, "Hub ทำงานอยู่ (ตัวเดียวบน bus นี้)", "The Hub is running (the only one on this bus)");
    s(Str::SetupHubFix, "ใช้ Hub ตัวเดียวบน Master แล้วถอดตัวอื่นออก", "Keep one Hub on the master bus and remove the others");
    s(Str::SetupTracksOk, "มี %n แทร็กที่ต่อกับ Hub", "%n tracks are connected to the Hub");
    s(Str::SetupTracksBad, "ยังไม่มีแทร็กที่ใส่ HEARASIDE Track", "No track has HEARASIDE Track yet");
    s(Str::SetupTracksYou, "หูฟังยังไม่มีเสียงจากแทร็กไหนผ่าน Hub", "your headphones get no track through the Hub yet");
    s(Str::SetupObsFix, "เปิด OBS แล้วเพิ่ม Source ชื่อ HEARASIDE (Monitoring จะปิดให้เอง) ถ้า DAW เล่นผ่าน Desktop Audio ให้ปิด Desktop Audio ใน OBS ด้วย",
      "Open OBS and add a HEARASIDE source (monitoring is switched off for you). If the DAW plays through Desktop Audio, mute Desktop Audio in OBS too");
    s(Str::SetupSignalOk, "เสียงไปถึงคนดูตามปกติ", "Sound reaches the viewers");
    s(Str::SetupSignalBad, "คนดูไม่ได้ยินอะไรเลย ทั้งที่ \"%t\" มีเสียงอยู่", "The viewers hear nothing although “%t” is playing");
    s(Str::SetupSignalFix, "ตรวจว่า Hub อยู่บน Master, fader ของแทร็กและ Master ไม่ได้ลดสุด, ปลั๊กอินเหนือ Hub ไม่ได้ปิดเสียง",
      "Check that the Hub is on the master bus, the track and master faders are up, and no plug-in above the Hub mutes the sound");
    s(Str::SetupRateOk, "Sample rate ตรงกันทุกแทร็ก", "Every track runs at the Hub’s sample rate");
    s(Str::SetupRateBad, "Sample rate ไม่ตรงกับ Hub:", "Sample rate differs from the Hub:");
    s(Str::SetupRateViewers, "ไม่ได้ยินแทร็กเหล่านี้", "don’t get these tracks");
    s(Str::SetupRateFix, "ตั้ง sample rate ของโปรเจกต์ให้เท่ากันทั้งหมด", "Use one sample rate for the whole project");
    s(Str::SetupBypassOk, "ไม่มี HEARASIDE Track ที่ถูก bypass", "No HEARASIDE Track is bypassed");
    s(Str::SetupBypassBad, "HEARASIDE Track ถูก bypass:", "HEARASIDE Track is bypassed on:");
    s(Str::SetupBypassYou, "ได้ยินแทร็กนี้เสมอ แม้ปิด \"คุณได้ยิน\"", "you always hear it, even with “You hear” off");
    s(Str::SetupBypassFix, "เลิก bypass ปลั๊กอินบนแทร็กนั้น", "Un-bypass the plug-in on that track");
    s(Str::SetupAheadOk, "ทุกแทร็กประมวลผลพร้อมกัน", "Every track is processed in time");
    s(Str::SetupAheadBad, "DAW ประมวลผลล่วงหน้า:", "Processed ahead of time:");
    s(Str::SetupAppOk, "App Audio รับเสียงได้", "App Audio can capture");
    s(Str::SetupAppYou, "ไม่ได้ยินเสียงจากโปรแกรม", "no program sound");
    s(Str::SetupAppFix, "อัปเดต Windows เป็น 10 เวอร์ชัน 2004 ขึ้นไป", "Update Windows to 10 version 2004 or newer");
    s(Str::SetupShareOff, "ไม่ได้แชร์ลิงก์อยู่", "Not sharing");
    s(Str::SetupShareLan, "แชร์ได้เฉพาะ WiFi เดียวกัน", "Sharing works on your Wi-Fi only");
    s(Str::SetupShareOk, "แชร์ลิงก์ใช้ได้ผ่านเน็ต", "The share links work over the internet");
    s(Str::SetupShareFix, "ส่งลิงก์สำรองในหน้าแชร์ไปก่อน Hub จะลองเชื่อมลิงก์ถาวรใหม่เอง", "Send the backup link from the share panel; the Hub keeps retrying the permanent one");
    s(Str::CopyReport, "คัดลอกรายงานปัญหา", "Copy problem report");
    s(Str::CopyReportTip, "DAW, Windows, buffer, สถานะ OBS และแทร็ก (ไม่มีชื่อแทร็ก ลิงก์ หรือ key) ไว้ส่งให้ทีมช่วยดู",
      "DAW, Windows, buffer, OBS and track state (no track names, links or keys) to send for help");
    s(Str::ViewersSilentBanner, "คนดูไม่ได้ยินอะไรเลย ทั้งที่มีแทร็กที่ส่งไปหาคนดูกำลังมีเสียง · แตะเพื่อดูวิธีแก้",
      "The viewers hear nothing although a track for them is playing · tap to see how to fix it");
    s(Str::AppRecordTitle, "อัดด้วยปุ่มอัดของ DAW", "Record with the DAW");
    s(Str::AppOnly, "เฉพาะเสียงโปรแกรม (ตัดเสียงเดิมของช่อง)", "Program only (drop the channel’s own sound)");
    s(Str::AppOnlyTip, "ใช้กับช่อง Input ที่ทำไว้อัดเสียงโปรแกรม: ไม่มีเสียงไมค์หรือเสียงอื่นปนในไฟล์ที่อัด",
      "For an input channel made for the program: no mic or other sound ends up in the recording");
    s(Str::AppSubtitle, "ดึงเสียงจากโปรแกรมเข้าแทร็กนี้", "Brings a program’s sound into this track");
    s(Str::AppSystem, "เสียงทั้งเครื่อง (ยกเว้น DAW)", "Whole computer (except the DAW)");
    s(Str::AppOff, "ปิดอยู่ แทร็กนี้ปล่อยเสียงเดิมผ่านอย่างเดียว", "Off · the track passes its own sound only");
    s(Str::AppLevel, "ระดับเสียงโปรแกรม", "Program level");
    s(Str::AppDelay, "หน่วง", "Delay");
    s(Str::PowerOn, "เปิด", "On");
    s(Str::PowerOff, "ปิด", "Off");
    s(Str::PrintTitle, "อัดเสียงลงไฟล์ (Print)", "Print to a file");
    s(Str::RecordStart, "อัดเสียง", "Record");
    s(Str::RecordStop, "หยุดอัด", "Stop");
    s(Str::Recording, "กำลังอัด", "Recording");
    s(Str::FollowRecord, "อัดเองตอน DAW อัด", "Record with the DAW");
    s(Str::FollowRecordTip, "กดอัดใน DAW เมื่อไร ก็อัดเสียงโปรแกรมเป็นไฟล์แยกให้ด้วย ตรงตำแหน่งบน timeline",
      "Whenever the DAW records, the program’s sound is printed to its own take, lined up with the timeline");
    s(Str::TakeDrag, "ลากไปวางในแทร็ก", "Drag into a track");
    s(Str::TakeNone, "ยังไม่มีไฟล์ที่อัด", "No takes yet");
    s(Str::OpenFolder, "เปิดโฟลเดอร์", "Open folder");
    s(Str::TakeDropped, "ดิสก์ช้า เสียงบางช่วงหาย", "Disk too slow - some audio was lost");
    s(Str::PrintHint, "ไฟล์ WAV เก็บที่ Documents\\HEARASIDE\\Recordings ลากไปวางแทร็กไหนก็ได้ ใน Cubase / Reaper ใช้ Move to origin จะวางตรงตำแหน่งที่อัด",
      "WAV takes go to Documents\\HEARASIDE\\Recordings. Drag one onto any track; in Cubase / Reaper “Move to origin” puts it where it was recorded.");
    s(Str::SourcesTitle, "เสียงจากโปรแกรม", "Program audio");
    s(Str::SourcesSubtitle, "HEARASIDE App Audio ทุกตัว เลือกได้ว่าคุณหรือคนดูได้ยิน เปิดปิดและอัดเสียงที่ปุ่ม ...",
      "Every HEARASIDE App Audio: choose who hears it; switch and record from the … button");
    s(Str::RecordTip, "อัดเสียงโปรแกรมลงไฟล์ WAV แล้วลากไปวางในแทร็ก", "Print the program’s sound to a WAV take you can drag into a track");
    s(Str::SyncButton, "ซิงค์เสียงร้องอัตโนมัติ", "Auto-sync vocal");
    s(Str::SyncTitle, "ซิงค์เสียงร้องกับเพลง (ฝั่งคนดู)", "Line the vocal up with the music (viewers)");
    s(Str::SyncHow, "1. เปิดเพลงไว้ (อย่าเพิ่งร้อง)\n2. กดเริ่ม แล้วเอาหูฟังจ่อไมค์ราว 7 วินาที ระหว่างนี้คนดูจะไม่ได้ยินเสียง\n3. ระบบหน่วงเพลงหรือเสียงร้องฝั่งคนดูให้ตรงกันเอง หูฟังของคุณเหมือนเดิม\nกดใหม่เมื่อเพิ่มหรือถอดปลั๊กอินที่มีดีเลย์ หรือเปลี่ยน buffer",
      "1. Play the music (don’t sing yet)\n2. Press Start and hold the headphones to the mic for about 7 s; the viewers hear nothing meanwhile\n3. The music or the vocal is delayed for the viewers so they line up; your headphones stay the same\nPress again after adding or removing a plug-in with latency, or changing the buffer");
    s(Str::SyncMic, "แทร็กไมค์ (เสียงร้อง)", "Mic track (vocal)");
    s(Str::SyncRef, "แทร็กเพลง", "Music track");
    s(Str::SyncStart, "เริ่มวัด", "Start");
    s(Str::SyncCancel, "ยกเลิก", "Cancel");
    s(Str::SyncMeasuringRef, "กำลังฟังแทร็กเพลง...", "Listening to the music track…");
    s(Str::SyncMeasuringMic, "กำลังฟังไมค์...", "Listening to the mic…");
    s(Str::SyncLate, "เสียงร้องช้ากว่าเพลง", "The vocal was late by");
    s(Str::SyncEarly, "เสียงร้องเร็วกว่าเพลง", "The vocal was early by");
    s(Str::SyncInTime, "ชดเชยฝั่งคนดูแล้ว (หูฟังคุณจะเหมือนเดิม กด \"ฟังแบบคนดู\" เพื่อเช็ค)",
      "fixed for the viewers (your headphones stay the same - press “Hear viewers’ mix” to check)");
    s(Str::SyncNeedTracks, "ต้องมี HEARASIDE Track บนแทร็กไมค์ และมี App Audio (หรือแทร็กเพลงที่มี HEARASIDE Track) เป็นเพลงอ้างอิง",
      "The mic track needs HEARASIDE Track, and the music has to come from App Audio (or a track with HEARASIDE Track)");
    s(Str::SyncCountdown, "เปิดเพลงไว้ เอาหูฟังจ่อไมค์ และอย่าเพิ่งร้อง เริ่มวัดใน", "Play the music, hold the headphones to the mic and stay quiet. Measuring in");
    s(Str::SyncOptions, "เลือกแทร็กที่ใช้ซิงค์", "Choose the tracks to sync");
    s(Str::SyncNoAudio, "DAW ไม่ได้ส่งเสียงมาเลย เปิดเล่นหรือเปิด monitor แล้วลองใหม่", "The DAW isn’t running audio - start playback or monitoring and try again");
    s(Str::SyncNoMusic, "ไม่ได้ยินแทร็กเพลง เปิดเพลงแล้วลองใหม่", "The music track is silent - play the music and try again");
    s(Str::SyncNoMic, "ไมค์ไม่ได้ยินเพลง เอาหูฟังจ่อไมค์ให้ใกล้ขึ้นหรือเพิ่มเสียง แล้วลองใหม่", "The mic doesn’t hear the music - hold the headphones closer or louder and try again");
    s(Str::SyncWeakMic, "ไมค์ได้ยินเพลงจากหูฟังไม่ชัด ปลั๊กอินลดเสียงรบกวนบนแทร็กไมค์ (เช่น NS1 หรือ noise gate) อาจตัดทิ้ง ปิดชั่วคราวตอนวัด หรือเอาหูฟังจ่อไมค์ให้ชิดและดังขึ้น",
      "The mic barely hears the music from the headphones - a noise suppressor or gate on the mic track (NS1…) may remove it. Bypass it while measuring, or hold the headphones closer and louder");
    s(Str::SyncUnsteady, "วัดหลายรอบแล้วได้ค่าไม่ตรงกัน (อาจมีเสียงพูดหรือเสียงรบกวน) เลยยังไม่เปลี่ยน delay เงียบไว้แล้วลองใหม่",
      "The measurements disagreed (talking or noise?), so the delays were left alone - stay quiet and try again");
    s(Str::AppLinkIn, "เสียงที่เพื่อนส่งเข้ามา (ลิงก์ส่งเสียง)", "Sent in through my send link");
    s(Str::AppLinkFrom, "ลิงก์", "Link");
    s(Str::AppLinkWaiting, "ยังไม่มีใครส่งเสียงเข้ามา ส่ง \"ลิงก์ส่งเสียงเข้า\" จาก Hub ให้เพื่อน", "Nobody is sending yet - give a friend the send link from the Hub");
    s(Str::AppLinkOffline, "ลิงก์นี้ยังไม่ออนไลน์ (อีกฝั่งยังไม่เปิดแชร์) จะต่อให้เองเมื่อพร้อม", "The link is offline (not shared right now); it connects by itself when it is back");
    s(Str::AppLinkAsk, "รับจากลิงก์ของคนอื่น...", "Receive someone’s link…");
    s(Str::AppLinkPrompt, "วางลิงก์ฟังสดของ HEARASIDE เครื่องอื่น (ขึ้นต้นด้วย https:// และมี /l/)", "Paste another HEARASIDE listen link (https://…/l/…)");
    s(Str::AppLinkBad, "ลิงก์ไม่ถูกต้อง ต้องเป็นลิงก์ฟังสดของ HEARASIDE", "That isn’t a HEARASIDE listen link");
    s(Str::AppLinkReceiving, "รับเสียงจากลิงก์อยู่", "Receiving the link");
    s(Str::ShareTip, "แชร์ลิงก์ให้คนอื่นฟัง หรือให้เพื่อนส่งเสียงเข้ามา (แบบ LISTENTO)", "Share a listen link, or let a friend send audio in (like LISTENTO)");
    s(Str::ShareTitle, "แชร์เสียง (แบบ LISTENTO)", "Share audio (like LISTENTO)");
    s(Str::ShareOn, "เปิดแชร์", "Sharing");
    s(Str::ShareListen, "ลิงก์ฟังสด", "Listen link");
    s(Str::ShareListenCap, "ส่งให้เพื่อนหรือลูกค้าเปิดในเบราว์เซอร์ ฟังเสียงที่คนดูได้ยิน คุณภาพไม่บีบอัด", "Anyone with it hears the viewers’ mix in a browser, uncompressed");
    s(Str::ShareSend, "ลิงก์ส่งเสียงเข้า", "Send-in link");
    s(Str::ShareSendCap, "ให้เพื่อนเปิดแล้วกดส่งไมค์เข้ามา แล้วเลือก \"เสียงที่เพื่อนส่งเข้ามา\" ใน App Audio", "A friend opens it and sends their mic; pick “Sent in through my send link” in App Audio");
    s(Str::Copy, "คัดลอก", "Copy");
    s(Str::Copied, "คัดลอกแล้ว", "Copied");
    s(Str::OpenLink, "เปิด", "Open");
    s(Str::Listeners, "ผู้ฟัง", "Listening");
    s(Str::SenderOn, "มีคนส่งเสียงเข้ามาอยู่", "Someone is sending audio in");
    s(Str::SenderOff, "ยังไม่มีใครส่งเสียงเข้ามา", "Nobody is sending audio in");
    s(Str::TunnelReady, "ลิงก์ใช้ได้ทุกที่ผ่านเน็ต (Cloudflare)", "The links work anywhere (via Cloudflare)");
    s(Str::TunnelStarting, "กำลังเปิดลิงก์ผ่านเน็ต...", "Opening the internet link…");
    s(Str::TunnelMissing, "ตอนนี้ใช้ได้เฉพาะคนที่ต่อ WiFi เดียวกัน ติดตั้ง cloudflared (ฟรี) เพื่อแชร์ผ่านเน็ต แล้วปิดเปิดแชร์ใหม่", "For now only devices on your Wi-Fi can open them. Install cloudflared (free) to share over the internet, then turn sharing off and on");
    s(Str::TunnelFailed, "เปิดลิงก์ผ่านเน็ตไม่สำเร็จ ใช้ได้เฉพาะ WiFi เดียวกันไปก่อน", "Couldn’t open the internet link - Wi-Fi only for now");
    s(Str::CopyInstall, "คัดลอกคำสั่งติดตั้ง", "Copy install command");
    s(Str::SharingChip, "แชร์อยู่", "Sharing");
    s(Str::PowerTip, "เปิด / ปิดการดึงเสียงจากโปรแกรม", "Switch program capture on / off");
    s(Str::ChainLatency, "ปลั๊กอินก่อนหน้าหน่วง", "Plug-ins before this add");
    s(Str::DawBuffer, "บัฟเฟอร์ DAW", "DAW buffer");
    s(Str::Samples, "sample", "samples");
    s(Str::LatencyTip, "เสียงถึงคนดูช้าเท่าไร แยกตามส่วน:\nDAW = buffer ของ DAW\nปลั๊กอินแทร็ก = แทร็กที่ปลั๊กอินหน่วงที่สุด (วัดเอง)\nปลั๊กอิน Master = ปลั๊กอินเหนือ Hub (วัดเอง)\nHub = กันเสียงพีค\nOBS = buffer ของ HEARASIDE ใน OBS\n(ไม่รวม latency ของ audio interface ที่ปลั๊กอินมองไม่เห็น)",
      "How late the viewers get the sound, part by part:\nDAW = the DAW buffer\nTrack plug-ins = the slowest track’s plug-ins (measured)\nMaster plug-ins = the plug-ins above the Hub (measured)\nHub = peak protection\nOBS = the HEARASIDE buffer in OBS\n(audio-interface latency is invisible to plug-ins)");
    s(Str::PillPrefixYou, "คุณ", "You: ");
    s(Str::PillPrefixViewers, "คนดู", "Viewers: ");
    s(Str::LevelsTitle, "ระดับเสียงรวม", "Levels");
    s(Str::LevelsTip, "ระดับรวมไปหาคนดู, ระดับหูฟังรวม และกันเสียงพีค", "Stream level, headphone master and peak protection");
    s(Str::SummaryOnlyViewers, "คนดูได้ยิน แต่คุณไม่ได้ยิน", "Viewers hear, you don’t");
    s(Str::StartTitle, "เริ่มใช้งาน 3 ขั้น", "Get started in 3 steps");
    s(Str::StartStep1, "ใส่ HEARASIDE Track เป็นปลั๊กอินตัวสุดท้ายของแทร็กที่ต้องการ", "Put HEARASIDE Track last on every track you want here");
    s(Str::StartStep2, "เปิด OBS แล้วเพิ่ม Source ชื่อ HEARASIDE", "Open OBS and add a HEARASIDE source");
    s(Str::StartStep3, "กด \"ฟังแบบคนดู\" ฟังเสียงที่ส่งไปหาคนดูสักครั้ง", "Press “Hear viewers’ mix” once to check what you send");
    s(Str::StartHide, "ซ่อน", "Hide");
    s(Str::StartStep1StudioOne, "Studio One: ลากจาก Browser › Effects ไปวางล่างสุดของช่อง Inserts", "Studio One: drag it from Browser › Effects to the bottom of the Inserts");
    s(Str::StartStep1Cubase, "Cubase: ใส่ในช่อง Insert ล่างสุดของแทร็ก", "Cubase: use the track’s last insert slot");
    s(Str::StartStep1Reaper, "Reaper: ปุ่ม FX ของแทร็ก › Add แล้วให้อยู่ล่างสุดของรายการ", "Reaper: the track’s FX button › Add, at the bottom of the list");
    s(Str::StartStep1Fl, "FL Studio: ช่อง Mixer ของแทร็ก ใส่ใน slot ล่างสุด", "FL Studio: the track’s Mixer insert, last slot");
    s(Str::StartStep1Ableton, "Ableton Live: วางขวาสุดของ Device chain ของแทร็ก", "Ableton Live: the far right of the track’s device chain");
    s(Str::SyncTip, "ตั้งดีเลย์ให้เสียงร้องตรงกับเพลงฝั่งคนดู กดแล้วทำตามที่ขึ้นบนจอ (ปุ่ม ⋯ ข้าง ๆ ดูขั้นตอนและเลือกแทร็กเอง)",
      "Lines your vocal up with the music for the viewers. Press it and follow the screen (the … button shows the steps and lets you pick the tracks)");
    s(Str::LevelsColumn, "ระดับเสียง", "Levels");
    s(Str::RowLevels, "ระดับในหูฟัง / ฝั่งคนดู...", "Headphone / viewers level…");

    // ---- redesign (prompt 3.9, design export) ----
    s(Str::YouHearTip, "คุณได้ยินอยู่ (คลิกเพื่อปิด)", "You hear this (click to turn off)");
    s(Str::YouDontHearTip, "คุณไม่ได้ยิน (คลิกเพื่อเปิด)", "You don’t hear this (click to turn on)");
    s(Str::ViewersHearTip, "คนดูได้ยินอยู่ (คลิกเพื่อปิด)", "Viewers hear this (click to turn off)");
    s(Str::ViewersDontHearTip, "คนดูไม่ได้ยิน (คลิกเพื่อเปิด)", "Viewers don’t hear this (click to turn on)");
    s(Str::YouHearAria, "คุณได้ยิน %s", "You hear %s");
    s(Str::ViewersHearAria, "คนดูได้ยิน %s", "Viewers hear %s");
    s(Str::YouDontHear, "คุณไม่ได้ยิน", "You don’t hear");
    s(Str::ViewersDontHear, "คนดูไม่ได้ยิน", "Viewers don’t hear");
    s(Str::DoubleClickToType, "ดับเบิลคลิกเพื่อพิมพ์ค่า", "Double-click to type a value");
    s(Str::DoubleClickToReset, "ดับเบิลคลิกเพื่อกลับเป็น 0 dB", "Double-click to reset to 0 dB");
    s(Str::DoubleClickToResetMs, "ดับเบิลคลิกเพื่อกลับเป็น 0 ms", "Double-click to reset to 0 ms");
    s(Str::DoubleClickToRename, "ดับเบิลคลิกเพื่อเปลี่ยนชื่อ", "Double-click to rename");
    s(Str::DoubleClickToCenter, "ดับเบิลคลิกเพื่อกลับกึ่งกลาง", "Double-click to center");
    s(Str::PanTypeTip, "ดับเบิลคลิกเพื่อพิมพ์: L40, C, R25", "Double-click to type: L40, C, R25");
    s(Str::PanCenterHint, "C · ดับเบิลคลิกเพื่อกลับกึ่งกลาง", "C · double-click to center");
    s(Str::NameInDaw, "ชื่อใน DAW: %s", "In the DAW: %s");
    s(Str::Back, "กลับ", "Back");
    s(Str::BackTip, "กลับหน้าหลัก", "Back to the main screen");
    s(Str::BackToTrack, "กลับไปที่แทร็ก", "Back to the track");
    s(Str::Center, "กึ่งกลาง", "Center");
    s(Str::More, "เพิ่มเติม", "More");
    s(Str::OptionsFor, "ตัวเลือกของ %s", "Options for %s");
    s(Str::Manage, "จัดการ", "Manage");
    s(Str::Open, "เปิด", "Open");
    s(Str::Connect, "เชื่อมต่อ", "Connect");
    s(Str::Change, "เปลี่ยน", "Change");
    s(Str::Keep, "ไม่ออก", "Keep");
    s(Str::TracksCount, "%d แทร็ก", "%d tracks");
    s(Str::MsValue, "%d ms", "%d ms");
    s(Str::ObsChipTip, "การเชื่อม OBS และความหน่วง", "OBS connection and delay");
    s(Str::ObsOpen, "เปิด OBS", "Open OBS");
    s(Str::ObsAddSource, "เพิ่ม Source ชื่อ HEARASIDE แล้ว Hub จะเชื่อมเอง", "Add a source called HEARASIDE and the Hub connects by itself");
    s(Str::ObsConnectedCap, "OBS ได้รับเสียงที่คนดูได้ยินอยู่", "OBS is getting what viewers hear");
    s(Str::SampleWord, "sample", "sample");
    s(Str::Total, "รวม", "Total");
    s(Str::AboutMs, "ประมาณ %s ms", "about %s ms");
    s(Str::LineUpForFriends, "รอเพื่อนให้ตรงจังหวะ", "Line-up for friends");
    s(Str::ShareAudioTip, "แชร์เสียง", "Share audio");
    s(Str::SettingsTip, "ตั้งค่า", "Settings");
    s(Str::AccountTip, "บัญชี: %s (@%s)", "Account: %s (@%s)");
    s(Str::SignInTip, "เข้าสู่ระบบ", "Sign in");
    s(Str::ManageTracksTip, "ทุกแทร็กและทุกค่าในที่เดียว", "Every track and setting in one place");
    s(Str::ColYou, "คุณ", "You");
    s(Str::ColViewers, "คนดู", "Viewers");
    s(Str::LevelModeHeadphones, "หูฟัง", "Headphones");
    s(Str::LevelModeViewers, "คนดู", "Viewers");
    s(Str::LevelModeTip, "แถบเลื่อนแสดงระดับฝั่งไหน", "Which level the sliders show");
    s(Str::RenameHint, "ดับเบิลคลิกที่ชื่อ", "double-click name");
    s(Str::FineSettingsEllipsis, "ตั้งค่าละเอียด…", "Fine settings…");
    s(Str::RenamedToast, "เปลี่ยนชื่อเป็น “%s” ใน Hub และปลั๊กอินแทร็กแล้ว", "Renamed to “%s” in the Hub and its Track plug-in");
    s(Str::RenameRevertToast, "กลับไปใช้ชื่อจาก DAW: “%s”", "Back to the DAW’s name: “%s”");
    s(Str::ViewersDelayBadge, "หน่วงฝั่งคนดู", "Viewers delay");
    s(Str::RecordingBadge, "กำลังอัด", "Recording");
    s(Str::ProgramCaption, "คลิกที่ชื่อเพื่อเปลี่ยนโปรแกรม · ดึงเสียงและอัดที่ ⋯", "Click a name to switch programs · capture and record from ⋯");
    s(Str::ChooseProgramAria, "เลือกโปรแกรม ตอนนี้: %s", "Choose a program, now: %s");
    s(Str::CaptureProgram, "ดึงเสียงโปรแกรม", "Capture program audio");
    s(Str::PrintToFile, "อัดลงไฟล์", "Print to a file");
    s(Str::StopRecording, "หยุดอัด", "Stop recording");
    s(Str::RecordWithDaw, "อัดพร้อม DAW", "Record with the DAW");
    s(Str::OpenRecordings, "เปิดโฟลเดอร์ไฟล์อัด", "Open recordings folder");
    s(Str::WholeComputerShort, "ทั้งเครื่อง", "Whole computer");
    s(Str::AppOffShort, "ปิด", "Off");
    s(Str::FriendsTitle, "เพื่อน", "Friends");
    s(Str::FriendsCount, "%d จาก 8", "%d of 8");
    s(Str::FriendsCaption, "ในห้อง %d จาก 8 · ไลฟ์หน่วง %d ms ให้ทุกคนร้องตรงจังหวะ", "%d of 8 in the room · live delayed %d ms so everyone sings in time");
    s(Str::FriendsCaptionNoLineUp, "ในห้อง %d จาก 8", "%d of 8 in the room");
    s(Str::FriendsEmptyCaption, "ยังไม่มีใครในห้อง", "Nobody in the room yet");
    s(Str::FriendLive, "กำลังร้อง · หน่วง %d ms · แพน %s", "Live · delay %d ms · pan %s");
    s(Str::FriendSinging, "กำลังร้อง", "Singing");
    s(Str::FriendWaiting, "รอ %s เปิดลิงก์", "Waiting for %s to open their link");
    s(Str::FriendNotOpened, "ยังไม่ได้เปิดลิงก์", "Hasn’t opened the link yet");
    s(Str::FriendOffline, "ออฟไลน์", "Offline");
    s(Str::CopyFriendLink, "คัดลอกลิงก์ส่งเสียงของเพื่อน", "Copy their send-in link");
    s(Str::CopiedFriendLink, "คัดลอกลิงก์ส่งเสียงของ %s แล้ว", "Copied %s’s send-in link");
    s(Str::MeasureAgain, "วัดใหม่", "Measure again");
    s(Str::MeasureDelayAgain, "วัดความหน่วงใหม่", "Measure delay again");
    s(Str::MeasuredToast, "%s: วัดความหน่วงใหม่แล้ว %d ms", "%s: delay measured again, %d ms");
    s(Str::RecordInDaw, "อัดใน DAW ของคุณ…", "Record in your DAW…");
    s(Str::AddFriend, "เพิ่มเพื่อน", "Add friend");
    s(Str::RemoveFriend, "เอาออกจากห้อง", "Remove from the room");
    s(Str::SpreadOut, "กระจายเสียงเพื่อน", "Spread out");
    s(Str::OnlyViewersLabel, "คนดูได้ยิน แต่คุณไม่ได้ยิน:", "Viewers hear, you don’t:");
    s(Str::SilentLufs, "เงียบ", "Silent");
    s(Str::TabTracks, "แทร็ก · %d", "Tracks · %d");
    s(Str::TabLevels, "ระดับเสียง", "Levels");
    s(Str::TabSummary, "สรุป", "Summary");
    s(Str::SlidersSet, "แถบเลื่อนปรับ", "Sliders set");
    s(Str::ProgramsCount, "%d โปรแกรม", "%d programs");
    s(Str::FriendsCountShort, "%d คน", "%d friends");
    s(Str::CompactSummary, "คุณ %d · คนดู %d · %s LUFS", "You %d · Viewers %d · %s LUFS");
    s(Str::CompactSummaryMuted, "คุณ %d · คนดู ตัดอยู่", "You %d · Viewers muted");
    s(Str::MoreMenuAria, "เพิ่มเติม: แชร์เสียง ซิงค์เสียงร้อง ตั้งค่า", "More: share audio, auto-sync vocal, settings");
    s(Str::ObsShort, "OBS", "OBS");
    s(Str::ObsAriaCompact, "%s ความหน่วง %s ms", "%s, delay %s ms");
    s(Str::SharePageTitle, "แชร์และเพื่อน", "Share and friends");
    s(Str::SharePageSubtitle, "ให้คนอื่นฟังผ่านเบราว์เซอร์ หรือชวนเพื่อนมาร้องด้วยกัน", "Let people listen in a browser, or bring friends in to sing with you");
    s(Str::SpreadOutTip, "แพนเพื่อนแยกซ้ายขวา ให้คนดูแยกเสียงแต่ละคนออก", "Pan friends apart so viewers can tell the voices apart");
    s(Str::FriendDefaultName, "เพื่อน %d", "Friend %d");
    s(Str::LineUpFriends, "ปรับให้เพื่อนตรงจังหวะสำหรับคนดู", "Line up friends for viewers");
    s(Str::LineUpOff, "ปิดอยู่ คนดูได้ยินเพื่อนตามที่มาถึง ช้ากว่าเพลงนิดหน่อย", "Off. Viewers hear friends as they arrive, a little behind the music.");
    s(Str::LineUpWaiting, "เมื่อเพื่อนเข้ามา ไลฟ์จะถูกหน่วงเท่าที่จำเป็นให้ทุกคนร้องตรงจังหวะ", "When friends join, the live is delayed just enough for everyone to sing in time.");
    s(Str::LineUpCaption, "ไลฟ์ถูกหน่วง %d ms ให้ทุกคนร้องตรงจังหวะ (ช้าที่สุด: %s) หูฟังของคุณไม่ถูกหน่วง", "Your live is delayed %d ms so everyone sings in time (slowest: %s). Your headphones aren’t delayed.");
    s(Str::Measuring, "กำลังวัด…", "Measuring…");
    s(Str::DelayMsLabel, "หน่วง %d ms", "Delay %d ms");
    s(Str::CopyLink, "คัดลอกลิงก์", "Copy link");
    s(Str::RemoveNamed, "เอา %s ออก", "Remove %s");
    s(Str::Volume, "ความดัง", "Volume");
    s(Str::VolumeTip, "ความดังของเพื่อนคนนี้ ทั้งที่คุณและคนดูได้ยิน", "This friend’s volume for you and your viewers");
    s(Str::PanWord, "แพน", "Pan");
    s(Str::NoFriends, "ยังไม่มีเพื่อน", "No friends yet");
    s(Str::NoFriendsCaption, "เพิ่มเพื่อนเพื่อรับลิงก์ส่งเสียงเฉพาะคนนั้น เพื่อนเปิดในเบราว์เซอร์แล้วร้องตามได้เลย", "Add a friend to get a send-in link just for them. They open it in a browser and sing along.");
    s(Str::FriendsFooter, "เพื่อนแต่ละคนขึ้นเป็นแถวในหน้าหลัก ถ้าจะใส่ปลั๊กอินให้เพื่อน ให้เลือกชื่อเพื่อนใน HEARASIDE Track ถ้าจะอัดเสียงเพื่อนใน DAW ให้ใส่ App Audio ที่ช่อง Input แล้วเลือกชื่อเพื่อน", "Each friend shows up as a row on the main screen. To put your plug-ins on a friend, choose their name in HEARASIDE Track. To record a friend in your DAW, put App Audio on an input channel and choose their name.");
    s(Str::SendInLegacy, "ลิงก์ส่งเสียงเข้า", "Send-in link");
    s(Str::SendInLegacyCap, "ให้เพื่อนเปิดแล้วส่งไมค์เข้ามา เสียงเข้ามาที่ App Audio (เลือก “เสียงที่เพื่อนส่งเข้ามา”)", "A friend opens it and sends their mic. It arrives on App Audio (choose “Sent in through my send link”).");
    s(Str::ListenLinkCap, "ฟังแบบเดียวกับที่คนดูได้ยินทุกอย่าง", "Hear exactly what your viewers hear");
    s(Str::ListeningCount, "ฟังอยู่ %d คน", "%d listening");
    s(Str::ConnectionTitle, "การเชื่อมต่อ", "Connection");
    s(Str::LinksAnywhere, "ลิงก์ใช้ได้ทุกที่", "Links work anywhere");
    s(Str::LinksAnywhereCap, "ส่งผ่าน Cloudflare ลิงก์เดิมใช้ได้ทุกไลฟ์", "Sent via Cloudflare. Links stay the same between streams.");
    s(Str::LinksChangeCap, "ส่งผ่าน Cloudflare ลิงก์เปลี่ยนทุกครั้งที่เปิดแชร์ (ตั้ง “ที่อยู่เว็บแชร์” ในการตั้งค่าเพื่อให้ลิงก์คงที่)", "Sent via Cloudflare. The links change each time you share (set a “Share web address” in Settings to keep them).");
    s(Str::LinksStarting, "กำลังเปิดลิงก์ผ่านเน็ต…", "Opening the internet link…");
    s(Str::WifiOnly, "ใช้ได้เฉพาะ WiFi เดียวกัน", "Wi-Fi only");
    s(Str::WifiOnlyCap, "ติดตั้ง cloudflared (ฟรี) เพื่อแชร์ผ่านเน็ต แล้วปิดเปิดแชร์ใหม่:", "Install cloudflared (free) to share over the internet, then switch sharing off and on:");
    s(Str::SharingOff, "ยังไม่ได้แชร์", "Not sharing");
    s(Str::SharingOffCap, "เปิดสวิตช์ลิงก์ฟังสด หรือเพิ่มเพื่อน แล้วลิงก์จะพร้อมใช้", "Switch the listen link on or add a friend and the links get ready.");
    s(Str::ReceiveTitle, "รับลิงก์ของคนอื่น", "Receive someone else’s link");
    s(Str::ReceiveCap, "วางลิงก์ฟังสดจาก HEARASIDE เครื่องอื่น เสียงจะเข้าที่ App Audio", "Paste a listen link from another HEARASIDE. It arrives on App Audio.");
    s(Str::ReceiveNoApp, "ใส่ HEARASIDE App Audio ในแทร็กใดแทร็กหนึ่งก่อน", "Put HEARASIDE App Audio on a track first.");
    s(Str::LinkToReceive, "ลิงก์ที่จะรับ", "Link to receive");
    s(Str::SettingsSubtitle, "เปลี่ยนแล้วมีผลทันทีและบันทึกเอง", "Changes apply right away and save automatically");
    s(Str::SettingsSections, "หมวดการตั้งค่า", "Settings sections");
    s(Str::SecAppearance, "หน้าตา", "Appearance");
    s(Str::SecAppearanceCap, "ภาษา ธีม และขนาด", "Language, theme and size");
    s(Str::SecAudio, "เสียง", "Audio");
    s(Str::SecAudioCap, "กันเสียงพีค การซิงก์ และชื่อ Stem", "Peak protection, sync and stem names");
    s(Str::SecConnection, "การเชื่อมต่อ", "Connection");
    s(Str::SecConnectionCap, "OBS และการเชื่อมระหว่างปลั๊กอิน", "OBS and the link between plug-ins");
    s(Str::SecAbout, "เกี่ยวกับ", "About");
    s(Str::SecAboutCap, "เวอร์ชันและการรีเซ็ต", "Version and reset");
    s(Str::SettingsScope, "การตั้งค่าเหล่านี้ใช้กับ Hub, Track และ App Audio ทุกตัวในเครื่องนี้", "These settings apply to every Hub, Track and App Audio on this computer");
    s(Str::LanguageCap, "ทุกปุ่มและทุกข้อความ", "For every button and message");
    s(Str::ThemeCap, "“ตามระบบ” จะสว่างหรือมืดตาม Windows", "“System” follows Windows light or dark mode");
    s(Str::UiSizeCap, "ขยายทุกอย่างสำหรับจอความละเอียดสูง", "Scales everything up for high-resolution screens");
    s(Str::GlassCap, "เพิ่มถ้าพื้นหลังทำให้อ่านตัวหนังสือยาก", "Raise it if the background makes text hard to read");
    s(Str::ColoursCap, "ใช้สีจาก DAW หรือขาวดำ", "Use the DAW’s colours or monochrome");
    s(Str::ReduceMotionCap, "ปิดแอนิเมชันและการขยับของมิเตอร์", "Turns off animations and meter movement");
    s(Str::CeilingCap, "เสียงที่คนดูได้ยินจะไม่ดังเกินระดับนี้", "What viewers hear never goes above this level");
    s(Str::StemNamesCap, "แสดงในเมนู “ส่งเป็น Stem” ของแต่ละแทร็ก และใน OBS", "Shown in each track’s “Send to stem” menu and in OBS");
    s(Str::StemNameN, "ชื่อ Stem %d", "Stem %d name");
    s(Str::StemN, "Stem %d", "Stem %d");
    s(Str::LineUpLimit, "เพดานการรอเพื่อน", "Line-up limit");
    s(Str::LineUpLimitCap, "เพื่อนที่ช้ากว่านี้จะไม่ถูกรอ", "Friends slower than this aren’t waited for");
    s(Str::ObsRowCap, "เปิด OBS แล้วเพิ่ม Source ชื่อ HEARASIDE จะเชื่อมเอง", "Open OBS and add a source called HEARASIDE; it connects by itself");
    s(Str::NotConnected, "ยังไม่เชื่อม", "Not connected");
    s(Str::ConnectedWord, "เชื่อมแล้ว", "Connected");
    s(Str::BusNameCap, "แยกหลายโปรเจกต์หรือหลาย DAW ในเครื่องเดียวกัน ส่วนใหญ่ไม่ต้องเปลี่ยน", "Keeps several projects or DAWs on one computer apart. Most people never change it");
    s(Str::DelayToObsCap, "วัดเอง: บัฟเฟอร์ DAW + ปลั๊กอินแทร็ก + ปลั๊กอิน Master", "Measured automatically: DAW buffer + track plug-ins + master plug-ins");
    s(Str::ShareBaseCap, "เว็บ Vercel ของคุณ ให้ลิงก์คงเดิมทุกไลฟ์ (web/share-vercel) ว่าง = ลิงก์เปลี่ยนทุกครั้ง", "Your Vercel site, so links stay the same between streams (web/share-vercel). Empty = they change each time");
    s(Str::RestApiCapShort, "สั่ง Hub จากโปรแกรมในเครื่องนี้ (Stream Deck, สคริปต์)", "Control the Hub from programs on this computer (Stream Deck, scripts)");
    s(Str::AppTagline, "แยกเสียงที่คุณได้ยินออกจากเสียงที่คนดูได้ยิน", "Separates what you hear from what your viewers hear");
    s(Str::VersionN, "เวอร์ชัน %s", "Version %s");
    s(Str::GettingStarted, "เริ่มใช้งาน", "Getting started");
    s(Str::GettingStartedCap, "แสดงคู่มือ 3 ขั้นในหน้าหลักอีกครั้ง", "Show the 3-step guide on the main screen again");
    s(Str::ShowAgain, "แสดงอีกครั้ง", "Show again");
    s(Str::ResetSettings, "รีเซ็ตการตั้งค่า", "Reset settings");
    s(Str::ResetSettingsCap, "คืนทุกอย่างในหน้านี้เป็นค่าเริ่มต้น แทร็กและลิงก์แชร์ยังอยู่", "Puts everything on this page back to default. Tracks and share links are kept");
    s(Str::ResetWord, "รีเซ็ต", "Reset");
    s(Str::SetupCheckCap, "ตรวจ Hub แทร็ก OBS และลิงก์ แล้วบอกวิธีแก้", "Checks the Hub, tracks, OBS and links, and says how to fix them");
    s(Str::SetupMastering, "อยากปรับเสียงที่คนดูได้ยิน (EQ, compressor, limiter) ให้ใส่ปลั๊กอินบน Master เหนือ Hub ปลั๊กอินใต้ Hub มีผลกับหูฟังเท่านั้น", "To shape what viewers hear (EQ, compressor, limiter), put plug-ins on the master above the Hub. Plug-ins below the Hub only change your headphones.");
    s(Str::TrackFineTitle, "ตั้งค่าแทร็กละเอียด", "Track fine settings");
    s(Str::TrackFineSubtitle, "มีผลกับแทร็กนี้เท่านั้น fader ใน DAW ไม่ขยับ", "Only affects this track. Your DAW faders stay put");
    s(Str::PrevTrack, "แทร็กก่อนหน้า", "Previous track");
    s(Str::NextTrack, "แทร็กถัดไป", "Next track");
    s(Str::ChooseTrack, "เลือกแทร็ก", "Choose a track");
    s(Str::TrackNOfM, "แทร็ก %d จาก %d", "Track %d of %d");
    s(Str::ViewersCard, "คนดู", "Viewers");
    s(Str::ViewersCardCap, "สิ่งที่ส่งไป OBS", "What goes to OBS");
    s(Str::PanCap, "ฝั่งคนดูเท่านั้น หูฟังของคุณไม่ถูกแพน ใช้ได้กับแทร็กสเตอริโอ", "Viewers only, your headphones aren’t panned. Works on stereo tracks.");
    s(Str::DelayCapFine, "เหมือน Sync Offset ใน OBS แต่ทำทีละแทร็ก ถ้าปลั๊กอินร้องหน่วง 40 ms ให้ตั้งแทร็กดนตรีเป็น 40 ms หูฟังของคุณไม่ถูกหน่วง ·", "Like OBS sync offset, per track. If your vocal chain adds 40 ms, set the backing track to 40 ms. Your headphones aren’t delayed ·");
    s(Str::MeasureAuto, "วัดอัตโนมัติ", "Measure automatically");
    s(Str::SoloCap, "คนดูได้ยินเฉพาะแทร็กที่ Solo", "Viewers hear only soloed tracks");
    s(Str::HeadphonesCard, "หูฟัง", "Headphones");
    s(Str::HeadphoneLevelCap, "เปลี่ยนเฉพาะที่คุณได้ยิน คนดูได้ยินเท่าเดิม", "Changes only what you hear. Viewers hear the same");
    s(Str::TrackInfo, "ข้อมูลแทร็ก", "Track info");
    s(Str::NameShown, "ชื่อที่แสดงใน HEARASIDE", "Name shown in HEARASIDE");
    s(Str::StatusWord, "สถานะ", "Status");
    s(Str::Running, "ทำงานอยู่", "Running");
    s(Str::NotRunning, "DAW ไม่ได้ประมวลผลอยู่", "Not running");
    s(Str::SampleRate, "Sample rate", "Sample rate");
    s(Str::MatchesHub, "ตรงกับ Hub", "Matches the Hub");
    s(Str::DiffersHub, "ไม่ตรงกับ Hub คนดูไม่ได้ยินแทร็กนี้", "Differs from the Hub, viewers don’t get this track");
    s(Str::BypassedWord, "ถูก bypass", "Bypassed");
    s(Str::SyncSubtitle, "ปรับเสียงร้องให้ตรงกับเพลงฝั่งคนดู หูฟังของคุณเหมือนเดิม", "Lines the vocal up with the music for viewers. Your headphones stay the same");
    s(Str::SyncStep1, "เลือกแทร็ก", "Choose the tracks");
    s(Str::SyncStep2, "เตรียมตัว", "Get ready");
    s(Str::SyncStep3, "วัด", "Measure");
    s(Str::SyncCheck1, "เปิดเพลง", "Play the music");
    s(Str::SyncCheck2, "เอาหูฟังจ่อไมค์", "Hold your headphones up to the mic");
    s(Str::SyncCheck3, "เงียบไว้จนกว่าจะเสร็จ", "Stay quiet until it’s done");
    s(Str::SyncStartMeasuring, "เริ่มวัด", "Start measuring");
    s(Str::SyncTakes, "ใช้เวลาราว 7 วินาที", "Takes about 7 seconds.");
    s(Str::SyncViewersNothing, "ระหว่างวัดคนดูจะไม่ได้ยินอะไร", "Viewers hear nothing while it measures.");
    s(Str::SyncViewersUntil, "คนดูจะไม่ได้ยินอะไรจนกว่าจะวัดเสร็จ", "Viewers hear nothing until this finishes");
    s(Str::SyncStartingIn, "เริ่มวัดใน %d…", "Starting in %d…");
    s(Str::SyncLateBy, "เสียงร้องช้ากว่าเพลง %d ms", "The vocal was late by %d ms");
    s(Str::SyncEarlyBy, "เสียงร้องเร็วกว่าเพลง %d ms", "The vocal was early by %d ms");
    s(Str::SyncFixedMusic, "แก้ให้คนดูแล้วโดยหน่วงแทร็กดนตรี หูฟังของคุณเหมือนเดิม", "Fixed for the viewers by delaying the backing track. Your headphones stay the same.");
    s(Str::SyncFixedVocal, "แก้ให้คนดูแล้วโดยหน่วงแทร็กเสียงร้อง หูฟังของคุณเหมือนเดิม", "Fixed for the viewers by delaying the vocal. Your headphones stay the same.");
    s(Str::SyncCheckWith, "ลองฟังด้วย “ฟังแบบคนดู”", "Check with “Hear viewers’ mix”");
    s(Str::SyncFailedTitle, "วัดไม่สำเร็จ", "Couldn’t measure");
    s(Str::CurrentDelays, "ความหน่วงตอนนี้ (ฝั่งคนดู)", "Current delays (viewers)");
    s(Str::SetDelaysByHand, "ตั้งความหน่วงเอง", "Set delays by hand");
    s(Str::IfItDoesntWork, "ถ้าไม่ได้ผล", "If it doesn’t work");
    s(Str::TipSilentTitle, "แทร็กเพลงไม่มีเสียง", "The music track is silent");
    s(Str::TipSilent, "เปิดเพลงแล้วลองใหม่", "Play the music, then try again.");
    s(Str::TipNoMicTitle, "ไมค์ไม่ได้ยินเพลง", "The mic can’t hear the music");
    s(Str::TipNoMic, "เอาหูฟังจ่อให้ใกล้ขึ้นและเพิ่มเสียง", "Hold the headphones closer and turn them up.");
    s(Str::TipWeakTitle, "ไมค์ได้ยินเบามาก", "The mic barely hears it");
    s(Str::TipWeak, "ปลั๊กอินลดเสียงรบกวนหรือ gate บนแทร็กไมค์อาจตัดทิ้ง ปิดชั่วคราวตอนวัด", "A noise suppressor or gate on the mic track may remove it. Bypass it while measuring.");
    s(Str::TipUnsteadyTitle, "วัดแล้วได้ค่าไม่ตรงกัน", "Measurements disagreed");
    s(Str::TipUnsteady, "มีเสียงพูดหรือเสียงรบกวนแทรก เงียบไว้แล้วลองใหม่", "Talking or noise got in. Stay quiet and try again.");
    s(Str::TracksPageTitle, "จัดการแทร็ก", "Manage tracks");
    s(Str::TracksPageSubtitle, "ทุกแทร็กที่มี HEARASIDE Track ทุกค่าในที่เดียว", "Every track with HEARASIDE Track, every setting in one place");
    s(Str::FilterAll, "ทั้งหมด %d", "All %d");
    s(Str::FilterYou, "คุณได้ยิน %d", "You hear %d");
    s(Str::FilterViewers, "คนดูได้ยิน %d", "Viewers hear %d");
    s(Str::FilterSilent, "เงียบ %d", "Silent %d");
    s(Str::FilterTip, "แสดง", "Show");
    s(Str::ClearSolo, "ยกเลิก Solo", "Clear solo");
    s(Str::ResetLevelsPan, "รีเซ็ตระดับและแพน", "Reset levels and pan");
    s(Str::ColHpLevel, "ระดับหูฟัง", "Headphone level");
    s(Str::ColVwLevel, "ระดับคนดู", "Viewers level");
    s(Str::ColPan, "แพน (คนดู)", "Pan (viewers)");
    s(Str::ColDelay, "หน่วง", "Delay");
    s(Str::ColStem, "Stem", "Stem");
    s(Str::ColSolo, "Solo", "Solo");
    s(Str::NoTracksFilter, "ไม่มีแทร็กที่ตรงกับตัวกรองนี้", "No tracks match this filter");
    s(Str::TracksFooter, "ดับเบิลคลิกชื่อหรือค่าเพื่อพิมพ์ · ดับเบิลคลิกแถบเลื่อนเพื่อรีเซ็ต · ทุกการเปลี่ยนแปลงไปถึงปลั๊กอินแทร็กทันที", "Double-click a name or a value to type · double-click a slider to reset · changes reach each Track plug-in right away");
    s(Str::ResetLevelsToast, "รีเซ็ตระดับและแพนของทุกแทร็กแล้ว", "Levels and pan are back to default on every track");
    s(Str::ProgramsPageTitle, "จัดการเสียงโปรแกรม", "Manage program audio");
    s(Str::ProgramsPageSubtitle, "App Audio ทุกตัวในโปรเจกต์นี้: แหล่งเสียง ระดับ และไฟล์อัด", "Every App Audio in this project: sources, levels and recordings");
    s(Str::AppAudioWord, "App Audio", "App Audio");
    s(Str::InThisProject, "%d ตัวในโปรเจกต์นี้", "%d in this project");
    s(Str::OnChannel, "อยู่ที่ %s", "on %s");
    s(Str::ChooseSource, "เลือกแหล่งเสียง", "Choose a source");
    s(Str::OffChannel, "ปิดอยู่ · ช่องนี้ปล่อยเสียงเดิมผ่านอย่างเดียว", "Off · the channel passes its own sound only");
    s(Str::FriendTakeShift, "รับเสียง %s อยู่ · ไฟล์ที่อัดเลื่อนย้อน %d ms ให้ตรงจังหวะ", "Receiving %s · takes are shifted back %d ms to line up");
    s(Str::DelayViewers, "หน่วง (คนดู)", "Delay (viewers)");
    s(Str::ProgramOnly, "เฉพาะเสียงโปรแกรม", "Program only");
    s(Str::ProgramOnlyCap, "ตัดเสียงเดิมของช่องทิ้ง ไม่มีเสียงไมค์ปนในไฟล์ที่อัด", "Drops the channel’s own sound, so no mic ends up in the recording");
    s(Str::AddSourceHint, "ถ้าจะเพิ่มแหล่งเสียง ให้ใส่ HEARASIDE App Audio ที่ช่อง Input ใน DAW แล้วจะขึ้นที่นี่เอง", "To add another source, put HEARASIDE App Audio on an input channel in your DAW. It shows up here by itself.");
    s(Str::HowToSetUp, "วิธีตั้งค่า", "How to set up");
    s(Str::RecordedTakes, "ไฟล์ที่อัดไว้", "Recorded takes");
    s(Str::RecordedTakesCap, "ลากไฟล์ไปวางในแทร็กไหนก็ได้ใน DAW", "Drag a take onto any track in your DAW");
    s(Str::RecordingTime, "กำลังอัด · %s", "Recording · %s");
    s(Str::StopTime, "หยุด · %s", "Stop · %s");
    s(Str::TakeName, "%s · ไฟล์ %d", "%s · take %d");
    s(Str::TakeMetaToday, "%s · วันนี้ %s", "%s · today %s");
    s(Str::TakeMetaDay, "%s · %s", "%s · %s");
    s(Str::DragWord, "ลาก", "Drag");
    s(Str::DragTakeTip, "ลาก %s ไปวางในแทร็ก", "Drag %s into a track");
    s(Str::TakesFolderHint, "บันทึกเป็น WAV ใน Documents\\HEARASIDE\\Recordings ใน Cubase หรือ Reaper ใช้ “Move to origin” จะวางไฟล์ตรงตำแหน่งที่อัด", "Saved as WAV in Documents\\HEARASIDE\\Recordings. In Cubase or Reaper, “Move to origin” puts a take where it was recorded.");
    s(Str::NoAppAudio, "ยังไม่มี App Audio ในโปรเจกต์นี้", "No App Audio in this project yet");
    s(Str::FineSettings, "ตั้งค่าละเอียด", "Fine settings");
    s(Str::RenameTrack, "เปลี่ยนชื่อแทร็ก", "Rename track");
    s(Str::RenameCap, "เปลี่ยนชื่อใน HEARASIDE เท่านั้น", "Only changes the name inside HEARASIDE");
    s(Str::BusNameTrackCap, "ต้องตรงกับ Hub ส่วนใหญ่ไม่ต้องเปลี่ยน", "Must match the Hub. Most people never change it.");
    s(Str::DawBufferFooter, "บัฟเฟอร์ DAW %d sample = %s ms · %s kHz", "DAW buffer %d samples = %s ms · %s kHz");
    s(Str::DawBufferFooterTip, "DAW = บัฟเฟอร์ของ DAW · Hub วัดความหน่วงของปลั๊กอินเอง", "DAW = DAW buffer. The Hub measures plug-in delay automatically.");
    s(Str::TrackDelayCap, "เหมือน Sync Offset ใน OBS เฉพาะแทร็กนี้ หูฟังของคุณไม่ถูกหน่วง", "Like OBS sync offset, for this track only. Your headphones aren’t delayed.");
    s(Str::ViewersDelayMsAria, "หน่วงฝั่งคนดู (ms)", "Viewers delay in ms");
    s(Str::SetUpIn, "ตั้งค่าใน %s", "Set up in %s");
    s(Str::Detected, "ตรวจพบ", "Detected");
    s(Str::OtherDaws, "DAW อื่น", "Other DAWs");
    s(Str::StudioOneStep1, "Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo) ตั้งชื่อว่า App Audio", "Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo), name it App Audio");
    s(Str::StudioOneStep2, "ใน Console เปิดช่อง Inputs แล้วใส่ปลั๊กอินนี้ที่ App Audio", "In the Console, show the Inputs and put this plug-in on App Audio");
    s(Str::StudioOneStep3, "เปิด “เฉพาะเสียงโปรแกรม”", "Turn on “Program only”");
    s(Str::StudioOneStep4, "ตั้งขาเข้าของแทร็กสเตอริโอเป็น App Audio แล้วกดอัด", "Set a stereo track’s input to App Audio and press Record");
    s(Str::CubaseStep1, "ใส่ปลั๊กอินนี้ที่ Input Channel ใน MixConsole", "Put this plug-in on an Input Channel in the MixConsole");
    s(Str::CubaseStep2, "ตั้งขาเข้าของแทร็กเป็น Input นั้นแล้วกดอัด", "Set a track’s input to that input and press Record");
    s(Str::ReaperStep1, "ใส่ปลั๊กอินนี้ใน Input FX ของแทร็กที่จะอัด", "Put this plug-in in the Input FX of the track you record on");
    s(Str::ReaperStep2, "Arm แทร็กแล้วกดอัด", "Arm the track and press Record");
    s(Str::OtherDawStep1, "ถ้า DAW มีช่อง Input ให้ใส่ปลั๊กอินนี้ที่นั่น", "If your DAW has input channels, put this plug-in there");
    s(Str::OtherDawStep2, "อัดแทร็กจาก Input นั้น", "Record a track from that input");
    s(Str::CubaseOther, "· ใส่ที่ Input Channel", "· put it on the Input Channel");
    s(Str::ReaperOther, "· ใส่ใน Input FX ของแทร็ก", "· put it in the track’s Input FX");
    s(Str::StudioOneOther, "· ใส่ที่ช่อง Inputs ใน Console", "· put it on an input in the Console");
    s(Str::AppNormalTrackNote, "ถ้าใส่ในแทร็กปกติ คุณและคนดูได้ยินเสียงโปรแกรม แต่ DAW ไม่อัด", "On a normal track you and your viewers hear the program, but the DAW doesn’t record it.");
    s(Str::AppOffTrack, "ปิดอยู่ · แทร็กนี้ปล่อยเสียงเดิมผ่านอย่างเดียว", "Off · the track passes its own sound only");
    s(Str::AppDelayCap, "เลื่อนโปรแกรมให้ตรงกับแทร็กอื่นฝั่งคนดู หูฟังของคุณไม่ถูกหน่วง", "Lines the program up with your other tracks for viewers. Your headphones aren’t delayed.");
    s(Str::ProgramCapture, "ดึงเสียงโปรแกรม", "Program capture");
    s(Str::AccountEllipsis, "บัญชี…", "Account…");
    s(Str::DelayInMs, "หน่วง (ms)", "Delay in ms");
    s(Str::SentInLegacy, "เสียงที่เพื่อนส่งเข้ามา (ลิงก์ส่งเสียง)", "Sent in through my send link");
    s(Str::SourceThisTrack, "เสียงของแทร็กนี้", "This track’s sound");
    s(Str::SourceBringFriend, "ดึงเสียงเพื่อนเข้ามา", "Bring in a friend");
    s(Str::SourcePasteLink, "วางลิงก์…", "Paste a link…");
    s(Str::SourceInviteFriend, "ชวนเพื่อนใหม่…", "Invite a new friend…");
    s(Str::SourceNeedsHub, "ใส่ HEARASIDE Hub ก่อน จึงจะดึงเสียงเพื่อนได้", "Add HEARASIDE Hub to bring in friends");
    s(Str::FriendItem, "%s · เพื่อน", "%s · friend");
    s(Str::FriendInTrack, "%s · ใน “%s”", "%s · in “%s”");
    s(Str::FriendLevelIn, "ระดับเสียง %s ที่เข้าแทร็กนี้", "%s’s level into this track");
    s(Str::KeepOwnSound, "เก็บเสียงเดิมของแทร็กไว้ด้วย", "Keep this track’s own sound");
    s(Str::KeepOwnSoundCap, "ปิด: ส่งเฉพาะ %s ต่อไปที่ปลั๊กอินของคุณ", "Off: only %s goes on to your plug-ins");
    s(Str::FriendPaired, "%s ออกทาง HEARASIDE Track ท้ายแทร็กนี้ · ปลั๊กอินของคุณเพิ่ม %d ms", "%s goes out through HEARASIDE Track at the end of this track · your plug-ins add %d ms");
    s(Str::FriendNotPaired, "ใส่ HEARASIDE Track ไว้ท้ายแทร็กนี้ ระหว่างนี้ %s ไปถึงคนดูผ่าน DAW อย่างเดียว ไม่เข้าหูฟัง และไม่ตรงจังหวะ", "Add HEARASIDE Track at the end of this track. Until then, %s reaches viewers through the DAW only, not your headphones, and isn’t lined up.");
    s(Str::FriendStepBelow, "วางปลั๊กอินของคุณไว้ใต้ตัวนี้", "Put your plug-ins below this one");
    s(Str::FriendStepEnd, "ใส่ HEARASIDE Track ไว้ท้ายแทร็กนี้", "Put HEARASIDE Track at the end of this track");
    s(Str::FriendDawPaused, "DAW พักแทร็กนี้เพราะไม่มีเสียง ให้กดอาร์มแทร็กหรือเปิด input monitoring", "The DAW paused this track because it has no audio. Arm the track or turn on input monitoring.");
    s(Str::FriendDawPausedCubase, "Cubase: ปิด “Suspend VST3 plug-in processing when no audio signals are received”", "Cubase: turn off “Suspend VST3 plug-in processing when no audio signals are received”");
    s(Str::FriendInDawTrack, "อยู่ในแทร็ก DAW “%s”", "In DAW track “%s”");
    s(Str::FriendBackToHub, "%s กลับมาอยู่ในกลุ่มเพื่อนของ Hub แล้ว เพราะแทร็กใน DAW หยุด", "%s is back in the Hub’s Friends. The DAW track stopped.");
    s(Str::MixInDawTrack, "มิกซ์ในแทร็กของ DAW…", "Mix in a DAW track…");
    s(Str::BringBackToHub, "ย้ายกลับมาที่ Hub", "Bring back to the Hub");
    s(Str::GoToTrack, "ไปที่แทร็ก", "Go to the track");
    s(Str::MixedInDaw, "มิกซ์ใน DAW · หน่วง %d ms", "Mixed in the DAW · delay %d ms");
    s(Str::InDawNotHeadphones, "อยู่ใน DAW แต่ยังไม่เข้าหูฟัง", "In the DAW, not in your headphones yet");
    s(Str::SingingInDaw, "กำลังร้อง · ในแทร็ก DAW “%s”", "Singing · in DAW track “%s”");
    s(Str::SetLevelOnTrack, "ปรับระดับและแพนของ %s ที่แทร็ก “%s”", "Set %s’s level and pan on the track “%s”");
    s(Str::FriendBadge, "เพื่อน · %s", "Friend · %s");
    s(Str::FriendLinedUp, "เพื่อน · %s · ตรงจังหวะ", "Friend · %s · lined up");
    s(Str::FriendNotLinedUp, "เพื่อน · %s · ไม่ตรงจังหวะ", "Friend · %s · not lined up");
    s(Str::FriendThroughTrack, "%s · เข้าทาง “%s” · ปลั๊กอินเพิ่ม %d ms", "%s · in through “%s” · plug-ins add %d ms");
    s(Str::FriendWord, "เพื่อน", "Friend");
    s(Str::GoesOutThrough, "ออกทาง", "Goes out through");
    s(Str::FriendHearSummary, "คุณและคนดูได้ยิน %s ผ่าน HEARASIDE Track ท้ายแทร็กนี้ ปรับระดับและแพนได้ที่นั่นหรือใน Hub", "You and your viewers hear %s through the HEARASIDE Track at the end of this track. Set levels and pan there or in the Hub.");
    s(Str::StopBringing, "หยุดดึงเสียง %s", "Stop bringing in %s");
    s(Str::FriendMoved, "%s ย้ายไปที่ “%s” แล้ว", "%s moved to “%s”");
    s(Str::MoveHere, "ย้ายมาที่นี่", "Move here");
    s(Str::LinkCopiedSendTo, "คัดลอกลิงก์แล้ว ส่งให้ %s", "Link copied. Send it to %s");
    s(Str::LinkNotHearaside, "ลิงก์นี้ไม่ใช่ของ HEARASIDE", "That isn’t a HEARASIDE link");
    s(Str::LinkOtherRoom, "ลิงก์นี้เป็นของห้อง HEARASIDE อื่น", "That link is from another HEARASIDE room");
    s(Str::LinkListenNotLinedUp, "ลิงก์ฟังปรับให้ตรงจังหวะไม่ได้", "Listen links can’t be lined up");
    s(Str::FriendGone, "%s ไม่อยู่ในห้องแล้ว", "%s isn’t in the room any more");
    s(Str::RecordFriendHint, "ถ้าจะอัดเสียงเพื่อนใน DAW ให้ใส่ App Audio ที่ช่อง Input แล้วเลือกชื่อเพื่อน", "To record a friend in your DAW, put App Audio on an input channel and choose their name.");
    s(Str::FriendOverLimit, "ช้ากว่า %d ms จึงไม่ถูกรอ", "Slower than %d ms, so it isn’t waited for");
    s(Str::RemovedFriendToast, "เอา %s ออกจากห้องแล้ว ลิงก์ของเพื่อนใช้ไม่ได้อีก", "Removed %s from the room. Their link doesn’t work any more.");
    s(Str::FriendFullToast, "ห้องเต็มแล้ว (8 คน)", "The room is full (8 friends)");
    s(Str::LineUpOnToast, "ไลฟ์ถูกหน่วง %d ms ให้ทุกคนร้องตรงจังหวะ หูฟังของคุณไม่ถูกหน่วง", "Your live is now delayed %d ms so everyone sings in time. Your headphones aren’t delayed.");
    s(Str::LineUpChangedToast, "ไลฟ์ถูกหน่วงเป็น %d ms", "Your live is now delayed %d ms");
    s(Str::LineUpOffToast, "ไลฟ์กลับมาสดแล้ว ไม่ถูกหน่วง", "Your live is back to normal, not delayed");
    s(Str::MixFriendTitle, "มิกซ์เสียงเพื่อนในแทร็กของ DAW", "Mix a friend in a DAW track");
    s(Str::MixFriendStep1, "เพิ่มแทร็ก audio ว่างใน DAW แล้วใส่ HEARASIDE Track ไว้บนสุด", "Add an empty audio track in your DAW and put HEARASIDE Track at the top");
    s(Str::MixFriendStep2, "เลือก %s ในปลั๊กอินนั้น แล้วใส่ EQ, compressor หรือ reverb ต่อจากมัน", "Choose %s in that plug-in, then add your EQ, compressor or reverb below it");
    s(Str::MixFriendStep3, "ใส่ HEARASIDE Track อีกตัวไว้ท้ายแทร็ก", "Put another HEARASIDE Track at the end of the track");
    s(Str::AccountTitle, "บัญชี", "Account");
    s(Str::AccountOptional, "ไม่บังคับ ทุกอย่างใน HEARASIDE ใช้ได้โดยไม่ต้องมีบัญชี", "Optional. Everything in HEARASIDE works without an account.");
    s(Str::SecAccountCap, "ไม่บังคับ", "Optional");
    s(Str::SignInBrowser, "เข้าสู่ระบบผ่านเบราว์เซอร์", "Sign in with your browser");
    s(Str::CreateAccount, "สร้างบัญชี", "Create an account");
    s(Str::SignOut, "ออกจากระบบ", "Sign out");
    s(Str::SignInEllipsis, "เข้าสู่ระบบ…", "Sign in…");
    s(Str::NotSignedIn, "ยังไม่ได้เข้าสู่ระบบ", "Not signed in");
    s(Str::EditArrow, "แก้ไข", "Edit");
    s(Str::EnterCode, "ใส่โค้ดนี้ในเบราว์เซอร์", "Enter this code in your browser");
    s(Str::WaitingAllow, "รอคุณกดอนุญาต… · หมดอายุใน %s", "Waiting for you to allow it… · expires in %s");
    s(Str::CodeExpired, "โค้ดหมดอายุแล้ว", "The code expired");
    s(Str::GetNewCode, "ขอโค้ดใหม่", "Get a new code");
    s(Str::CodeAt, "ที่ %s", "at %s");
    s(Str::OpenThePage, "เปิดหน้าเว็บ", "Open the page");
    s(Str::CopyCode, "คัดลอกโค้ด", "Copy code");
    s(Str::AccountPhoto, "รูปโปรไฟล์", "Photo");
    s(Str::AccountPhotoCap, "JPG, PNG หรือ WebP ไม่เกิน 5 MB", "JPG, PNG or WebP, up to 5 MB");
    s(Str::ChangeEllipsis, "เปลี่ยน…", "Change…");
    s(Str::PhotoEllipsis, "รูป…", "Photo…");
    s(Str::RemoveWord, "ลบ", "Remove");
    s(Str::AccountDisplayName, "ชื่อที่แสดง", "Display name");
    s(Str::AccountDisplayNameCap, "ชื่อที่เพื่อนเห็นตอนเข้าห้อง", "What friends see when they join");
    s(Str::AccountUsername, "ชื่อผู้ใช้", "Username");
    s(Str::UsernameAvailable, "ใช้ได้ · ใช้ในลิงก์ของคุณ", "Available · used in your links");
    s(Str::UsernameRules, "a–z, 0–9 และ _ ยาว 3 ถึง 20 ตัว", "a–z, 0–9 and _, 3 to 20 characters");
    s(Str::UsernameTaken, "ชื่อผู้ใช้นี้มีคนใช้แล้ว", "That username is taken");
    s(Str::UsernameFree, "ใช้ได้", "Available");
    s(Str::AccountAbout, "เกี่ยวกับคุณ", "About you");
    s(Str::AboutCounter, "%d / 160", "%d / 160");
    s(Str::SyncAppearance, "ซิงก์หน้าตา", "Sync appearance");
    s(Str::SyncAppearanceCap, "ภาษา ธีม และขนาดตามคุณไปทุกเครื่อง", "Language, theme and size follow you to every computer");
    s(Str::SyncAppearanceCapShort, "ภาษา ธีม และขนาดในทุกเครื่อง", "Language, theme and size on every computer");
    s(Str::HideEmailOnScreen, "ซ่อนอีเมลบนจอ", "Hide my email on screen");
    s(Str::HideEmailCap, "สำหรับตอนแชร์จอในไลฟ์", "For when you share your screen on stream");
    s(Str::ThisComputer, "เครื่องนี้", "This computer");
    s(Str::ThisComputerCap, "แสดงใน บัญชี › อุปกรณ์ บนเว็บ", "Shown in Account › Devices on the website");
    s(Str::ManageOnWeb, "อีเมล รหัสผ่าน และอุปกรณ์ จัดการบนเว็บไซต์", "Email, password and devices are on the website");
    s(Str::MoreOnWeb, "ตั้งค่าเพิ่มเติมบนเว็บ", "More on the website");
    s(Str::SignOutConfirm, "ออกจากระบบปลั๊กอินทุกตัวในเครื่องนี้?", "Sign out every plug-in on this computer?");
    s(Str::AccountOffline, "ติดต่อ %s ไม่ได้ตอนนี้ เสียงของคุณไม่ได้รับผลกระทบ", "Can’t reach %s right now. Your audio isn’t affected.");
    s(Str::SignInHereAll, "เข้าสู่ระบบตรงนี้ = เข้าสู่ระบบปลั๊กอิน HEARASIDE ทุกตัวในเครื่องนี้", "Signing in here signs in every HEARASIDE plug-in on this computer.");
    s(Str::BenefitLinks, "ลิงก์ที่คงเดิม โดยไม่ต้องตั้งเว็บเอง", "Links that stay the same, without setting up a website");
    s(Str::BenefitFriends, "เพื่อนเห็นชื่อและรูปของคุณตอนเข้าห้อง", "Friends see your name and photo when they join");
    s(Str::BenefitAppearance, "การตั้งค่าหน้าตาของคุณในทุกเครื่อง", "Your appearance settings on every computer");
    s(Str::BenefitLinksShort, "ลิงก์ที่คงเดิม", "Links that stay the same");
    s(Str::BenefitFriendsShort, "เพื่อนเห็นชื่อและรูปของคุณ", "Friends see your name and photo");
    s(Str::BenefitAppearanceShort, "หน้าตาเหมือนกันทุกเครื่อง", "Your appearance on every computer");
    s(Str::InvitedYou, "%s ชวนคุณมาร้องเพลง", "%s invited you to sing");
    s(Str::AppearanceSynced, "ซิงก์หน้าตาจากบัญชีของคุณแล้ว", "Appearance synced from your account");
    s(Str::SignedInToast, "เข้าสู่ระบบเป็น %s แล้ว", "Signed in as %s");
    s(Str::SignedOutToast, "ออกจากระบบแล้ว", "Signed out");
    s(Str::DawYours, "DAW ของคุณ", "your DAW");
    return t;
}

const Table& table() {
    static const Table t = build();
    return t;
}

} // namespace

juce::String tr(Str key) {
    const auto& e = table()[size_t(key)];
    jassert(e.th != nullptr && e.en != nullptr);   // every key needs both languages
    const char* p = gLanguage.load(std::memory_order_relaxed) == 1 ? e.en : e.th;
    return p ? juce::String(juce::CharPointer_UTF8(p)) : juce::String("?");
}

void setCurrentLanguage(Language l) { gLanguage.store(l == Language::English ? 1 : 0, std::memory_order_relaxed); }
Language currentLanguage() { return gLanguage.load(std::memory_order_relaxed) == 1 ? Language::English : Language::Thai; }

juce::String formatDb(float db, bool withUnit) { return valuetext::formatDb(db, valuetext::kFloorDb, withUnit); }

juce::String trf(Str key, std::initializer_list<juce::String> args) {
    const auto pattern = tr(key);
    juce::String out;
    auto next = args.begin();
    for (int i = 0; i < pattern.length(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.length() && (pattern[i + 1] == 's' || pattern[i + 1] == 'd') && next != args.end()) {
            out << *next++;
            ++i;
        } else {
            out << juce::String::charToString(pattern[i]);
        }
    }
    return out;
}

} // namespace hearaside
