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

    s(Str::SceneSinging, "ร้องเพลง", "Singing");
    s(Str::SceneTalking, "คุยกับคนดู", "Talking");
    s(Str::SceneBrb, "พักจอ", "Be right back");
    s(Str::SceneCustom, "ซีน", "Scene");
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
    s(Str::StreamSubtitle, "ส่งเข้า OBS โดยตรง", "Sent straight to OBS");
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
    s(Str::SecondHubBody, "มี Hub ทำงานอยู่แล้วในโปรเจกต์นี้ ตัวนี้จะปล่อยเสียงผ่านอย่างเดียว",
      "A Hub is already running in this project. This one only passes audio through.");
    s(Str::HubBusError, "เปิดช่องสื่อสารไม่ได้ Hub จะปล่อยเสียงผ่านอย่างเดียว",
      "Could not open the HEARASIDE bus. The Hub only passes audio through.");

    s(Str::StreamSolo, "Solo ฝั่งคนดู", "Solo for viewers");
    s(Str::AdvancedSettings, "ตั้งค่าละเอียด", "Fine settings");
    s(Str::RenameDisplay, "เปลี่ยนชื่อที่แสดง", "Rename");
    s(Str::SaveToScene, "บันทึกค่าปัจจุบันลงซีนนี้", "Save current setup to this scene");
    s(Str::RenameScene, "เปลี่ยนชื่อ", "Rename");
    s(Str::ClearScene, "ล้างซีนนี้", "Clear this scene");
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
    s(Str::SyncSafetyHint, "เพิ่มเมื่อบางแทร็กเหลื่อมกัน (ทุก 1 block = ช้าลง 1 buffer)",
      "Raise if some tracks drift apart (each block adds one buffer of delay)");
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
    s(Str::SaveAsNewScene, "บันทึกค่าปัจจุบันเป็นซีนใหม่", "Save current setup as a new scene");
    s(Str::Approx, "ประมาณ", "about");
    s(Str::SceneHint, "ซีนนี้ยังไม่ได้บันทึก คลิกขวาเพื่อบันทึกค่าปัจจุบันลงซีนนี้",
      "This scene is empty. Right-click it to save the current setup.");
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
