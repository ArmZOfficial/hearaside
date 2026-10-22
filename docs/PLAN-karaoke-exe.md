# HAS Karaoke (.exe): แผนสร้างโปรแกรมร้องคาราโอเกะบน Windows

> เขียนโดย Opus 5.5 (high) · 18 ต.ค. 2026 · คู่กับ [PLAN-v2-realtime-karaoke.md](PLAN-v2-realtime-karaoke.md) (ใช้แกนเสียง / Opus / UDP ชุดเดียวกัน ห้ามเขียนซ้ำ)
> ลำดับการทำงานเหมือนแผนหลัก: **Opus 5.5 high วางแผน → Sonnet 5.5 high ลงมือ → Opus 5.5 high รีวิว** (PLAN-v2 หัวข้อ 9) · โหมด ponytail full

---

## 0. สรุปสั้น

### 0.1 ทำไมต้องมี .exe (เทียบกับหน้าเว็บเพื่อน `send.html`)

| เรื่อง | หน้าเว็บตอนนี้ | โปรแกรม .exe |
|---|---|---|
| ความหน่วงของเสียงในเครื่อง (ไมค์ + หูฟัง) | 20–60 ms (AudioWorklet + ไดรเวอร์ทั่วไป) | 3–10 ms (ASIO / WASAPI exclusive ที่ buffer 64–128) |
| **ได้ยินเสียงตัวเองตอนร้อง** | ไม่ได้ยินเลย (mix-minus) ถ้าเปิดเองจะหน่วงจนร้องยาก | **ได้ยินทันทีในเครื่อง + reverb/echo แบบคาราโอเกะ** ไม่ผ่านเน็ต จึงไม่ซ้อน |
| เลือกไมค์ / interface / ช่อง input | จำกัด (เบราว์เซอร์ตัดสินใจเอง) | เลือกได้ทุกอย่าง, ใช้ ASIO ของ audio interface ได้ |
| ตัดเสียงสะท้อน / ตรวจหูฟัง | ขึ้นกับเบราว์เซอร์ | ควบคุมเองทั้งหมด (speexdsp AEC) |
| เปิดติดตลอด, tray, hotkey ปิดไมค์ | ไม่ได้ | ได้ |
| ติดตั้ง | ไม่ต้อง | ต้องติดตั้ง (คนใช้มือถือยังใช้หน้าเว็บ) |

สรุป: **.exe ไม่แทนหน้าเว็บ** แต่เป็นทางเลือกที่ดีที่สุดสำหรับเพื่อนที่ร้องจากคอม และสำหรับคนที่อยากเปิดห้องคาราโอเกะโดยไม่มี DAW

### 0.2 โปรแกรมเดียว 2 โหมด

1. **เข้าห้อง (Join)**: เพื่อนวางลิงก์ส่งเสียง `/s/<token>` ที่ได้จาก Hub แล้วร้องได้ทันที ต่อกับ Hub ตัวปัจจุบันได้ **โดยไม่ต้องแก้ Hub** (ใช้ WebSocket + HRA2 แบบเดียวกับหน้าเว็บ)
2. **เปิดห้อง (Host)**: เปิดห้องคาราโอเกะโดยไม่ต้องมี DAW: เพลงจากไฟล์หรือจากโปรแกรมอื่น (YouTube ฯลฯ ด้วย App Audio capture ที่มีอยู่แล้ว), ไมค์พร้อมเอฟเฟกต์ร้องในตัว, เพื่อนเข้าด้วยลิงก์, ส่งเข้า OBS ผ่าน HEARASIDE OBS Source ตัวเดิม

### 0.3 คำแนะนำลำดับ

1. **K0 → K1** ก่อน: โหมดเข้าห้อง ใช้ได้กับ Hub 1.0.4 ทันที ได้ประโยชน์เร็วที่สุด (ได้ยินตัวเอง + latency ต่ำในเครื่อง)
2. **K2** คุณภาพ (Opus, jitter buffer ใหม่, AEC, เช็กก่อนร้อง) ทำหลัง PLAN-v2 Phase 1–2 เพราะใช้โค้ดชุดเดียวกัน
3. **K3** โหมดเปิดห้อง
4. **K4** ตัวติดตั้ง + อัปเดต · **K5** UDP (ตาม PLAN-v2 Phase 4) · **K6** ของเสริม (เนื้อเพลง, เปลี่ยนคีย์, คะแนน) ตามที่เลือก

### 0.4 สถานะ

| Phase | ชื่อ | ขึ้นกับ | สถานะ |
|---|---|---|---|
| K0 | โครงโปรเจกต์ + แยก UI/Share เป็น library | — | ✅ |
| K1 | โหมดเข้าห้อง MVP (PCM, ต่อ Hub เดิมได้) | K0 | ✅ |
| K2 | คุณภาพเสียง: Opus, jitter buffer, AEC, เช็กก่อนร้อง | K1, PLAN-v2 Phase 1–3 | ⏳ |
| K3 | โหมดเปิดห้อง (ไม่ต้องมี DAW) | K1 | ⏳ |
| K4 | ตัวติดตั้ง, อัปเดต, เซ็นรหัส | K1 | ⏳ |
| K5 | UDP (WebRTC DataChannel) | PLAN-v2 Phase 4 | ⏳ |
| K6 | ของเสริมคาราโอเกะ (เลือกได้) | K3 | 💤 |

---

## 1. เป้าหมายวัดได้

| ตัวชี้วัด | เป้า |
|---|---|
| ไมค์ → หูฟังตัวเอง (ในเครื่อง, ASIO 128 @ 48 k) | ≤ 10 ms |
| ไมค์ → หูฟังตัวเอง (WASAPI shared) | ≤ 30 ms (ถ้าเกินบอกผู้ใช้ให้ลอง exclusive/ASIO) |
| เสียงเพื่อนถึง Hub (one-way, เน็ตในประเทศ) | ต่ำกว่าหน้าเว็บ ≥ 15 ms ในการวัดเดียวกัน |
| เข้าห้องครั้งแรก (ติดตั้งแล้ว) | วางลิงก์ + กด 1 ปุ่ม |
| CPU (โหมดเข้าห้อง, Opus) | ≤ 3 % ของ 1 core |
| ขนาดตัวติดตั้งเพิ่ม | ≤ 15 MB |
| ไม่มีเสียงตัวเองย้อนจากเน็ต | ผ่าน test "mix-minus" เดิม + test ใหม่ของ client |

---

## 2. สถาปัตยกรรม

```
 ┌──────────────── HAS Karaoke.exe (JUCE GUI app) ───────────────────────┐
 │ AudioDeviceManager (ASIO / WASAPI / DirectSound)                       │
 │   mic ─► [Vocal chain: gate · EQ · comp · reverb/echo] ─┬─► หูฟัง (ทันที) │
 │                                                        └─► ส่งออก (dry หรือ wet เลือกได้)
 │   หูฟัง ◄─ [jitter buffer + drift] ◄─ เพลง + เพื่อนคนอื่น (จาก Hub)      │
 │ Net: WsClient (HRA2 PCM → HRA3 Opus) → ภายหลัง WebRTC (K5)              │
 │ UI: plugins/common/ui (Frosted Studio, ไทย/อังกฤษ, ธีม) ใช้ร่วม          │
 └───────────────────────────────────────────────────────────────────────┘
          │  ws(s)://…/ws/s/<token>  (ผ่าน cloudflared หรือเว็บลิงก์ถาวร /api/resolve)
          ▼
   Hub ของเจ้าของห้อง (VST บน DAW)  หรือ  โหมดเปิดห้องของ .exe เอง (K3)
```

### 2.1 สิ่งที่ใช้ซ้ำ (ห้ามเขียนใหม่)

| ของเดิม | ใช้ทำอะไรใน .exe |
|---|---|
| `libs/ssengine` `FriendReader` (+ ปรับตาม PLAN-v2 1.5) | jitter buffer + drift + resample ของเสียงขาลง |
| `libs/ssdsp` (Smoother, limiter, LUFS, `VarResampler`) | ระดับเสียง, กันพีค, มิเตอร์ |
| `libs/ssnet` (สร้างใน PLAN-v2 Phase 2: Opus, HRA3) | codec + wire format |
| `plugins/common/ui/*`, `Strings`, `Settings`, `AccountManager` | หน้าตา, ภาษา, บัญชี (ลิงก์ถาวร) |
| `plugins/hub/ShareServer`, `HubEngine` | โหมดเปิดห้อง (K3) |
| `plugins/app/AppCapture` | ดึงเสียงจาก YouTube/Spotify ในโหมดเปิดห้อง |
| `obs/hearaside-obs` | ไม่ต้องแก้: โหมดเปิดห้องเขียน bus shm เหมือน Hub |
| `installer/windows/hearaside.iss` | เพิ่ม .exe + shortcut ในตัวติดตั้งเดิม |
| `web/site` `/api/resolve/<token>` | หา Hub จากลิงก์ถาวร (แบบเดียวกับ `send.html` `locate()`) |

---

## 3. การตัดสินใจ (ตัวเลือก + คำแนะนำ) ← ArmZ เลือก

### X1 เครื่องมือสร้างโปรแกรม

| ตัวเลือก | ดี | เสีย | แนะนำ |
|---|---|---|---|
| **JUCE GUI app (C++)** | ใช้ engine/UI/ไดรเวอร์ ASIO/WASAPI เดิมได้หมด, latency ต่ำสุด, ไฟล์เล็ก | C++ | ✅ |
| Electron/Tauri + หน้าเว็บเดิม | ทำหน้าตาง่าย | เสียงยังผ่าน Web Audio (หน่วงเท่าเดิม), ไฟล์ใหญ่ (Electron) | ❌ |
| JUCE plug-in แบบ Standalone wrapper | เร็วที่สุด (แค่เปิด format Standalone) | หน้าต่างตั้งค่าเสียงของ JUCE ดูไม่เข้ากับดีไซน์, ไม่มี tray | ใช้เป็น prototype ได้ใน K0 |

### X2 ไดรเวอร์เสียง

1. WASAPI (shared + exclusive) และ DirectSound: มีใน JUCE ใช้ได้ทันที
2. **ASIO**: Steinberg เปิด ASIO SDK แบบสองไลเซนส์ (proprietary หรือ GPLv3) ตั้งแต่ ต.ค. 2025 ถ้าโปรแกรมไม่เปิดซอร์ส (JUCE แบบ commercial) ต้องใช้ไลเซนส์ proprietary ของ Steinberg (ต้องตรวจเงื่อนไขก่อนแจกจ่าย) ถ้าเปิดซอร์สทั้งโปรแกรมภายใต้ AGPL/GPL ใช้ GPLv3 ได้
3. แนะนำ: K1 ปล่อยด้วย WASAPI ก่อน (exclusive ให้ latency ใกล้ ASIO บนการ์ดส่วนใหญ่), เปิด ASIO ใน K2 หลังเคลียร์ไลเซนส์

### X3 เสียงตัวเองที่ส่งไปให้คนอื่น

| ตัวเลือก | แนะนำ |
|---|---|
| ส่ง **dry** (ไม่มีเอฟเฟกต์) ให้ Hub ใส่เอฟเฟกต์เองใน DAW | ✅ ค่าเริ่มเมื่อเข้าห้อง Hub (เจ้าของห้องคุมเสียงรวม) |
| ส่ง **wet** (ผ่านเอฟเฟกต์ในเครื่อง) | ✅ ค่าเริ่มในโหมดเปิดห้อง / เมื่อเจ้าของห้องไม่มีปลั๊กอิน |

### X4 ของเสริมคาราโอเกะ (K6) เลือกได้หลายข้อ

1. เนื้อเพลงแบบ `.lrc` (ข้อความมีเวลา) แสดงเลื่อนตามเพลง
2. เปลี่ยนคีย์เพลง ±6 ครึ่งเสียง (pitch shift คุณภาพดี เช่น Rubber Band: GPL/commercial ต้องตรวจไลเซนส์ หรือ SoundTouch: LGPL)
3. ตัดเสียงร้องต้นฉบับ (vocal remover แบบ center-cancel ทำง่าย / แบบ AI แยกเสียงหนักและต้องโหลดโมเดล)
4. คะแนนร้อง (เทียบ pitch กับไฟล์ MIDI/`.kar`)
5. คิวเพลงของห้อง (เพื่อนกดจองเพลง)
6. **ไม่ทำ**: ไฟล์ `.emk` / ฐานเพลงคาราโอเกะเชิงพาณิชย์ (รูปแบบปิด + ลิขสิทธิ์เพลง)

### X5 ชื่อและที่อยู่โค้ด ✅ ตัดสินแล้ว

1. ชื่อโปรแกรม: **HAS Karaoke** (ArmZ เลือก 18 ต.ค. 2026) ไฟล์ `HAS Karaoke.exe`, deep link `has://join?u=<link>`
2. ที่อยู่โค้ด: `apps/karaoke/` ใน repo เดิม (ใช้ CMake/ตัวติดตั้ง/เวอร์ชันเดียวกัน)

---

## 4. แผนงาน

รูปแบบ: **ID · ไฟล์ · ทำอะไร · ผ่านเมื่อ · ขนาด** (S ≤ 0.5 วัน, M ≤ 2 วัน, L > 2 วัน)

### K0: โครงโปรเจกต์

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| K0.1 | `plugins/CMakeLists.txt`, `plugins/common/CMakeLists.txt` | แยก `plugins/common` (UI, Strings, Settings, Account) เป็น static lib `hearaside_common` ที่ปลั๊กอินและแอปใช้ร่วม | ปลั๊กอินทุกตัว build ผ่าน, `ui-snapshot` เหมือนเดิม ✅ | M |
| K0.2 | `plugins/common/ShareServer.*` | ย้ายจาก `plugins/hub` สู่ `plugins/common` ให้แอปใช้ได้ | test เดิม (`friends_room`, `friend_page`) ผ่าน ✅ | M |
| K0.3 | `apps/karaoke/CMakeLists.txt`, `Main.cpp`, `MainWindow.*` | `juce_add_gui_app` หน้าต่างเดียว ธีม Frosted Studio, tray icon, single instance, จำขนาดหน้าต่าง, ไมค์มิเตอร์ | เปิด/ปิดได้ ไม่มี leak ✅ | M |
| K0.4 | `CMakeLists.txt` | option `HEARASIDE_BUILD_KARAOKE` (ค่าเริ่ม ON บน Windows) | `.\build.ps1` ได้ `HAS Karaoke.exe` (5.17 MB) ✅ | S |

### K1: โหมดเข้าห้อง MVP (ใช้กับ Hub 1.0.4 ได้)

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| K1.1 | `apps/karaoke/AudioSettings.*` | เลือกไดรเวอร์ (WASAPI shared/exclusive, DirectSound), input/output, ช่องไมค์, sample rate, buffer + แสดง latency ที่ไดรเวอร์รายงาน | สลับอุปกรณ์ขณะเปิดอยู่ได้ไม่ crash ✅ | M |
| K1.2 | `apps/karaoke/JoinClient.*` | วางลิงก์ `https://…/s/<token>` หรือ `…/ws/s/<token>` → ถ้าเป็นเว็บลิงก์ถาวร เรียก `/api/resolve/<token>` เหมือน `send.html` `locate()` → ต่อ WebSocket (TLS ผ่าน `juce::URL`/WinHTTP หรือ lib WS ขนาดเล็กที่ไลเซนส์ MIT) | ต่อ Hub 1.0.4 ได้ทั้งลิงก์ cloudflared, ลิงก์ในวงแลน, ลิงก์ถาวร ✅ | M |
| K1.3 | `JoinClient` (wire) | ส่งไมค์ HRA2 (mono, 10 ms, `pos` = ตำแหน่งเพลงที่กำลังได้ยินลบ latency ของเครื่อง เหมือน worklet) รับ HRA2 ขาลง → `FriendReader` → หูฟัง; อ่าน JSON `{"v":2}`, `{"delay":…}` | Hub แสดงชื่อเพื่อน + "ช้าราว X ms" ถูกต้อง; test เทียบกับหน้าเว็บ ✅ | M |
| K1.4 | `apps/karaoke/VocalChain.*` | เสียงตัวเองในหูฟังทันที: gate, EQ 3 แบนด์, comp, **reverb + echo** (ใช้ `juce::dsp::Reverb`, delay line) + ปุ่มปิดเสียงตัวเองในหูฟัง; เลือกส่ง dry/wet (X3) | latency ไมค์ → หูฟัง ≤ 30 ms (WASAPI shared) วัดด้วย loopback ✅ | M |
| K1.5 | `MainWindow` | หน้าจอเดียวใหญ่ ๆ: ช่องวางลิงก์ · ปุ่ม "เข้าห้อง" · มิเตอร์ไมค์ · แถบ "เพลง" / "เสียงฉัน" / "เสียงเพื่อน" · ไฟสถานะเขียว/เหลือง/แดง · ปุ่มปิดไมค์ (hotkey ตั้งได้) | ผู้ใช้ใหม่เข้าห้องได้โดยไม่อ่านคู่มือ ✅ | M |
| K1.6 | `apps/karaoke/` deep link | ลงทะเบียน `has://join?u=<link>` ใน installer: หน้าเว็บ `send.html` มีปุ่ม "เปิดในโปรแกรม" | คลิกจากเว็บแล้วแอปเปิดพร้อมเข้าห้อง ✅ | S |
| K1.7 | `tests/` | test ที่ไม่ต้องมีการ์ดเสียง: `JoinClient` กับ `ShareServer` จริงใน process เดียว (loopback) ตรวจ mix-minus, ตำแหน่ง `pos`, การต่อใหม่ | ผ่านบน Linux + Windows ✅ | M |

### K2: คุณภาพเสียง (หลัง PLAN-v2 Phase 1–3)

| ID | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|
| K2.1 | ใช้ jitter buffer ใหม่ (PLAN-v2 1.5) + ตัวนับ (0.1) ในแอป (`FriendReader` bufferMs, jitterP95Ms, underruns, concealedMs) | net-sim jitter 20 ms: gap ≤ 1/นาที ✅ | S |
| K2.2 | Opus HRA3 ผ่าน `libs/ssnet` (hello v3) fallback HRA2 กับ Hub เก่า | ขาขึ้น ≤ 96 kbps, ต่อ Hub 1.0.4 ยังได้ ✅ | M |
| K2.3 | **เช็กก่อนร้อง**: ไมค์มีเสียง, หูฟังหรือลำโพง (chirp เบา ๆ), เน็ต (RTT/jitter/kbps) → "พร้อมร้อง ✓" หรือบอกวิธีแก้ | ตรวจลำโพงรั่วได้ถูกในการทดสอบ 3 แบบ (หูฟังปิด, หูฟังเปิด, ลำโพง) ✅ | M |
| K2.4 | AEC ในเครื่อง (speexdsp `mdf.c` ตัวเดียวกับ PLAN-v2 3.1) เมื่อใช้ลำโพง: reference = เสียงที่เครื่องเล่นออก (รู้แน่นอน, ไม่มี delay เน็ต) | ERLE ≥ 25 dB ในการจำลอง ✅ | M |
| K2.5 | ASIO (หลังเคลียร์ไลเซนส์ X2) | เลือก ASIO ได้, latency ≤ 10 ms ที่ 128 ✅ | S |

### K3: โหมดเปิดห้อง (ไม่ต้องมี DAW)

| ID | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|
| K3.1 | **แหล่งเพลง**: ไฟล์ (WAV/MP3/FLAC/M4A ด้วย JUCE format readers + Media Foundation บน Windows), หรือดึงจากโปรแกรม (`AppCapture`: YouTube ในเบราว์เซอร์ ฯลฯ), รายการเพลง + เล่น/หยุด/เลื่อน | เล่นไฟล์ได้ทุกนามสกุลที่ระบุ, capture YouTube ได้ ✅ | M |
| K3.2 | **Engine ในตัว**: เพลง + ไมค์ (ผ่าน `VocalChain`) เขียนเป็น "slot" บน bus shm แล้วรัน `HubEngine` ในแอป = แอปทำตัวเป็น Hub (ไม่ต้องแก้ OBS Source) + `ShareServer` เปิดลิงก์ฟัง/ลิงก์เพื่อน + cloudflared | OBS Source เห็นเสียง, เพื่อนเข้าด้วยลิงก์ได้, Line up ทำงาน ✅ | L |
| K3.3 | ห้ามเปิดคู่กับ Hub ใน DAW บน bus เดียวกัน: ใช้ bus ชื่อ `Karaoke` หรือเตือนเมื่อเจอ Hub อื่น (มี `hubOwnerToken` อยู่แล้ว) | ไม่มีเสียงดังซ้ำ ✅ | S |
| K3.4 | หน้าจอเจ้าของห้อง: เพลงที่เล่น, รายชื่อเพื่อน (ไฟสถานะ, ระดับ, ปิดเสียง, เตะออก), ปุ่ม "ไลฟ์" (ส่ง OBS), อัดทั้งห้อง/แยกไฟล์ (PLAN-v2 1.10) | ตั้งห้องได้ ≤ 3 คลิก ✅ | M |
| K3.5 | ลิงก์ถาวรผ่านบัญชีเดิม (`AccountManager`, `ShareDirectory`) | ลิงก์เหมือนเดิมทุกครั้งที่เปิดห้อง ✅ | S |

### K4: ติดตั้ง + อัปเดต

| ID | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|
| K4.1 | `hearaside.iss`: เพิ่ม .exe, Start menu, เลือกได้ว่าจะลง "แค่โปรแกรมคาราโอเกะ" (สำหรับเพื่อนที่ไม่ใช้ DAW) หรือครบชุด | ติดตั้ง/ถอนบนเครื่องสะอาดได้ | M |
| K4.2 | ตรวจเวอร์ชันใหม่จาก `web/site/src/content/release.json` ตอนเปิดแอป (แค่แจ้ง ไม่อัปเดตเอง) | แจ้งเมื่อมีรุ่นใหม่ | S |
| K4.3 | เซ็นรหัส (code signing) .exe และตัวติดตั้ง เพื่อไม่ให้ SmartScreen เตือน (ต้องมีใบรับรอง) | — | S (เมื่อมีใบรับรอง) |
| K4.4 | หน้า `/download` บนเว็บ: ปุ่ม "โปรแกรมคาราโอเกะสำหรับเพื่อน" (ตัวติดตั้งเล็ก) | — | S |

### K5: UDP

1. ใช้ `libs/ssnet/rtc` จาก PLAN-v2 Phase 4 ตรง ๆ (แอปเป็น peer เหมือนเบราว์เซอร์)
2. ลำดับ: WebRTC ตรง → TURN → WS Opus → WS PCM

### K6: ของเสริมคาราโอเกะ

ทำตามที่เลือกใน X4 ทีละข้อ แต่ละข้อเขียนแผนย่อยในไฟล์นี้ก่อนเริ่ม

---

## 5. การทดสอบ

1. **Unit / integration (Linux ได้)**: `JoinClient` ↔ `ShareServer` ใน process เดียว, `VocalChain` (ไม่มี denormal, ไม่มี NaN, latency = 0 sample), ตัวแปลงลิงก์
2. **Windows (ArmZ)**: `.\build.ps1 -Test` + เปิดแอปจริง
3. **อุปกรณ์**: หูฟัง USB, audio interface (ASIO), ไมค์ USB, บลูทูธ (ต้องเตือนว่าหน่วงมาก ไม่เหมาะร้อง)
4. **เน็ต**: `tools/net-sim` (PLAN-v2 0.3) 3 โปรไฟล์
5. **ร้องจริง**: เพื่อน 2 คน (คนหนึ่งใช้ .exe คนหนึ่งใช้หน้าเว็บ) ร้องเพลงเดียวกัน เทียบ latency ที่ Hub แสดง และความรู้สึกตอนร้อง
6. ห้าม merge ถ้า test ปลั๊กอินเดิมพัง (K0 แตะไฟล์ที่ปลั๊กอินใช้ร่วม)

---

## 6. ความเสี่ยง

| ความเสี่ยง | ลดอย่างไร |
|---|---|
| แยก `plugins/common` เป็น lib แล้วปลั๊กอินพัง | ทำ K0.1 เป็นงานแยก, ui-snapshot เทียบพิกเซล, plugin-host-test |
| ไลเซนส์ ASIO / JUCE / pitch shift | ตรวจก่อน merge ตาม checklist PLAN-v2 9.4 ข้อ 9; K1 ใช้ WASAPI ก่อน |
| บลูทูธ / ไดรเวอร์แย่ ทำให้หน่วงมาก | แสดง latency ที่วัดได้ + คำแนะนำ, ไม่บล็อกการใช้งาน |
| โหมดเปิดห้องชนกับ Hub ใน DAW | K3.3 |
| SmartScreen เตือนเพราะไม่ได้เซ็น | บอก SHA-256 ในหน้า download (ทำอยู่แล้ว) จนกว่าจะมีใบรับรอง |

---

## 7. Prompt แม่แบบสำหรับ Builder (Sonnet 5.5 high)

```text
คุณคือ Builder ของ HEARASIDE ทำงาน ID <K…> จาก "Hearaside plugin/docs/PLAN-karaoke-exe.md" เท่านั้น
อ่าน PLAN-v2-realtime-karaoke.md หัวข้อ 9 (กติกา) และหัวข้อ 2.1 ของไฟล์นี้ (ของที่ต้องใช้ซ้ำ) ก่อนเริ่ม
โหมด: ponytail full · ห้ามเขียนของที่มีอยู่แล้วซ้ำ · ห้ามคัดลอกโค้ด GPL (Jamulus) หรือของเชิงพาณิชย์
Audio callback: ห้าม lock / allocate / log / IO
ผ่านเมื่อ: <acceptance>
build/test: ตาม PLAN-v2 หัวข้อ 8.1 (Linux) ส่วน GUI ให้ระบุคำสั่ง Windows ที่ ArmZ ต้องรัน
commit บน branch karaoke/<ID>-<slug> · รายงานกลับเป็นข้อ ๆ ภาษาไทย: ไฟล์ที่แก้, ผล test, สิ่งที่ยังไม่ได้ตรวจ
ใกล้หมด context: เขียน docs/handoff/<วันที่>-<ID>.md แล้วหยุด
```

---

## 8. คำถามที่ต้องให้ ArmZ ตอบ

> **18 ต.ค. 2026: ArmZ ให้ "จัดการให้เลย"** จึงใช้ค่าที่แนะนำ: เริ่มโหมดเข้าห้องก่อน · ส่ง dry เป็นค่าเริ่ม · ของเสริม K6 ยังไม่ทำ · ปล่อยด้วย WASAPI ก่อน (ASIO รอเคลียร์ไลเซนส์ ข้อ 4) · ยังไม่เซ็นรหัส


1. เอาทั้ง 2 โหมด (เข้าห้อง + เปิดห้อง) หรือเริ่มแค่ **เข้าห้อง** ก่อน (แนะนำ)?
2. X3: ค่าเริ่มส่งเสียง dry ให้ Hub ตกลงไหม?
3. X4: ของเสริมคาราโอเกะข้อไหนที่อยากได้ (เนื้อเพลง / เปลี่ยนคีย์ / ตัดเสียงร้อง / คะแนน / คิวเพลง)?
4. จะขายโปรแกรมนี้แยก หรือแจกฟรีคู่กับ HEARASIDE? (มีผลกับไลเซนส์ JUCE / ASIO)
5. มีใบรับรอง code signing หรือยัง?

---

## แหล่งอ้างอิง

1. Steinberg: VST 3.8 MIT + ASIO dual license (press release 29 ต.ค. 2025): https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf
2. Libre Arts: Steinberg relicenses VST3 and ASIO: https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/
3. Jamulus Software Manual: https://jamulus.app/wiki/Software-Manual
