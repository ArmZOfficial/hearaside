# HEARASIDE

ปลั๊กอิน VST3 (และ VST2 ถ้ามี license) สำหรับสตรีมเมอร์สายดนตรี แยก **"เสียงที่คุณได้ยิน"** ในหูฟัง ออกจาก **"เสียงที่คนดูได้ยิน"** แล้วส่งเสียงฝั่งคนดูเข้า OBS โดยตรง ไม่ต้องใช้ virtual cable

> ตัวอย่างเคสหลัก: แทร็กร้องตั้ง `คุณได้ยิน = ปิด`, `คนดูได้ยิน = เปิด` คนดูได้ยินเสียงร้องที่ผ่าน EQ / Comp / Reverb แล้ว แต่ในหูฟังคุณไม่ได้ยินเสียงตัวเอง

แผนฉบับเต็ม: [docs/plan.md](docs/plan.md)

---

## ส่วนประกอบ

| ชิ้นส่วน | อยู่ที่ไหน | หน้าที่ |
|---|---|---|
| **HEARASIDE Track** | Insert ท้ายสุดของแต่ละแทร็กใน DAW | สวิตช์ "คุณได้ยิน" / "คนดูได้ยิน", ระดับในหูฟัง, ระดับฝั่งคนดู, หน่วงเวลาฝั่งคนดู ต่อแทร็ก |
| **HEARASIDE Hub** | Insert บน Master bus (ตัวเดียวต่อโปรเจกต์) | รวมแทร็กเป็น Stream Mix, mixer กลาง (รีโมตทุกแทร็ก), limiter, LUFS, ซีน, ปุ่มตัดเสียงคนดู, ฟังแบบคนดู, ระดับหูฟังรวม |
| **HEARASIDE OBS Source** | ปลั๊กอินใน OBS | อ่าน Stream Mix จาก shared memory, ชดเชย clock drift, แปลง sample rate |
| *Bridge* (เสริม) | แอปบน tray | ส่ง Stream Mix ออก audio device อื่น (VB-CABLE ฯลฯ) |

---

## สถานะ (7 ต.ค. 2026)

### ✅ เสร็จแล้ว

| โฟลเดอร์ | เนื้อหา |
|---|---|
| `libs/ssbus` | Shared memory protocol v1 บน Windows (security descriptor ให้ DAW/OBS ที่รันแบบ admin ใช้ร่วมกับโปรเซสปกติได้) และ macOS (POSIX shm), broadcast ring buffer แบบไม่ใช้ lock (writer ไม่เคยรอ reader), การจอง/คืน slot, กู้ slot ของโปรเซสที่ตาย, จัดการ UUID ซ้ำเมื่อ duplicate แทร็ก, mailbox คำสั่ง Hub → Track, ขอเปลี่ยนชื่อแทร็ก, timeline tag |
| `libs/ssdsp` | Smoother, equal-power pan, brick-wall limiter แบบ lookahead (รับประกันไม่เกิน ceiling), LUFS meter ตาม ITU-R BS.1770 (M / S), ตัวควบคุม drift แบบ PI, resampler ปรับ ratio ได้ละเอียด 5–15 ppm (speexdsp), `StreamConsumer` สำหรับฝั่ง OBS / Bridge |
| `libs/ssengine` | `TrackPublisher` และ `HubEngine` (sync แทร็กด้วย timeline tag จึงรองรับ ASIO-Guard / Anticipative FX, re-anchor เมื่อ seek / หยุด / เล่น, sync safety, stream delay สูงสุด 500 ms ที่อ่านย้อนจาก FIFO โดยไม่ใช้หน่วยความจำเพิ่ม, crossfade, solo, panic, stem 8 ช่อง, limiter, ฟังแบบคนดู) |
| `plugins/track` | **HEARASIDE Track** (VST3 + VST2): คุณได้ยิน / คนดูได้ยิน, **ระดับในหูฟัง** (ลดเสียงที่คุณได้ยินโดยคนดูยังได้ยินเท่าเดิม ไม่ต้องแตะ fader), ระดับฝั่งคนดู, **หน่วงเวลาฝั่งคนดู 0–500 ms** (เหมือน Sync Offset ของ OBS แต่ทำทีละแทร็ก เช่น ชดเชยปลั๊กอินร้องที่หน่วง), pan / stem / solo / ชื่อ / bus ในแผงตั้งค่าละเอียด, ชื่อ+สีแทร็กจาก DAW (VST3), รับคำสั่งจาก Hub ผ่าน host (undo / automation / save ได้) |
| `plugins/hub` | **HEARASIDE Hub** (VST3 + VST2): หน้าจอตาม mock-up (`StreamSplit Plugin UI.html`), รีโมตทุกแทร็ก (ปุ่มได้ยิน/ไม่ได้ยิน + สไลเดอร์ระดับหูฟัง/คนดูในแต่ละแถว), ซีน 8 ช่อง (บันทึก / เปลี่ยนชื่อ / automate ผ่านพารามิเตอร์ `scene`), **ระดับหูฟังรวม** (ไม่มีผลกับคนดูและตอน export), ตัดเสียงคนดู, ฟังแบบคนดู, กันเสียงพีค, LUFS, สถานะ OBS + ความหน่วงถึง OBS, ตั้งค่า bus / sync safety / ชื่อ stem |
| `plugins/common` | UI "Frosted Studio" แบบ native JUCE: กระจกขุ่น pre-render, ธีมสว่าง/มืด/ตามระบบ, ไทย/อังกฤษ, ฟอนต์ Anuphan ฝังในปลั๊กอิน, ขนาด 100–200 %, ลดการเคลื่อนไหว; design tokens จาก `design/tokens.json` → `Theme.h` |
| `obs/hearaside-obs` | **OBS Source** (OBS 32): เลือก bus / Stream Mix หรือ Stem 1–8 / buffer 15–60 ms, แสดงสถานะ (DAW rate, buffer, drift, underrun), ชดเชย drift + แปลง sample rate, ไม่พึ่ง DLL อื่นนอกจาก `obs.dll` |
| `tools/` | `bus-inspector` (ดู bus แบบ real-time), `ui-snapshot` (เรนเดอร์หน้าจอปลั๊กอินเป็น PNG), `plugin-host-test` (โหลด VST3/VST2 ที่ build แล้วแบบ DAW แล้วตรวจ end-to-end) |
| `installer/windows/install.ps1` | ติดตั้ง VST3 / VST2 / OBS plugin ลงโฟลเดอร์มาตรฐาน (และ `-Uninstall`) |

### ผลทดสอบล่าสุด (`.\build.ps1 -Test`): 5/5 ผ่าน

- ✅ **unit** 40 ชุด (ผ่านทั้ง Release และ AddressSanitizer) รวม e2e Hub → OBS consumer 44.1k → 48k + drift 150 ppm ไม่มี underrun / เสียงคลิก
- ✅ **ipc** ข้ามโปรเซส 242,944 frames ตรงทุก sample, คืน slot ของโปรเซสที่ถูก kill ได้
- ✅ **plugin_host_vst3 / plugin_host_vst2** โหลดไฟล์ปลั๊กอินจริงแบบ DAW: null test (เหลือ −240 dBFS), ระดับในหูฟัง −20 dB ลดเฉพาะเสียงออก DAW (คนดูยังได้ 100 %), ปิด "คุณได้ยิน" แล้วคนดูยังได้ยิน, หน่วง 40 ms ตรงกับสำเนาที่ช้า 1920 sample พอดี, คำสั่งจาก Hub เปลี่ยนพารามิเตอร์ที่ host เห็น, save/restore state
- ✅ **design_tokens_in_sync**

### ⏳ ยังไม่ได้ทำ

1. ติดตั้งลงเครื่องแล้วทดสอบกับ DAW จริง (Cubase 15 / Studio One 7 / Reaper / FL) และ OBS 32 ตาม host matrix ในแผน
2. pluginval strictness 10
3. Bridge app, installer แบบ Inno Setup, คู่มือ, build macOS

### เรื่อง VST2

Steinberg เปลี่ยน license เป็น MIT เฉพาะ **VST3** (ตั้งแต่ VST 3.8, ต.ค. 2025) ส่วน VST2 SDK ยังไม่เปิดให้ใช้ทั่วไป โปรเจกต์นี้จึง build VST3 เป็นหลัก และเปิด VST2 ได้เมื่อมี SDK ที่ถูกลิขสิทธิ์:

```powershell
.\build.ps1 -Vst2Sdk C:\path\to\licensed\vst2sdk
```

---

## วิธี build (Windows)

ต้องมี Visual Studio 2022 Build Tools (C++), CMake ≥ 3.25, Ninja, Git และ Python (ถ้าต้องการ)

```powershell
.\scripts\fetch-deps.ps1        # JUCE 8.0.15, libobs 32.2.2 headers, speexdsp
.\build.ps1 -Test               # build + รัน test (OBS plugin ต้องมี OBS ติดตั้งที่ C:\Program Files\obs-studio)
.\build.ps1 -Vst2Sdk C:\path\to\vstsdk2.4 -Test   # + VST2 (ห้าม commit SDK เข้า repo)
.\installer\windows\install.ps1  # (Administrator) ติดตั้ง VST3 / VST2 / OBS plugin
.\build\Release\tools\bus-inspector\bus-inspector.exe Main
```

ใน OBS: เพิ่ม Source ชื่อ **HEARASIDE** แล้วตั้ง Audio Monitoring เป็น "Monitor Off" (ฟังจาก DAW อยู่แล้ว) และถ้า DAW เล่นผ่าน Desktop Audio ให้ปิด Desktop Audio ไม่งั้นคนดูได้ยินซ้ำ

## Dependencies และ License

| | License |
|---|---|
| แกนกลาง `libs/*` | MIT |
| JUCE 8 | AGPLv3 หรือ commercial |
| VST3 SDK | MIT (ตั้งแต่ 3.8) |
| OBS / libobs | GPLv2 (OBS plugin ต้องเปิด source) |
| speexdsp | BSD |
| Anuphan (`external/fonts`) | SIL OFL 1.1 |
