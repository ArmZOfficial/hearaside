# HEARASIDE

ปลั๊กอิน VST3 (และ VST2 ถ้ามี license) สำหรับสตรีมเมอร์สายดนตรี แยก **"เสียงที่คุณได้ยิน"** ในหูฟัง ออกจาก **"เสียงที่คนดูได้ยิน"** แล้วส่งเสียงฝั่งคนดูเข้า OBS โดยตรง ไม่ต้องใช้ virtual cable

> ตัวอย่างเคสหลัก: แทร็กร้องตั้ง `คุณได้ยิน = ปิด`, `คนดูได้ยิน = เปิด` คนดูได้ยินเสียงร้องที่ผ่าน EQ / Comp / Reverb แล้ว แต่ในหูฟังคุณไม่ได้ยินเสียงตัวเอง

แผนฉบับเต็ม: [docs/plan.md](docs/plan.md) · เวอร์ชันล่าสุด **1.0.3** ([release notes](docs/releases/v1.0.3.md))

## ติดตั้ง (ผู้ใช้)

1. ดาวน์โหลด `HEARASIDE-Setup-1.0.5.exe` จาก [GitHub Releases](https://github.com/ArmZOfficial/hearaside/releases) หรือ [hearaside.netlify.app/download](https://hearaside.netlify.app/download)
2. ปิด DAW และ OBS แล้วรันตัวติดตั้ง ได้ครบในไฟล์เดียว: Hub, Track, App Audio (VST3 + VST2), HEARASIDE for OBS และ cloudflared (ลิงก์ผ่านเน็ต ไม่ต้องลงเพิ่ม)
3. ใน DAW: สแกนปลั๊กอินใหม่ → ใส่ **HEARASIDE Hub** บน Master และ **HEARASIDE Track** ท้ายทุกแทร็ก
4. ใน OBS: เพิ่ม Source ชื่อ **HEARASIDE**

> **อัปเดตทุกตัวพร้อมกัน** (ทุกเครื่อง ทุก DAW และปลั๊กอิน OBS) เวอร์ชันที่เปลี่ยนรูปแบบ shared memory (เช่น 1.0.3 = protocol v14) คุยกับตัวเก่าไม่ได้ ตัวเก่าจะขึ้นว่าไม่มี Hub

---

## ส่วนประกอบ

| ชิ้นส่วน | อยู่ที่ไหน | หน้าที่ |
|---|---|---|
| **HEARASIDE Track** | Insert ท้ายสุดของแต่ละแทร็กใน DAW | สวิตช์ "คุณได้ยิน" / "คนดูได้ยิน", ระดับในหูฟัง, ระดับฝั่งคนดู, หน่วงเวลาฝั่งคนดู ต่อแทร็ก |
| **HEARASIDE Hub** | Insert บน Master bus (ตัวเดียวต่อโปรเจกต์) | รับเสียงคนดูจาก Master (ผ่านปลั๊กอินเหนือ Hub) ส่ง OBS, สร้างเสียงหูฟังจากแต่ละแทร็ก, mixer กลาง (รีโมตทุกแทร็ก), limiter, LUFS, ปุ่มตัดเสียงคนดู, ฟังแบบคนดู, ระดับหูฟังรวม |
| **HEARASIDE OBS Source** | ปลั๊กอินใน OBS | อ่าน Stream Mix จาก shared memory, ชดเชย clock drift, แปลง sample rate |
| **HEARASIDE App Audio** (Windows) | Insert บนแทร็กไหนก็ได้ (Audio / Instrument / Bus) | ดึงเสียงจากโปรแกรมเดียว (YouTube, Spotify...) หรือทั้งเครื่องยกเว้น DAW เข้าแทร็ก, เปิด/ปิด + ระดับเสียง, อัดลง WAV (Print) แบบกดเองหรืออัดตาม DAW, ควบคุมจาก Hub ได้ |
| **เว็บไซต์ + บัญชี** (`web/site`) | [hearaside.netlify.app](https://hearaside.netlify.app) (Netlify/Vercel + Supabase) | หน้าแนะนำ ดาวน์โหลด คู่มือ, บัญชี (ไม่บังคับ), เข้าสู่ระบบในปลั๊กอินด้วยโค้ด, ลิงก์ถาวรของลิงก์ฟัง/ลิงก์เพื่อน (เสียงไม่ผ่านเว็บ) |
| *Bridge* (เสริม) | แอปบน tray | ส่ง Stream Mix ออก audio device อื่น (VB-CABLE ฯลฯ) — ยังไม่ได้ทำ |

---

## สถานะ (11 ต.ค. 2026, 1.0.3)

- **1.0.3 ห้องเพื่อนร้องด้วยกัน:** เพื่อนได้ยินกันเองแบบ Jamulus (ไม่ได้ยินเสียงตัวเอง), หน้าเว็บเพื่อนหน่วงน้อยลง, เพื่อนที่มิกซ์ในแทร็ก DAW ไม่สะท้อนกลับและไม่ช้ากว่าจังหวะ, ลิงก์ถาวรผ่านบัญชี, ซิงก์หน้าตาข้ามเครื่อง
- **1.0.2** ซิงก์เสียงร้องอัตโนมัติวัดได้ในโปรเจกต์จริง · **1.0.1** แก้บั๊ก UI · **1.0.0** UI ใหม่ทุกปลั๊กอิน, ห้องเพื่อน 8 คน, ตัวติดตั้งไฟล์เดียว
- รายละเอียดแต่ละรุ่น: [docs/releases](docs/releases) · audit UX และงานค้าง: [docs/ux-roadmap.md](docs/ux-roadmap.md)

### ✅ เสร็จแล้ว

| โฟลเดอร์ | เนื้อหา |
|---|---|
| `libs/ssbus` | Shared memory protocol (v14) บน Windows (security descriptor ให้ DAW/OBS ที่รันแบบ admin ใช้ร่วมกับโปรเซสปกติได้) และ macOS (POSIX shm), broadcast ring buffer แบบไม่ใช้ lock (writer ไม่เคยรอ reader), การจอง/คืน slot, กู้ slot ของโปรเซสที่ตาย, จัดการ UUID ซ้ำเมื่อ duplicate แทร็ก, mailbox คำสั่ง Hub → Track, ขอเปลี่ยนชื่อแทร็ก, timeline tag |
| `libs/ssdsp` | Smoother, equal-power pan, brick-wall limiter แบบ lookahead (รับประกันไม่เกิน ceiling), LUFS meter ตาม ITU-R BS.1770 (M / S), ตัวควบคุม drift แบบ PI, resampler ปรับ ratio ได้ละเอียด 5–15 ppm (speexdsp), `StreamConsumer` สำหรับฝั่ง OBS / Bridge |
| `libs/ssengine` | `TrackPublisher` และ `HubEngine` (sync แทร็กด้วย timeline tag จึงรองรับ ASIO-Guard / Anticipative FX, re-anchor เมื่อ seek / หยุด / เล่น, sync safety, stream delay สูงสุด 500 ms ที่อ่านย้อนจาก FIFO โดยไม่ใช้หน่วยความจำเพิ่ม, crossfade, solo, panic, stem 8 ช่อง, limiter, ฟังแบบคนดู) |
| `plugins/track` | **HEARASIDE Track** (VST3 + VST2): คุณได้ยิน / คนดูได้ยิน, **ระดับในหูฟัง** (ลดเสียงที่คุณได้ยินโดยคนดูยังได้ยินเท่าเดิม ไม่ต้องแตะ fader), ระดับฝั่งคนดู, **หน่วงเวลาฝั่งคนดู 0–500 ms** (เหมือน Sync Offset ของ OBS แต่ทำทีละแทร็ก เช่น ชดเชยปลั๊กอินร้องที่หน่วง), pan / stem / solo / ชื่อ / bus ในแผงตั้งค่าละเอียด, ชื่อ+สีแทร็กจาก DAW (VST3), รับคำสั่งจาก Hub ผ่าน host (undo / automation / save ได้) |
| `plugins/hub` | **HEARASIDE Hub** (VST3 + VST2): หน้าจอตาม design export (หน้าหลัก + หน้าย่อย Share and friends, Settings, Track fine settings, Auto-sync, Manage tracks, Manage program audio), ห้องเพื่อน 8 คน + Line up, ลิงก์แชร์, REST API, รีโมตทุกแทร็ก (ปุ่มได้ยิน/ไม่ได้ยิน + สไลเดอร์ระดับหูฟัง/คนดูในแต่ละแถว), **ระดับหูฟังรวม** (ไม่มีผลกับคนดูและตอน export), ตัดเสียงคนดู, ฟังแบบคนดู, กันเสียงพีค, LUFS, สถานะ OBS + ความหน่วงถึง OBS, ตั้งค่า bus / sync safety / ชื่อ stem |
| `plugins/common` | UI "Frosted Studio" แบบ native JUCE: กระจกขุ่น pre-render, ธีมสว่าง/มืด/ตามระบบ, ไทย/อังกฤษ, ฟอนต์ Anuphan ฝังในปลั๊กอิน, ขนาด 100–200 %, ลดการเคลื่อนไหว; design tokens จาก `design/tokens.json` → `Theme.h` |
| `obs/hearaside-obs` | **OBS Source** (OBS 32): เลือก bus / Stream Mix หรือ Stem 1–8 / buffer 15–60 ms, แสดงสถานะ (DAW rate, buffer, drift, underrun), ชดเชย drift + แปลง sample rate, ไม่พึ่ง DLL อื่นนอกจาก `obs.dll`; **hotkey** (ตัด/คืนเสียงคนดู, ฟังแบบคนดู ใช้กับ Stream Deck ได้) |
| `tools/` | `bus-inspector` (ดู bus แบบ real-time), `ui-snapshot` (เรนเดอร์หน้าจอปลั๊กอินเป็น PNG), `plugin-host-test` (โหลด VST3/VST2 ที่ build แล้วแบบ DAW แล้วตรวจ end-to-end) |
| `installer/windows` | `hearaside.iss` + `build-installer.ps1`: ตัวติดตั้ง Inno Setup ไฟล์เดียว (VST3, VST2, OBS, cloudflared ที่ตรวจ SHA-256) · `install.ps1` ติดตั้งจาก build โดยตรง (และ `-Uninstall`) |
| `web/site` | เว็บไซต์ React + Vite + Tailwind และ API Express (Vercel Function): บัญชี, device sign-in, `/me`, ลิงก์ถาวร (`/api/register`, `/api/resolve`, `/l/:token`, `/s/:token`) |
| `web/share-vercel` | เว็บลิงก์ถาวรแบบ deploy เอง (ไม่ใช้บัญชี) · ต้นฉบับของ `directory.js` ที่ `web/site` ใช้ร่วม (`npm run sync`) |

### ผลทดสอบล่าสุด (`.\build.ps1 -Test`, 1.0.3): 14/14 ผ่าน

- ✅ **unit** 68 ชุด รวมห้องเพื่อน: Line up ตรงจังหวะ, ไม่มีใครได้ยินเสียงตัวเอง, เพื่อนได้ยินกันทันที, เพื่อนผ่านแทร็ก DAW หน่วงครั้งเดียว, 8 เพื่อนพร้อมกันไม่เกิน limiter, ซิงก์อัตโนมัติ
- ✅ **ipc** ข้ามโปรเซส 242,944 frames ตรงทุก sample, คืน slot ของโปรเซสที่ถูก kill ได้
- ✅ **plugin_host_vst3 / plugin_host_vst2** โหลดไฟล์ปลั๊กอินจริงแบบ DAW: null test, ระดับในหูฟัง, คุณได้ยิน/คนดูได้ยิน, หน่วง, คำสั่งจาก Hub, save/restore state, คำสั่งจาก OBS
- ✅ **friends_room / friend_page / friend_app_source** ห้องเพื่อนฝั่ง Hub, หน้าเว็บเพื่อนในเบราว์เซอร์จริง, App Audio ที่รับเสียงเพื่อน
- ✅ **rest_api**, **share_directory**, **share_api**, **share_web_in_sync**, **ui_values**, **design_tokens_in_sync**, **design_contrast**
- ✅ เว็บ: `npm test` ใน `web/site` (39 ข้อ รวม API ลิงก์ถาวร) · `ui-snapshot --audit` ตัวตรวจ layout 0 ปัญหา

### ⏳ ยังไม่ได้ทำ

1. ร้องกับเพื่อนผ่านเน็ตใน DAW จริง และทดสอบตาม host matrix (Studio One 7 / Cubase 15 / Reaper / FL / Ableton) กับ OBS 32
2. Friend input: เลือก Track ท้ายช่องเอง (`Goes out through`) สำหรับ DAW ที่ไม่ส่งชื่อแทร็ก, รอนิ่ง 500 ms ก่อนสลับกลับทาง DAW
3. ซิงก์ชื่อ stem ผ่านบัญชี, pluginval strictness 10, Bridge app, build macOS

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
.\build.ps1 -Vst2Sdk C:\path\to\vstsdk2.4 -Installer   # + dist\HEARASIDE-Setup-<version>.exe (Inno Setup 6) และอัปเดต web\site\src\content\release.json
.\installer\windows\install.ps1  # (Administrator) ติดตั้ง VST3 / VST2 / OBS plugin จาก build โดยตรง
.\build\Release\tools\bus-inspector\bus-inspector.exe Main
```

### Latency

- **DAW:** Hub แสดงบัฟเฟอร์ของ DAW ที่หัวจอ (เช่น `DAW 256 · 5.3 ms`) และในกล่องสรุปแยกเป็น DAW + Hub + OBS = รวม; Track แสดงบัฟเฟอร์ DAW ที่ท้ายหน้าต่าง (latency ของ audio interface เองปลั๊กอินมองไม่เห็น)
- **OBS Source:** buffer ค่าเริ่มต้นเป็น **อัตโนมัติ** วัดจังหวะจริงของ DAW ทุก 2 วินาทีแล้วลดลงเท่าที่ปลอดภัย (เหลือ headroom ~3 ms ในจังหวะที่แย่ที่สุด) ถ้ามี underrun จะเพิ่มเอง 5 ms ทันที — ทดสอบที่ DAW buffer 256 ได้ ~11 ms (เดิมคงที่ 30 ms) อ่านทุก 5 ms และส่ง timestamp ที่ทำให้ OBS ไม่ต้องเพิ่ม audio buffering ของตัวเอง

### Mastering ให้คนดู (ใช้ปลั๊กอินใน DAW)

ใช้แค่ HEARASIDE Track (ท้ายทุกแทร็ก) + HEARASIDE Hub ตัวเดียว (บน Master):

```
Master:  Gullfoss ─► IMPusher ─► HEARASIDE Hub ─► (ปลั๊กอินสำหรับหูฟัง ถ้ามี)
```

- **ปลั๊กอินเหนือ Hub = เสียงคนดู** (ส่งเข้า OBS), **ใต้ Hub = หูฟังของคุณ**
- Track ส่ง "เสียงที่คนดูได้ยิน" เข้า DAW ดังนั้น Master และ fader ของแต่ละช่องคือเสียงคนดู ส่วนหูฟัง Hub ประกอบเองจากเสียงของแต่ละ Track (ตาม "คุณได้ยิน" และระดับในหูฟัง)
- ช่องที่ไม่มี HEARASIDE Track จะไปถึงคนดู แต่ไม่อยู่ในหูฟัง ใส่ Track ให้ครบทุกช่องที่อยากได้ยิน
- Export จาก Master = เสียงคนดูที่ผ่าน mastering แล้ว

### App Audio: ดึงเสียงจากโปรแกรม + Print

- เป็น **effect** ใส่ในแทร็ก Audio ธรรมดาได้เลย (เสียงโปรแกรมถูก *บวก* เข้ากับเสียงของแทร็ก ไม่ต้องใช้ Instrument track)
- เลือกโปรแกรม หรือ **เสียงทั้งเครื่อง (ยกเว้น DAW)** ซึ่งไม่ดึงเสียง DAW กลับเข้ามาเอง
- buffer ปรับเอง: 1 block ของ DAW + ~12 ms (ที่ 2048 sample ≈ 55 ms, ที่ 256 ≈ 17 ms) ถ้าจังหวะไม่นิ่งจะเพิ่มเอง แล้วค่อย ๆ ลดกลับ
- ตอน export แบบ offline จะไม่ใส่เสียงโปรแกรม (โปรแกรมเล่นเร็วตาม export ไม่ได้) ให้อัดเป็นแทร็กก่อน (ข้างล่าง)
- **อัดด้วยปุ่มอัดของ DAW**: DAW อัดเสียงขาเข้า (ก่อน insert ของแทร็ก) จึงต้องใส่ App Audio ที่ **ช่อง Input** แล้วเปิด **"เฉพาะเสียงโปรแกรม"** (พารามิเตอร์ `only` ตัดเสียงเดิมของช่อง ไม่มีไมค์ปน)
  - Studio One: Song › Song Setup › Audio I/O Setup › Inputs › Add (Stereo) ตั้งชื่อ `App Audio` → Console แสดง Inputs แล้วใส่ปลั๊กอินที่ช่อง App Audio → แทร็ก Stereo ตั้ง input = App Audio → กดอัด
  - Cubase: Input Channel · Reaper: Input FX ของแทร็ก
- มี **คุณได้ยิน / คนดูได้ยิน** + ระดับในหูฟัง / ระดับฝั่งคนดู เหมือน HEARASIDE Track (ไม่ต้องใส่ Track ต่อท้าย): App Audio ส่งเสียงโปรแกรมขึ้น bus เป็นช่องของตัวเอง (`kFlagApp`) ให้ Hub ใส่ในหูฟัง ส่วนเสียงที่ออก DAW คือฝั่งคนดู (บนช่อง Input ที่อัด: ปิด "คนดูได้ยิน" = DAW อัดได้ความเงียบ)
- VST3 ทุกตัวล้าง output silence flags เอง (`scripts/patches/juce-output-silence-flags.patch`): Studio One ส่ง flag "เงียบ" ของ input ที่เงียบมาให้ตอนแทร็กถูก arm แล้วทิ้งเสียงที่ปลั๊กอินสร้างเอง
- Print (อัดลง WAV เอง) ยังสั่งได้จากปุ่ม ... ของแถวใน Hub หัวข้อ "เสียงจากโปรแกรม" (เปิด/ปิด, อัด, ระดับ, เปลี่ยนโปรแกรม, อัดตาม DAW) ไฟล์อยู่ที่ `Documents\HEARASIDE\Recordings`

### ซิงค์เสียงร้องกับเพลงอัตโนมัติ (ฝั่งคนดู)

กด **ซิงค์เสียงร้องอัตโนมัติ** ใน Hub แล้วเอาหูฟังจ่อไมค์ภายใน 3 วินาที (เปิดเพลงไว้ อย่าร้อง) Hub จะวัดว่าเพลงถึง Master ทางตรง (App Audio หรือแทร็กเพลง) กับผ่านไมค์ห่างกันเท่าไร แล้วตั้งดีเลย์ให้เอง: เสียงร้องช้า → หน่วงเพลง (Sync Delay ของ App Audio / Viewers Delay ของแทร็ก), เสียงร้องเร็ว → หน่วงเสียงร้อง ระหว่างวัดคนดูไม่ได้ยินเสียง ไม่ต้องใส่ปลั๊กอินเพิ่มบนแทร็ก App Audio ปุ่ม ⋯ ข้าง ๆ ใช้เลือกแทร็กเอง กดใหม่เมื่อเพิ่ม/ถอดปลั๊กอินที่มีดีเลย์หรือเปลี่ยน buffer

### แชร์ลิงก์ (แบบ LISTENTO)

ปุ่ม 🔗 ใน Hub → เปิดแชร์ ได้ 2 ลิงก์:

- **ลิงก์ฟังสด** เปิดในเบราว์เซอร์/มือถือ ฟังเสียงที่คนดูได้ยิน (16-bit ไม่บีบอัด, เลือกความหน่วง เร็วสุด/สมดุล/ลื่นสุด)
- **ลิงก์เพื่อน** (ห้องเพื่อน ด้านล่าง) เพื่อนแต่ละคนมีลิงก์ส่งเสียงของตัวเอง อัดเสียงเพื่อนแยกคน: กดปุ่ม REC ในแถวของเพื่อนบน Hub ได้ทันที (ไฟล์ WAV จะเซฟใน Documents\HEARASIDE\Recordings พร้อมชดเชย delay ตรงกับเพลง) หรืออัดผ่าน App Audio ที่ช่อง Input โดยเลือกชื่อเพื่อน
- App Audio ยัง **รับลิงก์ฟังสดของ HEARASIDE เครื่องอื่น** ได้ (DAW ถึง DAW) จาก "รับจากลิงก์ของคนอื่น..."
- ผ่านเน็ต: ตัวติดตั้งใส่ cloudflared มาให้แล้ว (`C:\Program Files\HEARASIDE\cloudflared`) Hub ได้ลิงก์ https อัตโนมัติ ไม่ต้องสมัคร Cloudflare (ถ้า build เองให้ `winget install --id Cloudflare.cloudflared`) ถ้าไม่มี cloudflared จะได้ลิงก์ใน WiFi เดียวกัน (http: หน้าฟังใช้ตัวเล่นสำรองแทน AudioWorklet ซึ่งเบราว์เซอร์เปิดให้เฉพาะ https / localhost; การส่งไมค์จากเบราว์เซอร์ต้องใช้ลิงก์ https)
- ปุ่มเปิดลิงก์ฟังใน Hub เปิดผ่าน `http://127.0.0.1` (ฟังบนเครื่องนี้ ไม่ผ่าน tunnel)
- ไม่มีลิงก์ถาวร: เปิดแชร์แต่ละครั้งได้ลิงก์ฟังสุ่มใหม่ (ลิงก์ของไลฟ์ก่อนใช้ไม่ได้) และโดเมนของ cloudflared เปลี่ยนทุกครั้ง ครั้งแรก Windows อาจถามสิทธิ์ firewall ให้ DAW
- **ลิงก์ถาวร (เข้าสู่ระบบแล้ว ไม่ต้องตั้งอะไร)**: เข้าสู่ระบบบัญชี HEARASIDE ในปลั๊กอิน แล้ว Hub ใช้เว็บของบัญชี (`hearaside.netlify.app`) เป็นที่อยู่ลิงก์ถาวรให้เอง ทั้งลิงก์ฟังและลิงก์ส่งเสียงของเพื่อนแต่ละคน ลิงก์ขึ้นในหน้า Account › Links and friends และเพิกถอนจากเว็บได้ (Hub ออกลิงก์ใหม่ให้เองในรอบถัดไป) · ที่อยู่ที่ตั้งเองใน Settings ชนะเสมอ · ถ้าเว็บลงทะเบียนลิงก์ไม่ได้ 1 นาที Hub กลับไปใช้ลิงก์ที่เปลี่ยนทุกไลฟ์จนจบ session
- **ลิงก์ถาวร (ไม่มีบัญชี)**: deploy `web/share-vercel` ในบัญชี Vercel ของคุณ ([วิธีทำ](web/share-vercel/README.md)) แล้วใส่ที่อยู่ใน Hub › ตั้งค่า › "ที่อยู่เว็บแชร์" ลิงก์จะเป็น `https://<เว็บของคุณ>/l/<token>` คงที่ต่อโปรเจกต์ ส่งครั้งเดียวใช้ได้ทุกไลฟ์ (Hub บอกเว็บว่า tunnel อยู่ไหนทุก 30 วิ เสียงยังวิ่งตรงจาก Hub)
- หน้าเว็บที่ไม่ได้มาจาก Hub หรือเว็บแชร์ที่ตั้งไว้จะต่อ WebSocket ไม่ได้ (Hub ตรวจ `Origin`)

### ห้องเพื่อน (รับเพื่อนได้สูงสุด 8 คนพร้อมกัน)

- **รับเพื่อนส่งเสียงเข้าพร้อมกันได้สูงสุด 8 คน**: แต่ละคนมีลิงก์ของตัวเอง (`/s/<token>`) จัดการในกลุ่ม Friends ของ Hub
- **วัดความหน่วงอัตโนมัติ**: timestamp ไทม์ไลน์ใน packet ทำให้ Hub รู้ความหน่วง $d_i$ ของเพื่อนแต่ละคนอย่างต่อเนื่อง ไม่ต้องรอกดวัด
- **หน่วงไลฟ์ให้ทุกคนร้องตรงจังหวะ (Line up friends for viewers)**: Hub หน่วง Stream Mix ด้วย $D = \max(d_i + L_i)$ และวางเสียงเพื่อนแต่ละคนด้วยความหน่วง $D - d_i - L_i$ เพื่อให้คนดูได้ยินทุกคนตรงจังหวะกับเพลงพอดี หูฟังของคุณไม่หน่วง และ listen stream ที่ส่งให้เพื่อนไม่หน่วง
- **เพื่อนได้ยินกันเอง แบบ Jamulus (mix-minus)**: เพื่อนแต่ละคนได้ยิน เพลง + เสียงคุณ + เพื่อนทุกคน ยกเว้นตัวเอง เสียงเพื่อนส่งต่อทันทีที่มาถึง Hub (ไม่รอ Line up) เพื่อนที่มิกซ์ในแทร็ก DAW ได้ยินเป็นเสียงที่ผ่านปลั๊กอินแล้ว · ความหน่วงระหว่างเพื่อน A ได้ยิน B ≈ เน็ตขาขึ้นของ B + jitter buffer ของ Hub + เน็ตขาลงของ A (เหมือน Jamulus) · หน้าเว็บเพื่อนส่งไมค์ทีละ 10 ms และ buffer ฝั่งฟังปรับเองตามเน็ต (เริ่ม 60 ms ขยายเมื่อสะดุด หดคืนเมื่อเน็ตนิ่ง) · buffer ของ DAW ใหญ่ = ส่งเป็นก้อนใหญ่ ถ้าต้องการหน่วงต่ำสุดให้ลด buffer ของ DAW (เช่น 256) · ให้เพื่อนใส่หูฟัง

### มิกซ์เสียงเพื่อนด้วยปลั๊กอินใน DAW (ห้องเพื่อน + HEARASIDE Track)

คุณสามารถดึงเสียงเพื่อนที่ส่งเข้ามา เข้าไปวิ่งผ่านปลั๊กอินใน DAW (เช่น EQ, Compressor, Auto-Tune, Reverb) ได้ โดยใช้ **HEARASIDE Track 2 ตัว** บนแทร็ก Audio ว่างใน DAW:

```
ช่อง "Mint vox" ใน DAW (แทร็ก audio ว่าง ๆ):
  [HEARASIDE Track · Source = Mint]  ← วางลิงก์/เลือกเพื่อนตรงนี้ (Friend input)
  [EQ] [Compressor] [Reverb] ...     ← ปลั๊กอินของคุณ
  [HEARASIDE Track]                  ← ตัวท้ายช่องตามปกติ (คุมคุณได้ยิน/คนดูได้ยิน/ระดับ/แพน)
```

**ทำไมต้องใส่ 2 ตัว?**
- ปลั๊กอินใน DAW ประมวลผลเรียงจากบนลงล่าง
- **ตัวบน (Friend input):** ทำหน้าที่เป็นตัวป้อนเสียง (Feeder) ดึงเสียงเพื่อนจาก ring buffer มาลงช่องแทร็กของ DAW *ก่อน* ถึงปลั๊กอินของคุณ
- **ตัวท้ายช่อง (Track ปกติ):** รับเสียงที่ผ่านเอฟเฟกต์ของคุณแล้ว ส่งขึ้น bus ให้ Hub เพื่อวัดความหน่วงของปลั๊กอิน ($L_i$) และคำนวณการหน่วง Line up ให้ตรงจังหวะกับเพลงสำหรับคนดู พร้อมทั้งคุม You hear / Viewers hear / ระดับเสียง / แพน

**ความปลอดภัยด้านเสียง (Audio Safety):**
- **ไม่ดังซ้ำสองทางเด็ดขาด:** เพื่อนย้ายจาก Hub ไปทาง DAW ก็ต่อเมื่อมี HEARASIDE Track ท้ายช่องที่จับคู่แล้วเท่านั้น ระหว่างยังไม่จับคู่ ตัวบน (Friend input) ปล่อยความเงียบ และ Hub เล่นเพื่อนทางตรงตามเดิม เมื่อแทร็กใน DAW หยุด Hub ดึงเพื่อนกลับมาทางตรงให้อัตโนมัติพร้อม crossfade
- **ทางออกของเพื่อนที่มิกซ์ใน DAW:** Track ท้ายช่องที่จับคู่แล้วส่งฝั่งคนดูขึ้น bus ให้ Hub เสมอ (ไม่ออก master ของ DAW) Hub ผสมหลังดีเลย์ Line up และก่อน Peak protection ดังนั้นเพื่อนไม่ได้ยินตัวเอง และไม่ถูกหน่วงซ้ำ · ข้อแลก: ปลั๊กอิน mastering บน master เหนือ Hub ไม่ได้ประมวลผลเสียงเพื่อน (ใส่เอฟเฟกต์ที่ช่องของเพื่อนแทน)
- **ระดับเสียงเข้าแทร็ก + เก็บเสียงเดิม:** ตัวบนมีแถบ `<ชื่อ>’s level into this track` (−∞..+12 dB, สำหรับ gain staging ก่อน compressor) และสวิตช์ `Keep this track’s own sound` (ปิด = ส่งเฉพาะเสียงเพื่อนต่อไปที่ปลั๊กอิน)
- **ปุ่มตัดเสียง (Mute / Panic):** ปุ่ม Mute ใน Hub และ panic ตัดเสียงเพื่อนทันทีทั้งสองทาง (ทั้งทางตรงและทาง DAW) ภายใน 1 block
- **DAW Suspended / Paused:** หาก DAW พักการประมวลผลของช่อง (เช่น Cubase VST3 silence suspension) ปลั๊กอินจะรายงานสถานะเตือนบนหน้าจอ พร้อม tail length ตลอดชีพเพื่อป้องกัน DAW ตัดการประมวลผล

### บัญชีและเว็บไซต์ (ไม่บังคับ)

ทุกอย่างด้านเสียงใช้ได้โดยไม่มีบัญชีและไม่มีเน็ต บัญชีเป็นของเสริม:

- **เข้าสู่ระบบในปลั๊กอิน** (Hub › Settings › Account, Track/App Audio › Account): กด *Sign in with your browser* → ได้โค้ด 8 ตัว → อนุญาตที่ `hearaside.netlify.app/link` ไม่มีการพิมพ์รหัสผ่านในปลั๊กอิน · เข้าที่ตัวไหน ปลั๊กอินทุกตัวในเครื่องเข้าด้วย (token เก็บใน `%APPDATA%\HEARASIDE\account.dat` เข้ารหัส DPAPI ไม่อยู่ในไฟล์โปรเจกต์)
- **ได้อะไร:** ลิงก์ถาวรโดยไม่ต้องมีเว็บของตัวเอง, ชื่อ/รูปที่เพื่อนเห็น, **Sync appearance** (ภาษา ธีม ขนาด ความทึบกระจก สีแทร็ก ลดการเคลื่อนไหว ตามไปทุกเครื่อง ค่าใหม่กว่าชนะ ไม่ซิงก์ค่าที่กระทบเสียง), รายการอุปกรณ์ + ออกจากระบบจากเว็บ
- **deploy เว็บ** (`web/site` บน Netlify หรือ Vercel, ค่าตัวอย่างใน `web/site/.env.example`): Supabase (บัญชี, รูป), `PLUGIN_TOKEN_SECRET`, และ **`UPSTASH_REDIS_REST_URL` / `UPSTASH_REDIS_REST_TOKEN`** สำหรับลิงก์ถาวรและ rate limit · dev ในเครื่อง: `npm run dev` ใน `web/site` (`STORE=memory` ไม่ต้องมี Supabase)
- **เว็บ deploy จาก branch `production` เท่านั้น**: `netlify.toml` (`ignore`) และ `web/site/vercel.json` (`git.deploymentEnabled`) ข้ามการ build ของ `main`, branch อื่น และ pull request ทั้งหมด ปล่อยเว็บด้วยการ merge `main` เข้า `production` (`git push origin main:production`) และต้องตั้ง Production branch เป็น `production` ใน Netlify / Vercel

### REST API

Hub › ตั้งค่า › **REST API** แล้วคัดลอก API key: `http://127.0.0.1:47800/api/v1` (เฉพาะโปรแกรมในเครื่องนี้) อ่านสถานะ ตัดเสียงคนดู ฟังแบบคนดู ปรับระดับ และสลับคุณได้ยิน/คนดูได้ยินของแต่ละแทร็ก ทุกคำสั่งผ่าน host จึง undo ได้ — รายละเอียดใน [docs/rest-api.md](docs/rest-api.md)

### Hotkey ใน OBS

ตั้งได้ที่ **Settings → Hotkeys → HEARASIDE** (ตัด/คืนเสียงคนดู, ฟังแบบคนดู) ต้องมี Source **HEARASIDE** อย่างน้อย 1 ตัว

ใน OBS: เพิ่ม Source ชื่อ **HEARASIDE** แล้วตั้ง Audio Monitoring เป็น "Monitor Off" (ฟังจาก DAW อยู่แล้ว) และถ้า DAW เล่นผ่าน Desktop Audio ให้ปิด Desktop Audio ไม่งั้นคนดูได้ยินซ้ำ

### ออกเวอร์ชันใหม่

1. เปลี่ยนเลขใน `CMakeLists.txt` (`project(... VERSION x.y.z)`) และ `installer/windows/hearaside.iss` (`AppVersion`)
2. `.\build.ps1 -Vst2Sdk <sdk> -Installer -Test` แล้วคัดลอก `dist\HEARASIDE-Setup-x.y.z.exe` ไป `web/site/public/downloads/` และเพิ่มบรรทัดใน `notes` ของ `release.json`
3. เขียน `docs/releases/vx.y.z.md` (อังกฤษ + ไทย) แล้ว push: workflow สร้าง GitHub pre-release ให้เอง จากนั้น `gh release upload vx.y.z dist\HEARASIDE-Setup-x.y.z.exe`

## Dependencies และ License

โค้ดของ HEARASIDE ทั้งหมดเป็น **MIT**, Copyright (c) 2026 ArmZ (ดู `LICENSE`) ไบนารีที่ build แล้วต้องทำตาม license ของสิ่งที่รวมอยู่ด้วยตามตารางนี้ (เช่น ปลั๊กอินที่ใช้ JUCE, OBS plugin ที่ link libobs เป็น GPL)

| | License |
|---|---|
| โค้ด HEARASIDE (`libs/*`, `plugins`, `apps`, `obs`, `rust`, `tools`, `web`) | MIT |
| JUCE 8 | AGPLv3 หรือ commercial |
| VST3 SDK | MIT (ตั้งแต่ 3.8) |
| OBS / libobs | GPLv2 (OBS plugin ต้องเปิด source) |
| speexdsp | BSD |
| Anuphan (`external/fonts`) | SIL OFL 1.1 |
| cloudflared (แนบในตัวติดตั้ง) | Apache-2.0 |
