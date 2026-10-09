#include "Strings.h"
#include "Settings.h"

#include <array>
#include <atomic>

namespace hearaside {

namespace {

std::atomic<int> gLanguage { 0 };   // 0 = Thai, 1 = English

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
      "You won't hear this track in your headphones, but your viewers will.");
    s(Str::SumYouOnly, "คุณได้ยินคนเดียว คนดูจะไม่ได้ยินแทร็กนี้", "Only you hear this track. Viewers won't.");
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
      "The plug-in is bypassed, so you hear this track even with \"You hear\" off.");
    s(Str::RateMismatchTrack, "Sample rate ไม่ตรงกับ Hub แทร็กนี้จึงไม่ถูกส่งไปหาคนดู",
      "Sample rate differs from the Hub, so viewers don't get this track.");
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
      "You're hearing the viewers' mix. Your headphones get exactly what goes to OBS.");
    s(Str::BannerPanic, "ตัดเสียงไปหาคนดูอยู่ คนดูจะไม่ได้ยินอะไรจนกว่าคุณจะกดคืนเสียง",
      "The stream is muted. Viewers hear nothing until you unmute.");
    s(Str::NoTracks, "ยังไม่มีแทร็กที่ใส่ HEARASIDE Track ใส่ที่ท้าย FX ของแทร็กที่ต้องการ แล้วจะขึ้นที่นี่เอง",
      "No tracks yet. Put HEARASIDE Track last in a track's FX chain and it shows up here.");
    s(Str::Inactive, "ไม่ทำงาน", "Inactive");
    s(Str::InactiveTip, "DAW ไม่ได้ประมวลผลแทร็กนี้อยู่", "The DAW isn't processing this track right now.");
    s(Str::RateMismatch, "Sample rate ไม่ตรงกับ Hub แทร็กนี้จึงไม่ถูกส่งไปหาคนดู",
      "Sample rate differs from the Hub, so viewers don't get this track.");
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
    s(Str::PreviewOff, "ฟังแบบคนดู", "Hear viewers' mix");
    s(Str::PreviewOn, "กลับไปฟังแบบปกติ", "Back to my mix");

    s(Str::SummaryTitle, "สรุปตอนนี้", "Right now");
    s(Str::SummaryYou, "คุณได้ยินในหูฟัง", "In your headphones");
    s(Str::SummaryViewers, "คนดูได้ยิน", "Viewers hear");
    s(Str::SummaryPreviewing, "เหมือนที่คนดูได้ยิน (กำลังฟังแบบคนดู)", "Same as viewers (previewing)");
    s(Str::SummaryPanic, "ไม่มี เพราะกำลังตัดเสียงอยู่", "Nothing - the stream is muted");
    s(Str::LatencyToObs, "เสียงถึง OBS ช้ากว่าจริง", "Delay to OBS");
    s(Str::ObsHint, "OBS ยังไม่เชื่อม เปิด OBS แล้วเพิ่ม Source ชื่อ HEARASIDE",
      "OBS isn't connected. Open OBS and add a source called HEARASIDE.");

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
      "Turns down everything you hear without touching what viewers get (ignored when exporting).");
    s(Str::ViewersDelay, "หน่วงเวลาฝั่งคนดู", "Viewers delay");
    s(Str::DelayTip, "เหมือน Sync Offset ใน OBS แต่ทำทีละแทร็ก เช่น ปลั๊กอินร้องหน่วง 40 ms ให้ตั้งแทร็กดนตรีเป็น 40 ms เสียงร้องกับดนตรีจะตรงกันฝั่งคนดู หูฟังของคุณไม่ช้าลง",
      "Like OBS sync offset, per track. If your vocal chain adds 40 ms, set the backing track to 40 ms so voice and music line up for viewers. Your headphones are not delayed.");
    s(Str::Approx, "ประมาณ", "about");
    s(Str::MasterFx, "ปลั๊กอิน Master", "Master plug-ins");
    s(Str::TrackFx, "ปลั๊กอินแทร็ก", "Track plug-ins");
    s(Str::AppPick, "เลือกโปรแกรมที่จะดึงเสียง", "Choose a program");
    s(Str::AppNone, "ยังไม่ได้เลือกโปรแกรม", "No program chosen");
    s(Str::AppStarting, "กำลังเชื่อม...", "Connecting...");
    s(Str::AppRunning, "รับเสียงอยู่", "Capturing");
    s(Str::AppNotRunning, "โปรแกรมนี้ยังไม่เปิด จะเริ่มรับเสียงเองเมื่อเปิด", "This program isn't running; capture starts when it opens");
    s(Str::AppFailed, "รับเสียงไม่ได้ (ต้องใช้ Windows 10 2004 ขึ้นไป)", "Can't capture (needs Windows 10 2004 or newer)");
    s(Str::AppHint, "ใส่ปลั๊กอินนี้ที่ช่อง Input แล้วให้แทร็กรับเสียงจาก Input นั้น ปุ่มอัดของ DAW จะอัดเสียงโปรแกรมได้เลย",
      "Put this plug-in on an input channel and record a track from that input: the DAW's own Record button records the program.");
    s(Str::AppStepsStudioOne, "Studio One:\n1. Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo) ตั้งชื่อ App Audio\n2. ใน Console เปิดช่อง Inputs แล้วใส่ปลั๊กอินนี้ที่ช่อง App Audio\n3. เปิด \"เฉพาะเสียงโปรแกรม\" ข้างบน\n4. ตั้งขาเข้าของแทร็ก Stereo เป็น App Audio แล้วกดอัด",
      "Studio One:\n1. Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo), name it App Audio\n2. In the Console show the Inputs and put this plug-in on App Audio\n3. Turn on \"Program only\" above\n4. Set a stereo track's input to App Audio and record");
    s(Str::AppStepsCubase, "Cubase:\n1. ใส่ปลั๊กอินนี้ที่ Input Channel ใน MixConsole\n2. เปิด \"เฉพาะเสียงโปรแกรม\" ข้างบน\n3. ตั้งขาเข้าของแทร็กเป็น Input นั้นแล้วกดอัด",
      "Cubase:\n1. Put this plug-in on an Input Channel in the MixConsole\n2. Turn on \"Program only\" above\n3. Set a track's input to that input and record");
    s(Str::AppStepsReaper, "Reaper:\n1. ใส่ปลั๊กอินนี้ใน Input FX ของแทร็กที่จะอัด\n2. เปิด \"เฉพาะเสียงโปรแกรม\" ข้างบน\n3. Arm แทร็กแล้วกดอัด",
      "Reaper:\n1. Put this plug-in in the Input FX of the track you record on\n2. Turn on \"Program only\" above\n3. Arm the track and record");
    s(Str::AppStepsOther, "DAW นี้: ถ้ามีช่อง Input ให้ใส่ปลั๊กอินนี้ที่นั่น เปิด \"เฉพาะเสียงโปรแกรม\" แล้วอัดแทร็กจาก Input นั้น",
      "This DAW: if it has input channels, put this plug-in there, turn on \"Program only\" and record a track from that input");
    s(Str::AppHintNormal, "ถ้าใส่ในแทร็กปกติ คุณและคนดูได้ยินเสียงโปรแกรม แต่ DAW ไม่อัด", "On a normal track you and the viewers hear it, but the DAW doesn't record it.");
    s(Str::StepsOtherDaws, "ดูวิธีของ DAW อื่น", "Steps for other DAWs");
    s(Str::StepsThisDaw, "ดูเฉพาะ DAW นี้", "Only this DAW");
    s(Str::SharePermanent, "ลิงก์ถาวร ส่งครั้งเดียวใช้ได้ทุกไลฟ์", "Permanent link - send it once, use it every stream");
    s(Str::ShareDirOnline, "ลิงก์ถาวรออนไลน์ ส่งครั้งเดียวใช้ได้ทุกไลฟ์", "Permanent link online - send it once, use it every stream");
    s(Str::ShareDirRegistering, "กำลังเชื่อมลิงก์ถาวร...", "Connecting the permanent link...");
    s(Str::ShareDirOffline, "ลิงก์ถาวรใช้ไม่ได้ชั่วคราว ใช้ลิงก์สำรองไปก่อน กำลังลองใหม่...", "The permanent link is unavailable right now - use the backup link; retrying...");
    s(Str::ShareDirNeedsTunnel, "ลิงก์ถาวรต้องใช้ cloudflared: ติดตั้งตามข้างล่าง แล้วปิดเปิดแชร์ใหม่ ระหว่างนี้ใช้ได้เฉพาะ WiFi เดียวกัน",
      "The permanent link needs cloudflared: install it (below), then switch sharing off and on. Wi-Fi only meanwhile");
    s(Str::ShareNoBase, "ลิงก์นี้เปลี่ยนทุกครั้งที่เปิดแชร์ ตั้ง \"ที่อยู่เว็บแชร์\" ในการตั้งค่า (ปุ่มเฟือง) เพื่อได้ลิงก์ถาวร",
      "This link changes every time you share. Set the \"Share web address\" in Settings (gear) for a permanent link");
    s(Str::ShareBackup, "ลิงก์สำรองของวันนี้ (ใช้ตอนลิงก์ถาวรมีปัญหา)", "Today's backup link (if the permanent one has trouble)");
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
    s(Str::SetupSignalBad, "คนดูไม่ได้ยินอะไรเลย ทั้งที่ \"%t\" มีเสียงอยู่", "The viewers hear nothing although \"%t\" is playing");
    s(Str::SetupSignalFix, "ตรวจว่า Hub อยู่บน Master, fader ของแทร็กและ Master ไม่ได้ลดสุด, ปลั๊กอินเหนือ Hub ไม่ได้ปิดเสียง",
      "Check that the Hub is on the master bus, the track and master faders are up, and no plug-in above the Hub mutes the sound");
    s(Str::SetupRateOk, "Sample rate ตรงกันทุกแทร็ก", "Every track runs at the Hub's sample rate");
    s(Str::SetupRateBad, "Sample rate ไม่ตรงกับ Hub:", "Sample rate differs from the Hub:");
    s(Str::SetupRateViewers, "ไม่ได้ยินแทร็กเหล่านี้", "don't get these tracks");
    s(Str::SetupRateFix, "ตั้ง sample rate ของโปรเจกต์ให้เท่ากันทั้งหมด", "Use one sample rate for the whole project");
    s(Str::SetupBypassOk, "ไม่มี HEARASIDE Track ที่ถูก bypass", "No HEARASIDE Track is bypassed");
    s(Str::SetupBypassBad, "HEARASIDE Track ถูก bypass:", "HEARASIDE Track is bypassed on:");
    s(Str::SetupBypassYou, "ได้ยินแทร็กนี้เสมอ แม้ปิด \"คุณได้ยิน\"", "you always hear it, even with \"You hear\" off");
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
    s(Str::AppOnly, "เฉพาะเสียงโปรแกรม (ตัดเสียงเดิมของช่อง)", "Program only (drop the channel's own sound)");
    s(Str::AppOnlyTip, "ใช้กับช่อง Input ที่ทำไว้อัดเสียงโปรแกรม: ไม่มีเสียงไมค์หรือเสียงอื่นปนในไฟล์ที่อัด",
      "For an input channel made for the program: no mic or other sound ends up in the recording");
    s(Str::AppSubtitle, "ดึงเสียงจากโปรแกรมเข้าแทร็กนี้", "Brings a program's sound into this track");
    s(Str::AppSystem, "เสียงทั้งเครื่อง (ยกเว้น DAW)", "Whole computer (except the DAW)");
    s(Str::AppOff, "ปิดอยู่ แทร็กนี้ปล่อยเสียงเดิมผ่านอย่างเดียว", "Off - the track passes its own sound only");
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
      "Whenever the DAW records, the program's sound is printed to its own take, lined up with the timeline");
    s(Str::TakeDrag, "ลากไปวางในแทร็ก", "Drag into a track");
    s(Str::TakeNone, "ยังไม่มีไฟล์ที่อัด", "No takes yet");
    s(Str::OpenFolder, "เปิดโฟลเดอร์", "Open folder");
    s(Str::TakeDropped, "ดิสก์ช้า เสียงบางช่วงหาย", "Disk too slow - some audio was lost");
    s(Str::PrintHint, "ไฟล์ WAV เก็บที่ Documents\\HEARASIDE\\Recordings ลากไปวางแทร็กไหนก็ได้ ใน Cubase / Reaper ใช้ Move to origin จะวางตรงตำแหน่งที่อัด",
      "WAV takes go to Documents\\HEARASIDE\\Recordings. Drag one onto any track; in Cubase / Reaper \"Move to origin\" puts it where it was recorded.");
    s(Str::SourcesTitle, "เสียงจากโปรแกรม", "Program audio");
    s(Str::SourcesSubtitle, "HEARASIDE App Audio ทุกตัว เลือกได้ว่าคุณหรือคนดูได้ยิน เปิดปิดและอัดเสียงที่ปุ่ม ...",
      "Every HEARASIDE App Audio: choose who hears it; switch and record from the ... button");
    s(Str::RecordTip, "อัดเสียงโปรแกรมลงไฟล์ WAV แล้วลากไปวางในแทร็ก", "Print the program's sound to a WAV take you can drag into a track");
    s(Str::SyncButton, "ซิงค์เสียงร้องอัตโนมัติ", "Auto-sync vocal");
    s(Str::SyncTitle, "ซิงค์เสียงร้องกับเพลง (ฝั่งคนดู)", "Line the vocal up with the music (viewers)");
    s(Str::SyncHow, "1. เปิดเพลงไว้ (อย่าเพิ่งร้อง)\n2. กดเริ่ม แล้วเอาหูฟังจ่อไมค์ราว 7 วินาที ระหว่างนี้คนดูจะไม่ได้ยินเสียง\n3. ระบบหน่วงเพลงหรือเสียงร้องฝั่งคนดูให้ตรงกันเอง หูฟังของคุณเหมือนเดิม\nกดใหม่เมื่อเพิ่มหรือถอดปลั๊กอินที่มีดีเลย์ หรือเปลี่ยน buffer",
      "1. Play the music (don't sing yet)\n2. Press Start and hold the headphones to the mic for about 7 s; the viewers hear nothing meanwhile\n3. The music or the vocal is delayed for the viewers so they line up; your headphones stay the same\nPress again after adding or removing a plug-in with latency, or changing the buffer");
    s(Str::SyncMic, "แทร็กไมค์ (เสียงร้อง)", "Mic track (vocal)");
    s(Str::SyncRef, "แทร็กเพลง", "Music track");
    s(Str::SyncStart, "เริ่มวัด", "Start");
    s(Str::SyncCancel, "ยกเลิก", "Cancel");
    s(Str::SyncMeasuringRef, "กำลังฟังแทร็กเพลง...", "Listening to the music track...");
    s(Str::SyncMeasuringMic, "กำลังฟังไมค์...", "Listening to the mic...");
    s(Str::SyncLate, "เสียงร้องช้ากว่าเพลง", "The vocal was late by");
    s(Str::SyncEarly, "เสียงร้องเร็วกว่าเพลง", "The vocal was early by");
    s(Str::SyncInTime, "ชดเชยฝั่งคนดูแล้ว (หูฟังคุณจะเหมือนเดิม กด \"ฟังแบบคนดู\" เพื่อเช็ค)",
      "fixed for the viewers (your headphones stay the same - press \"Hear viewers' mix\" to check)");
    s(Str::SyncNeedTracks, "ต้องมี HEARASIDE Track บนแทร็กไมค์ และมี App Audio (หรือแทร็กเพลงที่มี HEARASIDE Track) เป็นเพลงอ้างอิง",
      "The mic track needs HEARASIDE Track, and the music has to come from App Audio (or a track with HEARASIDE Track)");
    s(Str::SyncCountdown, "เปิดเพลงไว้ เอาหูฟังจ่อไมค์ และอย่าเพิ่งร้อง เริ่มวัดใน", "Play the music, hold the headphones to the mic and stay quiet. Measuring in");
    s(Str::SyncOptions, "เลือกแทร็กที่ใช้ซิงค์", "Choose the tracks to sync");
    s(Str::SyncNoAudio, "DAW ไม่ได้ส่งเสียงมาเลย เปิดเล่นหรือเปิด monitor แล้วลองใหม่", "The DAW isn't running audio - start playback or monitoring and try again");
    s(Str::SyncNoMusic, "ไม่ได้ยินแทร็กเพลง เปิดเพลงแล้วลองใหม่", "The music track is silent - play the music and try again");
    s(Str::SyncNoMic, "ไมค์ไม่ได้ยินเพลง เอาหูฟังจ่อไมค์ให้ใกล้ขึ้นหรือเพิ่มเสียง แล้วลองใหม่", "The mic doesn't hear the music - hold the headphones closer or louder and try again");
    s(Str::SyncWeakMic, "ไมค์ได้ยินเพลงจากหูฟังไม่ชัด ปลั๊กอินลดเสียงรบกวนบนแทร็กไมค์ (เช่น NS1 หรือ noise gate) อาจตัดทิ้ง ปิดชั่วคราวตอนวัด หรือเอาหูฟังจ่อไมค์ให้ชิดและดังขึ้น",
      "The mic barely hears the music from the headphones - a noise suppressor or gate on the mic track (NS1...) may remove it. Bypass it while measuring, or hold the headphones closer and louder");
    s(Str::SyncUnsteady, "วัดหลายรอบแล้วได้ค่าไม่ตรงกัน (อาจมีเสียงพูดหรือเสียงรบกวน) เลยยังไม่เปลี่ยน delay เงียบไว้แล้วลองใหม่",
      "The measurements disagreed (talking or noise?), so the delays were left alone - stay quiet and try again");
    s(Str::AppLinkIn, "เสียงที่เพื่อนส่งเข้ามา (ลิงก์ส่งเสียง)", "Sent in through my send link");
    s(Str::AppLinkFrom, "ลิงก์", "Link");
    s(Str::AppLinkWaiting, "ยังไม่มีใครส่งเสียงเข้ามา ส่ง \"ลิงก์ส่งเสียงเข้า\" จาก Hub ให้เพื่อน", "Nobody is sending yet - give a friend the send link from the Hub");
    s(Str::AppLinkOffline, "ลิงก์นี้ยังไม่ออนไลน์ (อีกฝั่งยังไม่เปิดแชร์) จะต่อให้เองเมื่อพร้อม", "The link is offline (not shared right now); it connects by itself when it is back");
    s(Str::AppLinkAsk, "รับจากลิงก์ของคนอื่น...", "Receive someone's link...");
    s(Str::AppLinkPrompt, "วางลิงก์ฟังสดของ HEARASIDE เครื่องอื่น (ขึ้นต้นด้วย https:// และมี /l/)", "Paste another HEARASIDE listen link (https://.../l/...)");
    s(Str::AppLinkBad, "ลิงก์ไม่ถูกต้อง ต้องเป็นลิงก์ฟังสดของ HEARASIDE", "That isn't a HEARASIDE listen link");
    s(Str::AppLinkReceiving, "รับเสียงจากลิงก์อยู่", "Receiving the link");
    s(Str::ShareTip, "แชร์ลิงก์ให้คนอื่นฟัง หรือให้เพื่อนส่งเสียงเข้ามา (แบบ LISTENTO)", "Share a listen link, or let a friend send audio in (like LISTENTO)");
    s(Str::ShareTitle, "แชร์เสียง (แบบ LISTENTO)", "Share audio (like LISTENTO)");
    s(Str::ShareOn, "เปิดแชร์", "Sharing");
    s(Str::ShareListen, "ลิงก์ฟังสด", "Listen link");
    s(Str::ShareListenCap, "ส่งให้เพื่อนหรือลูกค้าเปิดในเบราว์เซอร์ ฟังเสียงที่คนดูได้ยิน คุณภาพไม่บีบอัด", "Anyone with it hears the viewers' mix in a browser, uncompressed");
    s(Str::ShareSend, "ลิงก์ส่งเสียงเข้า", "Send-in link");
    s(Str::ShareSendCap, "ให้เพื่อนเปิดแล้วกดส่งไมค์เข้ามา แล้วเลือก \"เสียงที่เพื่อนส่งเข้ามา\" ใน App Audio", "A friend opens it and sends their mic; pick \"Sent in through my send link\" in App Audio");
    s(Str::Copy, "คัดลอก", "Copy");
    s(Str::Copied, "คัดลอกแล้ว", "Copied");
    s(Str::OpenLink, "เปิด", "Open");
    s(Str::Listeners, "ผู้ฟัง", "Listening");
    s(Str::SenderOn, "มีคนส่งเสียงเข้ามาอยู่", "Someone is sending audio in");
    s(Str::SenderOff, "ยังไม่มีใครส่งเสียงเข้ามา", "Nobody is sending audio in");
    s(Str::TunnelReady, "ลิงก์ใช้ได้ทุกที่ผ่านเน็ต (Cloudflare)", "The links work anywhere (via Cloudflare)");
    s(Str::TunnelStarting, "กำลังเปิดลิงก์ผ่านเน็ต...", "Opening the internet link...");
    s(Str::TunnelMissing, "ตอนนี้ใช้ได้เฉพาะคนที่ต่อ WiFi เดียวกัน ติดตั้ง cloudflared (ฟรี) เพื่อแชร์ผ่านเน็ต แล้วปิดเปิดแชร์ใหม่", "For now only devices on your Wi-Fi can open them. Install cloudflared (free) to share over the internet, then turn sharing off and on");
    s(Str::TunnelFailed, "เปิดลิงก์ผ่านเน็ตไม่สำเร็จ ใช้ได้เฉพาะ WiFi เดียวกันไปก่อน", "Couldn't open the internet link - Wi-Fi only for now");
    s(Str::CopyInstall, "คัดลอกคำสั่งติดตั้ง", "Copy install command");
    s(Str::SharingChip, "แชร์อยู่", "Sharing");
    s(Str::PowerTip, "เปิด / ปิดการดึงเสียงจากโปรแกรม", "Switch program capture on / off");
    s(Str::ChainLatency, "ปลั๊กอินก่อนหน้าหน่วง", "Plug-ins before this add");
    s(Str::DawBuffer, "บัฟเฟอร์ DAW", "DAW buffer");
    s(Str::Samples, "sample", "samples");
    s(Str::LatencyTip, "เสียงถึงคนดูช้าเท่าไร แยกตามส่วน:\nDAW = buffer ของ DAW\nปลั๊กอินแทร็ก = แทร็กที่ปลั๊กอินหน่วงที่สุด (วัดเอง)\nปลั๊กอิน Master = ปลั๊กอินเหนือ Hub (วัดเอง)\nHub = กันเสียงพีค\nOBS = buffer ของ HEARASIDE ใน OBS\n(ไม่รวม latency ของ audio interface ที่ปลั๊กอินมองไม่เห็น)",
      "How late the viewers get the sound, part by part:\nDAW = the DAW buffer\nTrack plug-ins = the slowest track's plug-ins (measured)\nMaster plug-ins = the plug-ins above the Hub (measured)\nHub = peak protection\nOBS = the HEARASIDE buffer in OBS\n(audio-interface latency is invisible to plug-ins)");
    s(Str::PillPrefixYou, "คุณ", "You: ");
    s(Str::PillPrefixViewers, "คนดู", "Viewers: ");
    s(Str::LevelsTitle, "ระดับเสียงรวม", "Levels");
    s(Str::LevelsTip, "ระดับรวมไปหาคนดู, ระดับหูฟังรวม และกันเสียงพีค", "Stream level, headphone master and peak protection");
    s(Str::SummaryOnlyViewers, "คนดูได้ยิน แต่คุณไม่ได้ยิน", "Viewers hear, you don't");
    s(Str::StartTitle, "เริ่มใช้งาน 3 ขั้น", "Get started in 3 steps");
    s(Str::StartStep1, "ใส่ HEARASIDE Track เป็นปลั๊กอินตัวสุดท้ายของแทร็กที่ต้องการ", "Put HEARASIDE Track last on every track you want here");
    s(Str::StartStep2, "เปิด OBS แล้วเพิ่ม Source ชื่อ HEARASIDE", "Open OBS and add a HEARASIDE source");
    s(Str::StartStep3, "กด \"ฟังแบบคนดู\" ฟังเสียงที่ส่งไปหาคนดูสักครั้ง", "Press \"Hear viewers' mix\" once to check what you send");
    s(Str::StartHide, "ซ่อน", "Hide");
    s(Str::StartStep1StudioOne, "Studio One: ลากจาก Browser › Effects ไปวางล่างสุดของช่อง Inserts", "Studio One: drag it from Browser › Effects to the bottom of the Inserts");
    s(Str::StartStep1Cubase, "Cubase: ใส่ในช่อง Insert ล่างสุดของแทร็ก", "Cubase: use the track's last insert slot");
    s(Str::StartStep1Reaper, "Reaper: ปุ่ม FX ของแทร็ก › Add แล้วให้อยู่ล่างสุดของรายการ", "Reaper: the track's FX button › Add, at the bottom of the list");
    s(Str::StartStep1Fl, "FL Studio: ช่อง Mixer ของแทร็ก ใส่ใน slot ล่างสุด", "FL Studio: the track's Mixer insert, last slot");
    s(Str::StartStep1Ableton, "Ableton Live: วางขวาสุดของ Device chain ของแทร็ก", "Ableton Live: the far right of the track's device chain");
    s(Str::SyncTip, "ตั้งดีเลย์ให้เสียงร้องตรงกับเพลงฝั่งคนดู กดแล้วทำตามที่ขึ้นบนจอ (ปุ่ม ⋯ ข้าง ๆ ดูขั้นตอนและเลือกแทร็กเอง)",
      "Lines your vocal up with the music for the viewers. Press it and follow the screen (the ... button shows the steps and lets you pick the tracks)");
    s(Str::LevelsColumn, "ระดับเสียง", "Levels");
    s(Str::RowLevels, "ระดับในหูฟัง / ฝั่งคนดู...", "Headphone / viewers level...");
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

juce::String formatDb(float db, bool withUnit) {
    const juce::String minus = juce::String(juce::CharPointer_UTF8("\xe2\x88\x92"));   // U+2212
    if (db <= -30.0f + 1.0e-4f) return minus + juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e"));   // −∞
    juce::String s = juce::String(std::abs(db), 1);
    if (db > 0.04f) s = "+" + s;
    else if (db < -0.04f) s = minus + s;
    return withUnit ? s + " dB" : s;
}

} // namespace hearaside
