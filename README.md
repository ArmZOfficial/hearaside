# HEARASIDE

ปลั๊กอิน VST3 (และ VST2 ถ้ามี license) สำหรับสตรีมเมอร์สายดนตรี แยก **"เสียงที่คุณได้ยิน"** ในหูฟัง ออกจาก **"เสียงที่คนดูได้ยิน"** แล้วส่งเสียงฝั่งคนดูเข้า OBS โดยตรง ไม่ต้องใช้ virtual cable

> ตัวอย่างเคสหลัก: แทร็กร้องตั้ง `คุณได้ยิน = ปิด`, `คนดูได้ยิน = เปิด` คนดูได้ยินเสียงร้องที่ผ่าน EQ / Comp / Reverb แล้ว แต่ในหูฟังคุณไม่ได้ยินเสียงตัวเอง

แผนฉบับเต็ม: [docs/plan.md](docs/plan.md)

> ⚠️ ในโค้ดตอนนี้ยังใช้ชื่อชั่วคราว **StreamSplit** อยู่ (ชื่อ namespace, ชื่อ shared memory, ข้อความ) และจะเปลี่ยนเป็น HEARASIDE ทั้งหมดในขั้นถัดไป

---

## ส่วนประกอบ

| ชิ้นส่วน | อยู่ที่ไหน | หน้าที่ |
|---|---|---|
| **HEARASIDE Track** | Insert ท้ายสุดของแต่ละแทร็กใน DAW | สวิตช์ "คุณได้ยิน" / "คนดูได้ยิน" ต่อแทร็ก |
| **HEARASIDE Hub** | Insert บน Master bus (ตัวเดียวต่อโปรเจกต์) | รวมแทร็กเป็น Stream Mix, mixer กลาง, limiter, LUFS, ซีน, ปุ่มตัดเสียงคนดู, ฟังแบบคนดู |
| **HEARASIDE OBS Source** | ปลั๊กอินใน OBS | อ่าน Stream Mix จาก shared memory, ชดเชย clock drift, แปลง sample rate |
| *Bridge* (เสริม) | แอปบน tray | ส่ง Stream Mix ออก audio device อื่น (VB-CABLE ฯลฯ) |

---

## สถานะ (7 ต.ค. 2026)

### ✅ เสร็จแล้ว: แกนกลาง (C++20 ไม่พึ่ง JUCE, license MIT)

| โฟลเดอร์ | เนื้อหา |
|---|---|
| `libs/ssbus` | Shared memory protocol v1 บน Windows (security descriptor ให้ DAW/OBS ที่รันแบบ admin ใช้ร่วมกับโปรเซสปกติได้) และ macOS (POSIX shm), broadcast ring buffer แบบไม่ใช้ lock (writer ไม่เคยรอ reader), การจอง/คืน slot, กู้ slot ของโปรเซสที่ตาย, จัดการ UUID ซ้ำเมื่อ duplicate แทร็ก, mailbox คำสั่ง Hub → Track, ขอเปลี่ยนชื่อแทร็ก, timeline tag |
| `libs/ssdsp` | Smoother, equal-power pan, brick-wall limiter แบบ lookahead (รับประกันไม่เกิน ceiling), LUFS meter ตาม ITU-R BS.1770 (M / S), ตัวควบคุม drift แบบ PI, resampler ปรับ ratio ได้ละเอียด 1 ppm (speexdsp), `StreamConsumer` สำหรับฝั่ง OBS / Bridge |
| `libs/ssengine` | `TrackPublisher` (ส่งเสียง + tag ตำแหน่ง timeline) และ `HubEngine` (sync แทร็กด้วย timeline tag จึงรองรับ ASIO-Guard / Anticipative FX, re-anchor เมื่อ seek / หยุด / เล่น, sync safety, stream delay ที่อ่านย้อนจาก FIFO โดยไม่ใช้หน่วยความจำเพิ่ม, crossfade, solo, panic, stem 8 ช่อง, limiter, ฟังแบบคนดู) |
| `tools/bus-inspector` | CLI ดู slot / heartbeat / meter / สถานะ Hub แบบ real-time |
| `tests/` | unit test, จำลอง host (null test ในกรณี block size ไม่คงที่, ahead processing, seek, แทร็กที่ถูกประมวลผลหลัง master, แทร็ก suspend, delay, stem, sample rate ไม่ตรง ฯลฯ), test ข้ามโปรเซส |
| `build.ps1`, `scripts/fetch-deps.ps1` | สคริปต์ build (MSVC + Ninja) และสคริปต์ดึง dependencies |

### ผลทดสอบล่าสุด

- ✅ **IPC ข้ามโปรเซส:** ส่ง 241,920 frames ไปอีกโปรเซสได้ตรงทุก sample, ไม่มี overrun, คืน slot ของโปรเซสที่ถูก kill ได้ถูกต้อง
- ❌ **`ss_unit_tests` crash (access violation)** ก่อนที่จะพิมพ์ผลข้อแรกออกมา ยังไม่ได้หาสาเหตุ (เป็นงานแรกที่ต้องทำต่อ)

### ⏳ ยังไม่ได้ทำ

1. แก้ unit test ที่ crash แล้วรันให้ผ่านทุกข้อ
2. เปลี่ยนชื่อ StreamSplit → **HEARASIDE** ทั้งหมด
3. ปลั๊กอิน JUCE: Track + Hub พร้อม UI แนว "Frosted Studio" ทั้งธีมสว่างและมืด, ภาษาไทย / อังกฤษ, ฟอนต์ Anuphan
4. OBS Source plugin (libobs 32.2.x)
5. ติดตั้งลงเครื่องแล้วทดสอบกับ Cubase 15 / Studio One 7 / OBS 32
6. Bridge app, installer, คู่มือ

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
.\build.ps1 -Test               # build + รัน test
.\build\Release\tools\bus-inspector\bus-inspector.exe Main
```

## Dependencies และ License

| | License |
|---|---|
| แกนกลาง `libs/*` | MIT |
| JUCE 8 | AGPLv3 หรือ commercial |
| VST3 SDK | MIT (ตั้งแต่ 3.8) |
| OBS / libobs | GPLv2 (OBS plugin ต้องเปิด source) |
| speexdsp | BSD |
| Anuphan (`external/fonts`) | SIL OFL 1.1 |
