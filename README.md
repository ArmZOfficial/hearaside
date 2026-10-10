# HEARASIDE

ปลั๊กอิน VST3 (และ VST2 ถ้ามี license) สำหรับสตรีมเมอร์สายดนตรี แยก **"เสียงที่คุณได้ยิน"** ในหูฟัง ออกจาก **"เสียงที่คนดูได้ยิน"** แล้วส่งเสียงฝั่งคนดูเข้า OBS โดยตรง ไม่ต้องใช้ virtual cable

> ตัวอย่างเคสหลัก: แทร็กร้องตั้ง `คุณได้ยิน = ปิด`, `คนดูได้ยิน = เปิด` คนดูได้ยินเสียงร้องที่ผ่าน EQ / Comp / Reverb แล้ว แต่ในหูฟังคุณไม่ได้ยินเสียงตัวเอง

แผนฉบับเต็ม: [docs/plan.md](docs/plan.md)

---

## ส่วนประกอบ

| ชิ้นส่วน | อยู่ที่ไหน | หน้าที่ |
|---|---|---|
| **HEARASIDE Track** | Insert ท้ายสุดของแต่ละแทร็กใน DAW | สวิตช์ "คุณได้ยิน" / "คนดูได้ยิน", ระดับในหูฟัง, ระดับฝั่งคนดู, หน่วงเวลาฝั่งคนดู ต่อแทร็ก |
| **HEARASIDE Hub** | Insert บน Master bus (ตัวเดียวต่อโปรเจกต์) | รับเสียงคนดูจาก Master (ผ่านปลั๊กอินเหนือ Hub) ส่ง OBS, สร้างเสียงหูฟังจากแต่ละแทร็ก, mixer กลาง (รีโมตทุกแทร็ก), limiter, LUFS, ปุ่มตัดเสียงคนดู, ฟังแบบคนดู, ระดับหูฟังรวม |
| **HEARASIDE OBS Source** | ปลั๊กอินใน OBS | อ่าน Stream Mix จาก shared memory, ชดเชย clock drift, แปลง sample rate |
| **HEARASIDE App Audio** (Windows) | Insert บนแทร็กไหนก็ได้ (Audio / Instrument / Bus) | ดึงเสียงจากโปรแกรมเดียว (YouTube, Spotify...) หรือทั้งเครื่องยกเว้น DAW เข้าแทร็ก, เปิด/ปิด + ระดับเสียง, อัดลง WAV (Print) แบบกดเองหรืออัดตาม DAW, ควบคุมจาก Hub ได้ |
| *Bridge* (เสริม) | แอปบน tray | ส่ง Stream Mix ออก audio device อื่น (VB-CABLE ฯลฯ) |

---

## สถานะ (9 ต.ค. 2026)

> รอบ UX + automation (9 ต.ค.): audit และแผนเต็มอยู่ที่ [docs/ux-roadmap.md](docs/ux-roadmap.md)
> - **UI responsive**: Hub แบบ Compact / Regular / Wide (ขั้นต่ำ 420×460 จากเดิม 880×750), Track และ App Audio ย่อขยายได้, จำขนาดหน้าต่าง, เปิดครั้งแรกเลือกขนาด UI ให้พอดีจอ
> - **ใช้ง่ายขึ้น**: เช็คลิสต์เริ่มใช้งานที่ติ๊กเอง, ปุ่ม ✓ **ตรวจระบบ** (บอกผลกับคุณ / ผลกับคนดู / วิธีแก้) + คัดลอกรายงานปัญหา, เตือน "คนดูไม่ได้ยินอะไรเลย", ขั้นตอนเฉพาะ DAW ที่ใช้อยู่, การ์ดสรุปบอก "คนดูได้ยิน แต่คุณไม่ได้ยิน"
> - **ลิงก์แชร์ถาวร** ผ่านเว็บ Vercel ของคุณเอง ([web/share-vercel](web/share-vercel/README.md)) เสียงไม่ผ่าน Vercel
> - **REST API** ในเครื่อง สำหรับ Stream Deck / สคริปต์ ([docs/rest-api.md](docs/rest-api.md))
> - OBS Source ปิด Audio Monitoring ให้เองเมื่อสร้าง และเตือนถ้าเปิดอยู่

### ✅ เสร็จแล้ว

| โฟลเดอร์ | เนื้อหา |
|---|---|
| `libs/ssbus` | Shared memory protocol v1 บน Windows (security descriptor ให้ DAW/OBS ที่รันแบบ admin ใช้ร่วมกับโปรเซสปกติได้) และ macOS (POSIX shm), broadcast ring buffer แบบไม่ใช้ lock (writer ไม่เคยรอ reader), การจอง/คืน slot, กู้ slot ของโปรเซสที่ตาย, จัดการ UUID ซ้ำเมื่อ duplicate แทร็ก, mailbox คำสั่ง Hub → Track, ขอเปลี่ยนชื่อแทร็ก, timeline tag |
| `libs/ssdsp` | Smoother, equal-power pan, brick-wall limiter แบบ lookahead (รับประกันไม่เกิน ceiling), LUFS meter ตาม ITU-R BS.1770 (M / S), ตัวควบคุม drift แบบ PI, resampler ปรับ ratio ได้ละเอียด 5–15 ppm (speexdsp), `StreamConsumer` สำหรับฝั่ง OBS / Bridge |
| `libs/ssengine` | `TrackPublisher` และ `HubEngine` (sync แทร็กด้วย timeline tag จึงรองรับ ASIO-Guard / Anticipative FX, re-anchor เมื่อ seek / หยุด / เล่น, sync safety, stream delay สูงสุด 500 ms ที่อ่านย้อนจาก FIFO โดยไม่ใช้หน่วยความจำเพิ่ม, crossfade, solo, panic, stem 8 ช่อง, limiter, ฟังแบบคนดู) |
| `plugins/track` | **HEARASIDE Track** (VST3 + VST2): คุณได้ยิน / คนดูได้ยิน, **ระดับในหูฟัง** (ลดเสียงที่คุณได้ยินโดยคนดูยังได้ยินเท่าเดิม ไม่ต้องแตะ fader), ระดับฝั่งคนดู, **หน่วงเวลาฝั่งคนดู 0–500 ms** (เหมือน Sync Offset ของ OBS แต่ทำทีละแทร็ก เช่น ชดเชยปลั๊กอินร้องที่หน่วง), pan / stem / solo / ชื่อ / bus ในแผงตั้งค่าละเอียด, ชื่อ+สีแทร็กจาก DAW (VST3), รับคำสั่งจาก Hub ผ่าน host (undo / automation / save ได้) |
| `plugins/hub` | **HEARASIDE Hub** (VST3 + VST2): หน้าจอตาม mock-up (`StreamSplit Plugin UI.html`), รีโมตทุกแทร็ก (ปุ่มได้ยิน/ไม่ได้ยิน + สไลเดอร์ระดับหูฟัง/คนดูในแต่ละแถว), **ระดับหูฟังรวม** (ไม่มีผลกับคนดูและตอน export), ตัดเสียงคนดู, ฟังแบบคนดู, กันเสียงพีค, LUFS, สถานะ OBS + ความหน่วงถึง OBS, ตั้งค่า bus / sync safety / ชื่อ stem |
| `plugins/common` | UI "Frosted Studio" แบบ native JUCE: กระจกขุ่น pre-render, ธีมสว่าง/มืด/ตามระบบ, ไทย/อังกฤษ, ฟอนต์ Anuphan ฝังในปลั๊กอิน, ขนาด 100–200 %, ลดการเคลื่อนไหว; design tokens จาก `design/tokens.json` → `Theme.h` |
| `obs/hearaside-obs` | **OBS Source** (OBS 32): เลือก bus / Stream Mix หรือ Stem 1–8 / buffer 15–60 ms, แสดงสถานะ (DAW rate, buffer, drift, underrun), ชดเชย drift + แปลง sample rate, ไม่พึ่ง DLL อื่นนอกจาก `obs.dll`; **hotkey** (ตัด/คืนเสียงคนดู, ฟังแบบคนดู ใช้กับ Stream Deck ได้) |
| `tools/` | `bus-inspector` (ดู bus แบบ real-time), `ui-snapshot` (เรนเดอร์หน้าจอปลั๊กอินเป็น PNG), `plugin-host-test` (โหลด VST3/VST2 ที่ build แล้วแบบ DAW แล้วตรวจ end-to-end) |
| `installer/windows/install.ps1` | ติดตั้ง VST3 / VST2 / OBS plugin ลงโฟลเดอร์มาตรฐาน (และ `-Uninstall`) |

### ผลทดสอบล่าสุด (`.\build.ps1 -Test`): 10/10 ผ่าน

- ✅ **unit** 40 ชุด (ผ่านทั้ง Release และ AddressSanitizer) รวม e2e Hub → OBS consumer 44.1k → 48k + drift 150 ppm ไม่มี underrun / เสียงคลิก
- ✅ **ipc** ข้ามโปรเซส 242,944 frames ตรงทุก sample, คืน slot ของโปรเซสที่ถูก kill ได้
- ✅ **plugin_host_vst3 / plugin_host_vst2** โหลดไฟล์ปลั๊กอินจริงแบบ DAW: null test (เหลือ −240 dBFS), ระดับในหูฟัง −20 dB ลดเฉพาะเสียงออก DAW (คนดูยังได้ 100 %), ปิด "คุณได้ยิน" แล้วคนดูยังได้ยิน, หน่วง 40 ms ตรงกับสำเนาที่ช้า 1920 sample พอดี, คำสั่งจาก Hub เปลี่ยนพารามิเตอร์ที่ host เห็น, save/restore state
- ✅ host test ส่วนคำสั่งจาก OBS: ตัดเสียงคนดู → stream เงียบ, ระดับรวม −6 dB + ระดับแทร็ก −6 dB ถึง Stream Mix ตรงค่า
- ✅ **design_tokens_in_sync** + **design_contrast** (ข้อความทุกสีบนทุกพื้น ≥ 4.5:1 ทั้งสองธีม)
- ✅ **rest_api** 12 ข้อ (key, Origin, Host ปลอม, state, คำสั่งถึง Track ผ่านพารามิเตอร์ของ host)
- ✅ **share_directory** (ลิงก์ถาวรฝั่ง Hub กับเว็บจำลอง: register / heartbeat / retry / unregister / ปิด DAW ไม่รอเน็ต) + **share_api** (node:test 10 ข้อ) + **share_web_in_sync**
- ✅ `ui-snapshot --audit`: 81 ภาพ (Hub 8 ขนาด × ไทย/อังกฤษ × สว่าง/มืด, Track/App, แผงย่อย, สถานะต่าง ๆ) ตัวตรวจ layout 0 ปัญหา

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
- **ลิงก์ส่งเสียงเข้า** เพื่อนเปิดแล้วส่งไมค์เข้ามา → ใส่ App Audio แล้วเลือก "เสียงที่เพื่อนส่งเข้ามา"
- App Audio ยัง **รับลิงก์ฟังสดของ HEARASIDE เครื่องอื่น** ได้ (DAW ถึง DAW) จาก "รับจากลิงก์ของคนอื่น..."
- ผ่านเน็ต: ติดตั้ง cloudflared (ฟรี ไม่ต้องสมัคร) `winget install --id Cloudflare.cloudflared` แล้วปิด/เปิดแชร์ใหม่ Hub จะได้ลิงก์ https อัตโนมัติ ถ้าไม่มีจะได้ลิงก์ใน WiFi เดียวกัน (http: หน้าฟังใช้ตัวเล่นสำรองแทน AudioWorklet ซึ่งเบราว์เซอร์เปิดให้เฉพาะ https / localhost; การส่งไมค์จากเบราว์เซอร์ต้องใช้ลิงก์ https)
- ปุ่มเปิดลิงก์ฟังใน Hub เปิดผ่าน `http://127.0.0.1` (ฟังบนเครื่องนี้ ไม่ผ่าน tunnel)
- ลิงก์ผูกกับโปรเจกต์ (เปิดโปรเจกต์ใหม่ได้ลิงก์เดิม ยกเว้นส่วนโดเมนของ cloudflared ที่เปลี่ยนทุกครั้ง) ครั้งแรก Windows อาจถามสิทธิ์ firewall ให้ DAW
- **ลิงก์ถาวร**: deploy `web/share-vercel` ในบัญชี Vercel ของคุณ ([วิธีทำ](web/share-vercel/README.md)) แล้วใส่ที่อยู่ใน Hub › ตั้งค่า › "ที่อยู่เว็บแชร์" ลิงก์จะเป็น `https://<เว็บของคุณ>/l/<token>` คงที่ต่อโปรเจกต์ ส่งครั้งเดียวใช้ได้ทุกไลฟ์ (Hub บอกเว็บว่า tunnel อยู่ไหนทุก 30 วิ เสียงยังวิ่งตรงจาก Hub)
- หน้าเว็บที่ไม่ได้มาจาก Hub หรือเว็บแชร์ที่ตั้งไว้จะต่อ WebSocket ไม่ได้ (Hub ตรวจ `Origin`)

### ห้องเพื่อน (รับเพื่อนได้สูงสุด 8 คนพร้อมกัน)

- **รับเพื่อนส่งเสียงเข้าพร้อมกันได้สูงสุด 8 คน**: แต่ละคนมีลิงก์ของตัวเอง (`/s/<token>`) จัดการในกลุ่ม Friends ของ Hub
- **วัดความหน่วงอัตโนมัติ**: timestamp ไทม์ไลน์ใน packet ทำให้ Hub รู้ความหน่วง $d_i$ ของเพื่อนแต่ละคนอย่างต่อเนื่อง ไม่ต้องรอกดวัด
- **หน่วงไลฟ์ให้ทุกคนร้องตรงจังหวะ (Line up friends for viewers)**: Hub หน่วง Stream Mix ด้วย $D = \max(d_i + L_i)$ และวางเสียงเพื่อนแต่ละคนด้วยความหน่วง $D - d_i - L_i$ เพื่อให้คนดูได้ยินทุกคนตรงจังหวะกับเพลงพอดี หูฟังของคุณไม่หน่วง และ listen stream ที่ส่งให้เพื่อนไม่หน่วง

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
- **ไม่ดังซ้ำสองทางเด็ดขาด:** เมื่อเพื่อนถูกเลือกใน HEARASIDE Track เสียงทางตรงจาก Hub จะถูกปิดทันที และเมื่อแทร็กใน DAW หยุดหรือไม่มี Track ท้ายช่อง Hub จะดึงเพื่อนกลับมาทางตรงให้อัตโนมัติพร้อม crossfade นุ่มนวล
- **ปุ่มตัดเสียง (Mute / Panic):** ปุ่ม Mute ใน Hub และ panic ตัดเสียงเพื่อนทันทีทั้งสองทาง (ทั้งทางตรงและทาง DAW) ภายใน 1 block
- **DAW Suspended / Paused:** หาก DAW พักการประมวลผลของช่อง (เช่น Cubase VST3 silence suspension) ปลั๊กอินจะรายงานสถานะเตือนบนหน้าจอ พร้อม tail length ตลอดชีพเพื่อป้องกัน DAW ตัดการประมวลผล

### REST API

Hub › ตั้งค่า › **REST API** แล้วคัดลอก API key: `http://127.0.0.1:47800/api/v1` (เฉพาะโปรแกรมในเครื่องนี้) อ่านสถานะ ตัดเสียงคนดู ฟังแบบคนดู ปรับระดับ และสลับคุณได้ยิน/คนดูได้ยินของแต่ละแทร็ก ทุกคำสั่งผ่าน host จึง undo ได้ — รายละเอียดใน [docs/rest-api.md](docs/rest-api.md)

### Hotkey ใน OBS

ตั้งได้ที่ **Settings → Hotkeys → HEARASIDE** (ตัด/คืนเสียงคนดู, ฟังแบบคนดู) ต้องมี Source **HEARASIDE** อย่างน้อย 1 ตัว

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
