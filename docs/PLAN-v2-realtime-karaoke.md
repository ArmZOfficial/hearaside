# HEARASIDE v2: แผนปรับ Realtime + Karaoke (แนว Jamulus + ListenTo)

> เขียนโดย Opus 5.5 (high) · 18 ต.ค. 2026 · จากโค้ดหลัง commit `7bbfc45` (1.0.4, `kProtocolVersion = 14`)
> ไฟล์นี้คือ **แผนหลักฉบับเดียว** ทุกคน (คนและ AI) แก้ที่นี่ที่เดียว สถานะงานอยู่ในตารางหัวข้อ 0.3
> ลำดับการทำงาน: **Opus 5.5 high วางแผน → Sonnet 5.5 high ลงมือ → Opus 5.5 high รีวิว** (รายละเอียดหัวข้อ 9)

---

## 0. สรุปสั้น

### 0.1 ปัญหาที่ผู้ใช้เจอ และสาเหตุจริงที่พบในโค้ด

| # | อาการ | สาเหตุหลัก (มีหลักฐานในหัวข้อ 3) | แก้ที่ Phase |
|---|---|---|---|
| A | **มิกซ์เสียงเพื่อนใน Track ไม่ได้ เสียงไม่เข้า** | ระบบจับคู่ Friend input ↔ Track ท้ายช่อง **ติดตาย (deadlock)**: ตัวบนปล่อยความเงียบจนกว่าจะจับคู่ แต่การจับคู่สำรองต้องได้ยินเสียงเพื่อนที่ Track ท้ายช่องก่อน · การจับคู่ด้วยชื่อใช้ไม่ได้ใน VST2 / DAW ที่ไม่ส่งชื่อแทร็ก | 1 |
| B | **ได้ยินเสียงตัวเองซ้อน / เสียงสะท้อน** ตอนเพื่อนมาร้อง | มี 4 ทาง: (1) เพื่อนไม่ใส่หูฟัง ไมค์เพื่อนอัดเสียงเราจากลำโพงส่งกลับ (หน้าเว็บปิด echoCancellation) (2) อัดเพื่อนผ่าน App Audio แล้วเสียงเพื่อนวิ่งเข้า master ซึ่งเป็นสิ่งที่เพื่อนได้ยิน (3) direct monitoring ของ audio interface + ได้ยินจาก DAW อีกรอบ (4) ไม่มีตัวตรวจจับให้รู้ว่าเกิดอะไร | 1, 3 |
| C | **กระตุก / สะดุด / stutter** | (1) jitter buffer ทั้งฝั่ง Hub และหน้าเว็บ "เงียบแล้วเติมใหม่" ทุกครั้งที่ขาด = ช่องว่าง 40–100 ms (2) ส่งเสียงเพื่อนแบบ PCM 16-bit สเตอริโอไม่บีบอัด ~1.5 Mbps ต่อคนต่อทาง ผ่าน TCP (3) เธรดเดียวทั้งรับไมค์และส่งเพลงต่อเพื่อน 1 คน: ขาลงช้า → ขาขึ้นค้าง (4) คิวส่งฝั่ง Hub ไม่จำกัด เสียงค่อย ๆ ช้าลง | 1, 2, 4 |
| D | **หน่วง / delay สูง** | jitter buffer เริ่มที่ ~block+70 ms และหดช้ามาก (1 ms ทุก 3 วิ) · Friend input เตรียม reader ด้วย block 2048 เสมอ (+30 ms) · TCP + Cloudflare tunnel · DAW buffer ใหญ่ = ส่งเป็นก้อนใหญ่ | 1, 2, 4 |
| E | **ใช้ยาก เข้าใจยาก** | ต้องรู้เรื่อง route / pair / Line up / Track 2 ตัว เอง ไม่มีไฟสถานะแบบ Jamulus ไม่มีตัวช่วยเช็กหูฟัง / เน็ต | 5 |

### 0.2 คำแนะนำของผม (ลำดับที่ควรทำ)

1. **Phase 0 + 1 ก่อน (1.0.5, ไม่เปลี่ยน protocol shm)** แก้ A ให้จบ + แก้กระตุก/หน่วงที่แก้ได้โดยไม่เปลี่ยนรูปแบบเสียง ได้ผลเร็วที่สุดต่อแรงที่ลง
2. **Phase 2 Opus codec (1.1.0)** ลดข้อมูล ~10–20 เท่า ปัญหากระตุกจากเน็ตส่วนใหญ่หายไปเองแม้ยังใช้ TCP
3. **Phase 3 กันเสียงสะท้อน** ด้วย AEC ฝั่ง Hub (speexdsp ที่อยู่ในโปรเจกต์แล้ว) + ตัวตรวจหูฟัง/เสียงซ้ำ
4. **Phase 5 โหมดคาราโอเกะ** (UX) ทำคู่ขนานกับ Phase 2–3 ได้ เพราะแตะไฟล์ UI คนละชุด
5. **Phase 4 UDP (WebRTC DataChannel)** เมื่อ 1–3 นิ่งแล้ว ได้ latency ต่ำสุดแบบ Jamulus
6. Phase 6 (DAW ถึง DAW แบบ Jamulus เต็มตัว) และ Phase 7 (optimize ลึก) ทำเมื่อมีเวลา

### 0.3 สถานะ (อัปเดตทุกครั้งที่ปิดงาน)

| Phase | ชื่อ | เวอร์ชัน | protocol shm | สถานะ |
|---|---|---|---|---|
| 0 | วัดผลก่อนแก้ (baseline + net-sim) | — | ไม่เปลี่ยน | ✅ |
| 1 | แก้ด่วน: จับคู่เพื่อน, jitter buffer, เธรดส่ง, mono, echo path | 1.0.5 | ไม่เปลี่ยน | ✅ |
| 2 | Opus codec + HRA3 | 1.1.0 | ไม่เปลี่ยน (เปลี่ยนแค่ wire) | ✅ |
| 3 | กันเสียงสะท้อน (AEC + detector) | 1.1.x | ไม่เปลี่ยน | ✅ |
| 4 | UDP: WebRTC DataChannel | 1.2.0 | ไม่เปลี่ยน | ✅ |
| 5 | โหมดคาราโอเกะ + UX ใหม่ | 1.1–1.2 | ไม่เปลี่ยน | ✅ |
| 6 | Hub ถึง Hub (DAW ถึง DAW) | 2.0 | v15+ | 💤 ตัวเลือก |
| 7 | Big optimize (RT safety, SIMD, thread) | ทยอย | ไม่เปลี่ยน | ⏳ |
| K | โปรแกรม HAS Karaoke (.exe) | แผนแยก [PLAN-karaoke-exe.md](PLAN-karaoke-exe.md) | ไม่เปลี่ยน | ⏳ |

---

## 1. เป้าหมายและตัวชี้วัด (Definition of success)

ทุกตัวเลขวัดด้วยเครื่องมือ Phase 0 (ห้ามเดา) ค่า "ตอนนี้" ในตารางเป็น **ค่าประมาณจากโค้ด** ต้องแทนด้วยค่าวัดจริงใน Phase 0.4

| ตัวชี้วัด | ตอนนี้ (ประมาณ) | เป้า v1.1 | เป้า v1.2 (UDP) |
|---|---|---|---|
| เสียงเพื่อนถึงหูฟังเรา (one-way, เน็ตในประเทศ, DAW 256) | 80–150 ms | ≤ 60 ms p50 | ≤ 40 ms p50 |
| เพื่อน A ได้ยินเพื่อน B | 150–250 ms | ≤ 90 ms p50 | ≤ 60 ms p50 |
| เสียงขาด (gap ≥ 10 ms) ต่อนาที เน็ตปกติ | หลายครั้ง | ≤ 1 | ≤ 0.2 |
| ข้อมูลขาขึ้นต่อเพื่อน | ~1.54 Mbps | ≤ 96 kbps | ≤ 96 kbps |
| ข้อมูลขาลงต่อเพื่อน | ~1.54 Mbps | ≤ 160 kbps | ≤ 160 kbps |
| จับคู่เพื่อนในแทร็ก DAW สำเร็จ (ทุก DAW ใน matrix) | ขึ้นกับชื่อแทร็ก | 100 % ภายใน 2 คลิก | 100 % |
| คนที่ได้ยินเสียงตัวเองย้อนกลับ (เพื่อนใช้ลำโพง) | ได้ยินชัด | ลด ≥ 25 dB (ERLE) | ลด ≥ 25 dB |
| ขั้นตอนตั้งค่าห้องคาราโอเกะ (คลิก) | ~10+ | ≤ 3 | ≤ 3 |
| CPU audio thread ของ Hub (8 เพื่อน, 48 k, 256) | ไม่ทราบ | วัดแล้วไม่เพิ่มเกิน +10 % | — |

---

## 2. สถาปัตยกรรมตอนนี้ (as-is)

```
 เบราว์เซอร์เพื่อน (send.html)                   เครื่อง ArmZ
 ┌────────────────────────┐   WebSocket (TCP)  ┌──────────────────────────── Hub (VST บน Master) ───────────────┐
 │ mic → AudioWorklet      │──── HRA2 PCM16 ───►│ ShareServer: 1 เธรด/เพื่อน (รับไมค์ + ส่งเพลง ในลูปเดียว)      │
 │  10 ms/packet, stereo   │  via cloudflared   │   └─ ringWrite → shm friendAudio[j]                           │
 │ jitter buf 60 ms+ → 🎧  │◄─── HRA2 PCM16 ────│   ◄─ friendMixAudio (Stream Mix ก่อน Line up) + friendVoice   │
 └────────────────────────┘                    │      ของเพื่อนคนอื่น (mix-minus)                               │
                                               │ HubEngine (audio thread): FriendReader (jitter+drift+resample)│
                                               │   → หูฟัง (ไม่หน่วง) + Stream Mix (หลัง Line up D)              │
                                               └───────────────────────────────────────────────────────────────┘
 ทาง DAW (S7):  friendAudio[j] → [Track: Friend input] → EQ/Comp/Reverb → [Track ท้ายช่อง] ─(kFlagViewersViaHub)→ Hub
                 จับคู่ด้วย: ชื่อแทร็กตรงกัน หรือ correlation ของ feederTap กับเสียงของ slot
```

ไฟล์หลัก:

1. `libs/ssbus/include/ssbus/protocol.h` layout shm v14 (`FriendHeader`, `FeederRecord`, `PluginRequest`, `feederTap`)
2. `libs/ssengine/src/friend_reader.cpp` jitter buffer ฝั่ง Hub และ Friend input
3. `libs/ssengine/src/hub_friends.cpp` มิกซ์เพื่อน, Line up, mix-minus
4. `libs/ssengine/src/hub_engine.cpp` อ่าน Track, จับคู่ S7 (`measureLatencies`, บรรทัด ~216–364)
5. `plugins/hub/ShareServer.cpp` WebSocket, `streamTo` (ลิงก์ฟัง), `friendSession` (เพื่อน)
6. `plugins/hub/web/send.html`, `listen.html` หน้าเว็บ (ต้นฉบับ sync ไป `web/share-vercel/public`)
7. `plugins/track/TrackProcessor.cpp` (`processFriend`), `plugins/app/AppAudioProcessor.cpp` (โหมดเพื่อน)
8. `plugins/hub/HubProcessor.cpp` ตัดสิน route ของเพื่อน (บรรทัด ~690–727)

---

## 3. วิเคราะห์สาเหตุ (หลักฐานจากโค้ด)

### 3.A เสียงเพื่อนไม่เข้าแทร็ก (deadlock การจับคู่)

1. `TrackProcessor.cpp:171-173` ตัวบน (Friend input) ตั้ง `friendGain_` เป็น 0 ถ้า `fh.outSlot < 0` (ยังไม่จับคู่) และ `KeepOwn` ค่าเริ่มเป็น `false` (`:42`) ดังนั้นสัญญาณที่ไหลไปปลั๊กอินถัดไปและ Track ท้ายช่อง = **ความเงียบ**
2. `hub_engine.cpp:258-270` จับคู่ทางที่ 1 = ชื่อ: `feederName == slotName` ชื่อมาจาก host (`updateTrackProperties`, VST3) ถ้าไม่มีชื่อ feeder ใช้ชื่อเพื่อน (`TrackProcessor.cpp` `displayName()`: "Mint") แต่ Track ท้ายช่องใช้ "Track N" → **ไม่ตรงกันตลอดไป** (VST2 ทุก DAW, FL Studio, DAW ที่ไม่ส่งชื่อ, แทร็กชื่อซ้ำ)
3. `hub_engine.cpp:272-312` จับคู่ทางที่ 2 = correlation ระหว่าง `feederTap` (เสียงเพื่อนดิบ) กับประวัติเสียงของแต่ละ slot แต่ slot ท้ายช่องได้แต่ความเงียบ (ข้อ 1) และ `eySlot < tapN*1e-6` → `continue` (`:303`) **จึงไม่มีทางจับคู่ได้**
4. `HubProcessor.cpp:720-724` route = Daw ก็ต่อเมื่อ paired → ไม่ paired = Hub เล่นเพื่อนทางตรง ผู้ใช้เห็นแทร็กเงียบ = "เสียงไม่เข้า"
5. ซ้ำเติม: Cubase "Suspend VST3 plug-in processing when no audio signals are received" / FL "Smart disable" พักแทร็กว่าง → heartbeat feeder หยุด → หลุดคู่ (มีเตือนแล้วใน proposal S7 ข้อ 5 แต่ยังไม่ทดสอบ DAW จริง)

### 3.B ได้ยินเสียงตัวเองซ้อน / สะท้อน

1. **เพื่อนใช้ลำโพง**: `send.html:344` `echoCancellation: false` หน้าเว็บเล่นเพลง + เสียงเรา + เพื่อนคนอื่นออกลำโพง ไมค์เพื่อนอัดกลับมา → เรา (และเพื่อนคนอื่น) ได้ยินเสียงตัวเองช้าไป 1 รอบเน็ต ไม่มีอะไรตรวจหรือกันเลย มีแค่ข้อความ "ใส่หูฟัง"
2. **อัดเพื่อนผ่าน App Audio**: `AppAudioProcessor.cpp:126-130` ในโหมดเพื่อน ปิดเฉพาะ "คุณได้ยิน" แต่ "คนดูได้ยิน" ยังส่งเสียงเพื่อนออก DAW → ถ้าแทร็กที่อัด monitor อยู่ เสียงเพื่อนเข้า master → `friendMixAudio` (สิ่งที่เพื่อนทุกคนได้ยิน, `hub_friends.cpp:113-116`) → **เพื่อนได้ยินตัวเองย้อนกลับ** + คนดูได้ยินเพื่อน 2 ทาง (Hub ตรง + master) README แนะนำวิธีนี้อยู่ ("อัดเพื่อนลง DAW: ใส่ App Audio ที่ช่อง Input")
3. **ฝั่งเราเอง**: Hub ประกอบหูฟังจากเสียงที่ Track ส่งมา อ่านช้ากว่า 1 block เสมอ (`hub_engine.cpp:438` `back = n * (1 + syncSafety)`) ถ้า audio interface เปิด direct monitoring ด้วย = ได้ยินเสียงร้องตัวเอง 2 ครั้งห่างกัน 10–30 ms (เสียงแตก/ก้อง)
4. **ไม่มีตัวตรวจ**: Hub มีเครื่องมือ correlation (`measureLag`, `bestLag`) อยู่แล้วแต่ไม่ได้ใช้หา "เสียงเพื่อนอยู่ใน master ขณะ route = Direct" หรือ "เสียงที่ส่งให้เพื่อนวนกลับมาในไมค์เพื่อน"

### 3.C กระตุก / สะดุด

1. **jitter buffer ฝั่ง Hub เงียบแล้วเติมใหม่**: `friend_reader.cpp:106-116` ขาดเมื่อไร → `primed_ = false`, ความเงียบ, รอจน `w >= starvedAt_ + target_ + n` (`:95`) = ช่องว่างยาวเท่า target (40–100 ms) ทุกครั้ง ไม่มีการปกปิด (PLC / fade) และเพิ่ม target ทีละ 2 ms (`:108`) → ช่วงแรกขาดซ้ำหลายรอบ
2. **หน้าเว็บก็แบบเดียวกัน**: `send.html:281-283` ขาด → `primed = false` เงียบจนเติมเต็ม target แล้ว +15 ms
3. **PCM16 สเตอริโอไม่บีบอัด**: `send.html:344` ขอไมค์ `channelCount: 2`, ส่ง 960 sample/10 ms (`:312`) = 1.536 Mbps ขาขึ้น ขาลง (`ShareServer.cpp` `friendSession` ส่ง HRA2 สเตอริโอ) อีก 1.536 Mbps ต่อเพื่อน 1 คน 8 คน = ~12 Mbps ขาขึ้นจากเครื่องเรา ผ่าน TCP: แพ็กเก็ตหาย 1 ตัว = ทุกอย่างหลังจากนั้นรอ (head-of-line blocking)
4. **เธรดเดียวสองทาง**: `ShareServer.cpp:698-801` ลูปเดียวทั้ง `readMessage` (ไมค์) และ `sendFrame` (ขาลง) `sendFrame`→`writeAll` แบบ blocking ถ้าขาลงของเพื่อนช้า ลูปค้างที่ส่ง → ไม่ได้อ่านไมค์ → ring ของเพื่อนขาด → Hub กระตุก (ขาลงช้าทำให้ขาขึ้นพังด้วย)
5. **ไม่จำกัดคิวส่งฝั่ง Hub**: ฝั่งเว็บทิ้งเมื่อ `bufferedAmount > 40000` (`send.html:442`) แต่ฝั่ง Hub ไม่มี เสียงที่ค้างใน TCP send buffer ของ Windows (auto-tune ได้หลาย MB) → เพื่อนได้ยินเพลงช้าลงเรื่อย ๆ จนกว่า jitter buffer เว็บจะกระโดด (`fill > want + 0.6 s`) = สะดุดเป็นพัก ๆ
6. **ส่งเป็นก้อนตาม DAW block**: Hub เขียน `friendMixAudio` ทีละ block (2048 = 43 ms) ฝั่งเว็บต้องตั้ง floor = pkt + 10 ms (`send.html:289`) DAW buffer ใหญ่ = หน่วงและกระตุกง่าย ไม่มีคำเตือนใน UI
7. `sendFrame` (`ShareServer.cpp:102-109`) เขียน header กับ payload แยก 2 ครั้ง + TCP_NODELAY (JUCE ตั้งให้) = 2 segment ต่อแพ็กเก็ต (เล็กแต่ฟรีที่จะแก้)

### 3.D หน่วง (latency budget ประมาณ, DAW 256 @ 48 k, เน็ตในไทย)

| ช่วง | ตอนนี้ | หลัง Phase 1–2 | หลัง Phase 4 |
|---|---|---|---|
| ไมค์เบราว์เซอร์ (input latency) | 10–20 | 10–20 | 10–20 |
| แพ็กเก็ต | 10 | 10 (Opus 10 ms, เลือก 5 ได้) | 5–10 |
| เน็ตขาขึ้น (เพื่อน → CF edge → cloudflared → Hub) | 15–40 + TCP retransmit | 15–40 | 8–25 (UDP ตรง / TURN) |
| jitter buffer ของ Hub (`FriendReader`) | block + 70 (~75) หดช้า | 15–30 (ปรับตาม jitter จริง + PLC) | 10–20 |
| อ่าน Track ช้า 1 block (เฉพาะทาง DAW) | 5.3 | 5.3 | 5.3 |
| Friend input เตรียม reader ด้วย 2048 | +30 | 0 | 0 |
| DAW output → หูฟัง | 5–10 | 5–10 | 5–10 |
| **รวม (ms)** | **~120–190** | **~55–110** | **~45–90** |

---

## 4. เรียนรู้จาก Jamulus และ ListenTo

### 4.1 Jamulus: ยืม / ไม่ยืม

| แนวคิด Jamulus | ใช้กับ HEARASIDE อย่างไร | Phase |
|---|---|---|
| UDP + OPUS, เฟรมเล็ก (64/128 sample) | Opus 10 ms (5 ms ตัวเลือก) บน WS ก่อน แล้ว WebRTC DataChannel แบบ unreliable | 2, 4 |
| Auto jitter buffer ทั้งฝั่ง client และ server | `FriendReader` + worklet ปรับ target จาก jitter ที่วัดได้ (p95) แทน +2 ms คงที่ หดเร็วขึ้นเมื่อนิ่ง | 1 |
| ไฟ delay เขียว/เหลือง/แดง + ไฟ jitter แดงเมื่อขาด | ไฟสถานะต่อเพื่อนทั้งใน Hub และหน้าเว็บเพื่อน | 0, 5 |
| Server ทำ mix ส่วนตัวให้ทุกคน | มีแล้ว (mix-minus ใน `friendSession`) ย้ายไปเธรดส่งแยก + encode ต่อคน | 1, 2 |
| Mono / Mono-in-Stereo-out / Stereo | ไมค์เพื่อนเป็น mono เริ่มต้น, ขาลง stereo (= Mono-in-Stereo-out) | 1 |
| ได้ยินเสียงตัวเองจาก server (เพื่อให้ตรงจังหวะคนอื่น) | **ตัวเลือก** "ได้ยินตัวเองจาก Hub" ปิดเป็นค่าเริ่ม (คาราโอเกะไม่ต้องการ) | 3 |
| Mute myself | มีปุ่มปิดไมค์แล้ว | — |
| Server recorder (อัดทุกช่องแยกไฟล์) | "อัดเพื่อนแยกไฟล์" ใน Hub แทนการใช้ App Audio ที่ช่อง Input (ตัดทาง echo 3.B.2) | 1 |
| แนะนำ buffer 128, upload ไม่เกินเน็ต | เตือนเมื่อ DAW buffer > 256 ตอนมีเพื่อน, แสดง kbps ต่อเพื่อน | 1, 5 |

> **License:** Jamulus เป็น GPL-2.0 ส่วน `libs/*` ของเราเป็น MIT **ห้ามคัดลอกโค้ด Jamulus** มาใส่ ใช้แค่แนวคิด/พฤติกรรม เขียนใหม่เอง

### 4.2 ListenTo: ยืม / ไม่ยืม

| แนวคิด ListenTo | ใช้กับ HEARASIDE | Phase |
|---|---|---|
| เลือกรูปแบบเสียง: PCM 16/24/32, AAC, Opus 128/256 | ลิงก์ฟัง: Auto / Opus 128 / Opus 256 / PCM16 / PCM24 พร้อมบอก kbps ที่ต้องใช้ | 2 |
| ตั้งค่า latency ฝั่งผู้ฟัง (ListenTo plug-in เริ่ม ~100 ms, Web Transmitter 0.1–2 วิ) | ลิงก์ฟังมี เร็วสุด/สมดุล/ลื่นสุด อยู่แล้ว เพิ่ม "Auto" ที่ปรับตามเน็ต | 2 |
| Receiver plug-in (รับลิงก์เข้า DAW) + recorder ในตัว | App Audio รับลิงก์ได้แล้ว เพิ่มอัดในตัว (มี TakeRecorder อยู่แล้ว) | 1 |
| Session password / listener approval | ตัวเลือกรหัสผ่านลิงก์ฟัง + กดอนุมัติเพื่อนใหม่ | 5 |
| ชื่อ session คงที่ = ลิงก์คงที่ | มีแล้ว (ลิงก์ถาวรผ่านบัญชี) | — |

### 4.3 การใช้ skill `reverse-engineer-anything` (rea)

1. skill นี้บอกเองว่า **ไม่ใช้กับ repo ที่มีซอร์สครบ** → Jamulus อ่านซอร์สจาก GitHub ตรง ๆ ไม่ต้องใช้ rea
2. ใช้ rea กับ **ListenTo เฉพาะสิ่งที่เราเห็นเองในเบราว์เซอร์**: เปิดลิงก์ฟัง ListenTo ของ session ตัวเอง แล้วใช้ `list_browser_targets` / `capture_browser_scenario` หรือบันทึก HAR แล้ว `inspect_web_network_capture` เพื่อดู transport (WebSocket? WebRTC?), codec, ขนาดแพ็กเก็ต, ขนาด buffer ของ player
3. **ไม่ decompile ปลั๊กอิน ListenTo** (ซอฟต์แวร์เชิงพาณิชย์ EULA ห้าม) ใช้เฉพาะ runtime ที่สังเกตได้
4. ใช้ rea กับ **ไฟล์ build ของ HEARASIDE เอง** ได้เต็มที่ เช่น ตรวจว่า `.vst3` ไม่ link DLL แปลก ๆ หรือดูหน้าเว็บเพื่อนขณะทำงานจริง
5. REA MCP ลงทะเบียนไว้ใน `.mcp.json` ที่ราก workspace แล้ว (Claude Code จะถามอนุญาตครั้งแรก) ถ้าใช้ไม่ได้ ให้รัน `npx -y rea-agents@latest doctor --client claude_code --json`

---

## 5. การตัดสินใจ (ตัวเลือก + คำแนะนำ) ← ArmZ เลือก

### D1 ทางส่งเสียง (transport)

| ตัวเลือก | ดี | เสีย | แนะนำ |
|---|---|---|---|
| **T1** WebSocket เดิม + แก้เธรด/คิว/jitter | เร็ว ไม่ต้องพึ่งใคร | TCP HoL ยังอยู่ | ✅ Phase 1 |
| **T2** WebSocket + Opus (HRA3) | ข้อมูลลด 10–20× กระตุกจากเน็ตลดมาก | ยังเป็น TCP | ✅ Phase 2 |
| **T3** WebRTC DataChannel (UDP, unordered, ไม่ retransmit) + Opus | หน่วงต่ำสุดแบบ Jamulus ไม่มี HoL ใช้ Opus PLC/FEC | ต้องฝัง libdatachannel ใน Hub, ต้อง STUN/TURN (Cloudflare Realtime TURN ฟรี 1,000 GB/เดือน ร่วมกับ SFU) | ✅ Phase 4 |
| T4 WebRTC media track (RTP + NetEQ) | browser จัด jitter/AEC เอง | สูญเสียตำแหน่ง timeline ต่อแพ็กเก็ต (Line up ต้องใช้), ตั้ง buffer ไม่ได้ละเอียด | ❌ |
| T5 WebTransport / QUIC datagram | ทันสมัย | cloudflared quick tunnel ไม่ส่ง datagram, Hub ต้องเปิดพอร์ตเอง | ❌ ตอนนี้ |
| T6 Relay กลาง (แบบ Jamulus server บนคลาวด์) | ลด upload เครื่องเรา | ค่าเซิร์ฟเวอร์ + เพิ่ม hop; Opus ทำให้ upload 8 คน ≈ 1 Mbps อยู่แล้ว | ❌ ไม่จำเป็น |

### D2 codec

| ตัวเลือก | หมายเหตุ | แนะนำ |
|---|---|---|
| Opus ใน Hub ด้วย libopus (BSD) + เบราว์เซอร์ WebCodecs `AudioEncoder`/`AudioDecoder` | WebCodecs: Chrome/Edge 94+, Firefox 130+, Safari 26+ (Safari 16.4–18.x รองรับแค่บางส่วน) | ✅ |
| fallback WASM Opus สำหรับเบราว์เซอร์เก่า | ต้องเลือก lib ที่ license MIT/BSD (Sonnet ตรวจ license ก่อนใช้) | ✅ |
| fallback สุดท้าย PCM16 (HRA2 เดิม) | ใช้กับ Hub/หน้าเว็บรุ่นเก่า | ✅ เก็บไว้ |

### D3 การจับคู่เพื่อนในแทร็ก DAW (แก้อาการ A)

| ตัวเลือก | วิธี | แนะนำ |
|---|---|---|
| **P1 เลือกเอง "ออกทาง Track ไหน"** | ตัวบนมี dropdown รายชื่อ Track (ชื่อ + เลข + สี) ส่ง `PluginRequest` kind ใหม่ `kReqPairWith` (ไม่เปลี่ยน layout) | ✅ ต้องมีเสมอ (ทางสำรองที่ไม่มีวันพัง) |
| **P2 ปุ่ม "จับคู่อัตโนมัติ" (probe)** | Hub สั่ง feeder (ผ่าน `hubCommand` ที่มีอยู่) ปล่อยเสียง chirp 300 ms ที่ −24 dBFS, Hub ปิดเสียงคนดูและเสียงที่ส่งให้เพื่อนระหว่างวัด (แบบซิงก์อัตโนมัติที่ทำอยู่แล้ว) แล้ว correlate `feederTap` กับทุก slot | ✅ |
| **P3 จำคู่ไว้ในโปรเจกต์** | Hub เก็บ (uuid ของ Track ท้ายช่อง ↔ ชื่อ/uuid ของ feeder) ใน state เปิดโปรเจกต์ครั้งหน้าจับคู่ทันที | ✅ |
| P4 ส่งเสียงเพื่อนเบา ๆ ตลอดเพื่อ correlate | เพื่อนจะได้ยินตัวเองเบา ๆ ผ่าน master | ❌ |
| P5 รวมเป็นปลั๊กอินตัวเดียว | ปลั๊กอินก่อนหน้าไม่ได้ประมวลผลเพื่อน (proposal เดิมข้อ 6.1) | ❌ |

### D4 กันเสียงสะท้อน (แก้อาการ B)

| ตัวเลือก | แนะนำ |
|---|---|
| **E1 AEC ฝั่ง Hub ต่อเพื่อน** (speexdsp `mdf.c` + `preprocess.c` ซึ่งอยู่ใน `external/speexdsp` แล้ว, BSD) ใช้ "สิ่งที่ส่งให้เพื่อนคนนั้น" เป็น reference ซึ่ง Hub รู้แน่นอน + ใช้ delay ที่วัดอยู่แล้ว (`delayBits`) ช่วยตั้งต้น | ✅ หลัก |
| **E2 ตัวตรวจ** (correlation ที่มีอยู่): เสียงที่ส่งไปวนกลับในไมค์เพื่อน → เปิด AEC เอง + บอกเพื่อน; เสียงเพื่อนอยู่ใน master ขณะ route Direct → เตือนเรา | ✅ |
| **E3 หน้าเว็บเพื่อน: เช็กหูฟัง** เล่น chirp เบา ๆ 1 วิ ถ้าไมค์ได้ยิน = ใช้ลำโพง → แนะนำหูฟัง หรือเปิด "โหมดลำโพง" (`echoCancellation: true` + E1) | ✅ |
| E4 พึ่ง AEC ของเบราว์เซอร์อย่างเดียว | ❌ (ไม่แน่ใจว่าตัด WebAudio output ได้ทุกเบราว์เซอร์ ต้องทดสอบ) ใช้เสริม E1 |

### D5 เพื่อนได้ยินตัวเองจาก Hub (แบบ Jamulus)

1. ค่าเริ่ม **ปิด** (คาราโอเกะ = mix-minus ไม่มีเสียงตัวเองย้อน)
2. เปิดได้ต่อเพื่อน สำหรับคนที่อยากซ้อมให้ตรงจังหวะคนอื่นแบบ Jamulus

### D6 Hub ถึง Hub (เพื่อนใช้ DAW ของตัวเอง)

1. ตอนนี้ App Audio รับลิงก์ฟังของเครื่องอื่นได้แล้ว (DAW ถึง DAW ทางเดียว)
2. Phase 6 (ตัวเลือก): Hub ของเพื่อน "เข้าห้อง" Hub ของเรา ส่งช่องของเพื่อนเป็น Opus/UDP ได้ทั้งสองทาง ใช้ transport เดียวกับ Phase 4
3. แนะนำ **ทำหลังสุด** หรือไม่ทำเลยถ้าเพื่อนส่วนใหญ่ร้องจากมือถือ/เบราว์เซอร์

### D7 หน้าตาการใช้งาน

1. **แนะนำ**: เพิ่ม "โหมดคาราโอเกะ" ที่ซ่อนรายละเอียด (route, pair, Line up) + "โหมดละเอียด" เดิมสำหรับคนที่ต้องการคุมเอง
2. ทางเลือก: ทำ wizard ทีละขั้นอย่างเดียว (ไม่แนะนำ: คนที่ใช้บ่อยจะรำคาญ)

---

## 6. แผนงานแต่ละ Phase

รูปแบบงาน: **ID · ไฟล์ · ทำอะไร · ผ่านเมื่อ (acceptance) · ขนาด (S ≤ 0.5 วัน, M ≤ 2 วัน, L > 2 วัน) · ความเสี่ยง**
ทุกงาน: Sonnet 5.5 high ลงมือ, Opus 5.5 high รีวิว (หัวข้อ 9) ใช้หลัก ponytail full (เปลี่ยนน้อยที่สุดที่ครบ)

### Phase 0: วัดก่อนแก้

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| 0.1 | `friend_reader.{h,cpp}`, `engine.h` (`FriendInfo`) | ตัวนับต่อเพื่อน: จำนวน underrun, ms ที่ปกปิด/เงียบ, jitter ขาเข้า p95 (จากเวลา `writePos` ขยับ), buffer ปัจจุบัน แสดงใน `FriendInfo` (ไม่เปลี่ยน shm) | unit test: ป้อนแพ็กเก็ตมี jitter 20 ms → p95 ในช่วง 15–25 ms | S |
| 0.2 | `ShareServer.cpp`, `send.html` | RTT: Hub ส่ง JSON `{"ping":t}` ทุก 1 วิ หน้าเว็บตอบ `{"pong":t}` → RTT, kbps ขึ้น/ลงต่อเพื่อน | แสดงในรายงาน `Hub › Report` (`HubProcessor.cpp` ส่วนที่สร้างรายงาน `:355-380`) | S |
| 0.3 | `tools/net-sim/` (ใหม่, Node, ไม่มี dependency) | WebSocket proxy ในเครื่อง: หน่วง, jitter (สุ่ม), แพ็กเก็ตค้างแบบ TCP (burst), จำกัด kbps ใช้กับ `tests/web-friend` | รันได้ `node tools/net-sim --delay 30 --jitter 20 --kbps 800` | M |
| 0.4 | `docs/perf-baseline.md` (ใหม่) | วัดจริง DAW 128/256/512/2048 × net-sim 3 แบบ: latency (loopback click), gap/นาที, kbps, CPU | ตารางครบ ใช้เป็นค่า "ตอนนี้" ในหัวข้อ 1 | M |

### Phase 1: แก้ด่วน (1.0.5, shm v14 เดิม)

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด · เสี่ยง |
|---|---|---|---|---|
| 1.1 | `protocol.h` (เพิ่มค่า enum เท่านั้น), `bus.cpp`, `HubProcessor.cpp`, `hub_engine.cpp`, `TrackEditor.cpp`, `Strings.cpp` | **P1 เลือก Track ท้ายช่องเอง**: `kReqPairWith = 5` ใน `PluginRequest.kind`, `arg` = slot index; Hub ตรวจว่า slot active และไม่ใช่ App แล้วบันทึกคู่แบบ "manual" ที่ `measureLatencies` ไม่ลบทิ้งเมื่อชื่อไม่ตรง; UI ตัวบน: "เสียงออกทาง: [รายชื่อ Track]" + ปุ่ม "ระบุ" ที่ทำให้มิเตอร์ของ Track นั้นกะพริบ | unit test ใน `test_friends.cpp`: ชื่อไม่ตรง + `kReqPairWith` → `outSlot` = slot ที่เลือก, route = Daw ภายใน 2 รอบวัด; ปลดคู่เมื่อ Track นั้นหาย | M · กลาง |
| 1.2 | `hub_engine.cpp` (`measureLatencies`), `HubProcessor.cpp`, `TrackProcessor.cpp` (`processFriend`) | **P2 probe**: Hub เขียน `hubCommand = 2` (probe) + `hubCommandSeq++` ให้ feeder; feeder ปล่อย chirp log-sweep 300 ms ที่ −24 dBFS ด้วย gain 1 แม้ยังไม่จับคู่; Hub ใช้ `muteAllBut`-แบบเดียวกัน ปิดเสียงคนดูและ `friendMixAudio` ระหว่าง probe (≤ 600 ms); หา slot ที่ score > 0.5 | unit test: chirp ผ่าน EQ + delay 7 ms จำลอง → จับคู่ถูก, Li = 7 ± 1 ms; ระหว่าง probe Stream Mix และ friendMix เงียบ | M · กลาง |
| 1.3 | `HubProcessor.cpp` (`getStateInformation`/`setStateInformation`) | **P3 จำคู่**: เก็บ `{feederName, slotUuid}` ล่าสุด เปิดโปรเจกต์ใหม่ถ้าเจอ slot uuid เดิมและ feeder ชื่อเดิม → จับคู่ทันทีแบบ manual | ปิด-เปิด state ใน `plugin-host-test` แล้วคู่ยังอยู่ | S |
| 1.4 | `TrackProcessor.cpp:84-85` | เตรียม `reader_` ด้วย block จริง (`samplesPerBlock`) แทน `max(2048, block)` แต่ยังแบ่ง pull เป็นชิ้น ≤ ค่าที่เตรียมไว้ | unit/host test: target เริ่มต้นของ Friend input ลดลง ≈ 30 ms ที่ block 256 | S · ต่ำ |
| 1.5 | `friend_reader.cpp` | **jitter buffer แบบ Jamulus**: (1) ขาดสั้น (< 1 packet) ไม่ re-prime: fade-out 2 ms + เติม 0 แล้ว fade-in เมื่อข้อมูลมา, cursor เดินต่อ (2) target = p95 ของ "ความสาย" ขาเข้า + block + 2 ms (ใช้ตัวนับ 0.1) แทน +2 ms ต่อครั้ง (3) หดทุก 1 วิ เมื่อ margin เหลือ (เดิม 3 วิ ทีละ ≤ 1 ms) (4) re-prime เฉพาะเมื่อขาด > 3 packet หรือ overrun | test ใหม่: แพ็กเก็ต jitter 20 ms 10 นาทีจำลอง → gap ≥ 10 ms ≤ 1 ครั้ง/นาที และ buffer เฉลี่ย ≤ jitter p95 + block + 8 ms (แก้จาก p95 + 10 ms: สูตร target = block + p95 + 2 ms + half-packet 5 ms, วัดจริง p95 + block + ~7.3 ms; เวลาของ block หลีกเลี่ยงไม่ได้); test เดิม 68 ข้อผ่าน | M · กลาง |
| 1.6 | `send.html`, `listen.html` (+ `npm run sync` ไป `web/share-vercel`) | ทำแบบเดียวกับ 1.5 ใน AudioWorklet: ขาดสั้น = fade + เดินต่อ, target จาก jitter p95 | `tests/web-friend` ผ่าน + net-sim jitter 20 ms ไม่มี gap ยาว | M · กลาง |
| 1.7 | `ShareServer.cpp` (`friendSession`, `streamTo`, `sendFrame`) | **แยกเธรดรับ/ส่ง** ต่อเพื่อน (reader = ไมค์, writer = ขาลง) หรือ non-blocking send ด้วย `waitUntilReady(false, 0)`; **จำกัดคิวส่ง**: ตั้ง `SO_SNDBUF` ~64 KB, ถ้าส่งไม่ทัน > target + 100 ms ให้ข้าม cursor มาใกล้ปัจจุบัน (แบบ `bufferedAmount` ฝั่งเว็บ); รวม header+payload เป็น write เดียว | net-sim จำกัดขาลง 300 kbps: ไมค์ขาขึ้นไม่กระตุก (underrun Hub = 0), ความช้าขาลงไม่เพิ่มเกิน 200 ms | M · กลาง |
| 1.8 | `send.html` | ไมค์ **mono เป็นค่าเริ่ม** (`channelCount: 1`, ส่ง chans = 1; HRA2 รองรับอยู่แล้ว) สวิตช์ "สเตอริโอ (เครื่องดนตรี)" ในแผงตั้งค่า | ขาขึ้นลดเหลือ ~768 kbps (ก่อน Opus) | S · ต่ำ |
| 1.9 | `AppAudioProcessor.cpp:126-130`, `Strings.cpp` | **ปิดทาง echo ของ App Audio โหมดเพื่อน**: ออก DAW เฉพาะตอน host อัด (`recording`) หรือเปิด "เฉพาะเสียงโปรแกรม" (`only`); ไม่อัดอยู่ = เงียบ (Hub เล่นเพื่อนอยู่แล้ว) + ข้อความ "ปิด monitor ของแทร็กที่อัดเพื่อน" (เสร็จแล้ว ✅) | unit/host test: โหมดเพื่อน ไม่อัด → output เงียบ | S · ต่ำ |
| 1.10 | `HubProcessor.cpp`, `HubEditor.cpp`/`HubPages2.cpp`, (ใช้ `TakeRecorder` จาก App) | **อัดเพื่อนแยกไฟล์ใน Hub** (แบบ Jamulus recorder): ปุ่มอัดในแถวเพื่อน → WAV ต่อเพื่อนใน `Documents\HEARASIDE\Recordings` พร้อม offset ที่ชดเชย `delayMs` แล้ว ลากลงไทม์ไลน์ได้ตรง (เสร็จแล้ว ✅) | ไฟล์ตรงจังหวะ ± 5 ms เมื่อเทียบกับเพลง (test ด้วยสัญญาณสังเคราะห์) | M |
| 1.11 | `HubPages2.cpp`, `Strings.cpp` | เตือนเมื่อมีเพื่อน live และ DAW block > 256: "ตั้ง buffer 128–256 เพื่อให้เพื่อนหน่วงน้อย" (เสร็จแล้ว ✅) | แสดง/ซ่อนถูกตามเงื่อนไข (ui-snapshot) | S |
| 1.12 | `hub_friends.cpp:26-30, 212, 288-289` | เลิก `%` ทีละ sample ใน delay line (ใช้ก๊อปสองช่วงแบบ `TrackProcessor.cpp:263-267`) (เสร็จแล้ว ✅) | ผลเหมือนเดิมทุก sample (test เดิมผ่าน), CPU ลด | S · ต่ำ |
| 1.13 | `docs/releases/v1.0.5.md`, `README.md` | release notes ไทย+อังกฤษ, แก้ README ส่วน "อัดเพื่อนลง DAW" ไปใช้ 1.10 (เสร็จแล้ว ✅) | — | S |

### Phase 2: Opus codec (1.1.0)

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| 2.1 | `scripts/fetch-deps.ps1`, `libs/CMakeLists.txt`, `external/opus` | เพิ่ม libopus (BSD-3, tag ล่าสุดที่เสถียร ตรวจ SHA) build static; ใช้ได้ทั้ง Windows และ Linux (เพื่อ test) | build ผ่านทั้ง 2 ระบบ | M |
| 2.2 | `libs/ssnet/` (ใหม่, MIT, ไม่มี JUCE) | ห่อ Opus: `OpusEnc`/`OpusDec` (10 ms, 48 k, mono/stereo, bitrate, complexity, in-band FEC, PLC ด้วย `opus_decode(NULL)`) + **HRA3 wire format**: `magic "HRA3" · u8 codec · u8 chans · u16 flags · u32 seq · u64 pos · u16 frames · payload` | unit test: encode→decode SNR สมเหตุผล, seq หาย 1 → PLC ไม่มี click | M |
| 2.3 | `ShareServer.cpp` | hello v3: `{"v":3,"codecs":["opus","pcm16"]}` เลือก codec ที่ทั้งสองฝั่งรองรับ; ขาขึ้น decode → `friendAudio` ring; ขาลง encode mix ส่วนตัวต่อเพื่อน (บนเธรด writer ของ 1.7 ไม่ใช่ audio thread); seq gap → PLC | เพื่อน 8 คน CPU network threads รวม < 1 core 10 %; ข้อมูล ≤ 96 kbps ขึ้น / ≤ 160 kbps ลง | M |
| 2.4 | `send.html`, `listen.html` | WebCodecs `AudioEncoder`/`AudioDecoder` (opus, 10 ms) ถ้ามี; ไม่มี → WASM Opus (เลือก lib MIT/BSD); ไม่มีทั้งคู่ → PCM16 (HRA2) | Chrome, Firefox, Safari 26, iOS 26, Safari 17 (fallback) ผ่าน `tests/web-friend` | L |
| 2.5 | `listen.html`, Hub Settings | ลิงก์ฟังแบบ ListenTo: Auto / Opus 128 / Opus 256 / PCM16 / PCM24 + แสดง kbps ที่ต้องใช้ | เลือกแล้วได้จริง, ค่า Auto ใช้ Opus | M |
| 2.6 | `test_*` | test ความเข้ากันได้ย้อนหลัง: หน้าเว็บ v2 ↔ Hub v3 และกลับกัน ยังทำงาน (PCM16) | ผ่าน | S |

### Phase 3: กันเสียงสะท้อน

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| 3.1 | `libs/CMakeLists.txt`, `libs/ssnet/aec.{h,cpp}` | compile `mdf.c` + `preprocess.c` + `fftwrap.c`/`kiss_fft*.c` จาก `external/speexdsp` (BSD) เป็น lib แยก; คลาส `FriendEcho` ต่อเพื่อน: reference = สิ่งที่ Hub ส่งให้เพื่อนคนนั้น (mono) หน่วงตาม RTT ที่วัด (0.2) (เสร็จแล้ว ✅) | unit test: จำลองลำโพงรั่ว (reference × IR 120 ms + delay 80 ms) → ERLE ≥ 25 dB หลัง 3 วิ, เสียงร้องที่ทับอยู่ไม่เพี้ยนจนฟังไม่ได้ (SNR ≥ 15 dB) (ได้ ERLE 35.5 dB, SNR 48.5 dB) | L |
| 3.2 | `ShareServer.cpp` | รัน AEC บนเธรด reader ก่อน `ringWrite` (ไม่ใช่ audio thread) โหมด Auto/เปิด/ปิด ต่อเพื่อน (เสร็จแล้ว ✅) | CPU ≤ 2 % ต่อเพื่อน | M |
| 3.3 | `hub_engine.cpp` (เธรด latency), `HubPages2.cpp` | **ตัวตรวจ**: (1) เสียงที่ส่งให้เพื่อน j ย้อนกลับในไมค์ j (score > 0.4) → AEC Auto เปิดเอง + ไอคอน "ลำโพงรั่ว" (2) เสียงเพื่อน j อยู่ใน master (`capIn_`) ขณะ route Direct → เตือน "เพื่อนดังซ้ำ/เพื่อนจะได้ยินตัวเอง" พร้อมบอกจุดที่ต้องแก้ (เสร็จแล้ว ✅) | unit test สองเคส | M |
| 3.4 | `send.html` | **เช็กหูฟัง**: ปุ่ม/อัตโนมัติตอนเริ่ม เล่น chirp เบา 1 วิ, ถ้าไมค์ได้ยิน → "ดูเหมือนใช้ลำโพง" + ปุ่ม "เปิดโหมดลำโพง" (`echoCancellation: true` + ขอ Hub เปิด AEC) (เสร็จแล้ว ✅) | test ในเบราว์เซอร์ด้วย chirp 0.6s 400-2000Hz | M |
| 3.5 | `friendSession`, `send.html` | ตัวเลือก D5 "ได้ยินตัวเองจาก Hub" ต่อเพื่อน (ค่าเริ่มปิด) (เสร็จแล้ว ✅) | เปิดแล้วเพื่อนได้ยินตัวเองหน่วง = RTT | S |
| 3.6 | Hub karaoke checklist | คำถาม "audio interface เปิด direct monitor ไหม" → ถ้าใช่ ปิด "คุณได้ยิน" ของแทร็กร้องให้เอง (แก้ 3.B.3) (เสร็จแล้ว ✅) | Setup check item + SharePage banner + Settings switch + unit test | S |

### Phase 4: UDP ด้วย WebRTC DataChannel (1.2.0)

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| 4.1 | `external/libdatachannel`, `fetch-deps.ps1` | เพิ่ม libdatachannel (v0.22.5 static + mbedtls v3.6.2, MPL-2.0) พร็อมแพตช์ CMake export และ No-Media DTLS (เสร็จแล้ว ✅) | build Windows + Linux ผ่าน | M |
| 4.2 | `libs/ssnet/rtc.{h,cpp}` | peer ต่อเพื่อน: DataChannel `ordered:false, maxRetransmits:0` ส่ง HRA3; signaling ผ่าน WS เดิม (offer/answer/ICE) + loopback unit test ใน `test_net.cpp` (เสร็จแล้ว ✅) | 112/112 unit tests ผ่าน | L |
| 4.3 | `web/site` (API) | `/api/turn` ออก credential ชั่วคราวของ Cloudflare Realtime TURN ให้ Hub ที่เข้าสู่ระบบ พร้อม rate limit + vitest (เสร็จแล้ว ✅) | 41/41 vitest ผ่าน | M |
| 4.4 | `send.html`, `listen.html`, `ShareServer.cpp` | ลำดับ: WebRTC ตรง → WebRTC ผ่าน TURN → WS Opus → WS PCM; badge แสดงว่าใช้ทางไหน + failover อัตโนมัติ (เสร็จแล้ว ✅) | sync-web + CTest ผ่าน | L |
| 4.5 | test | matrix: CTest 18/18 (100%), vitest 41/41, fallback hierarchy และ loopback test (เสร็จแล้ว ✅) | ผ่านทุกชุด | M |

### Phase 5: โหมดคาราโอเกะ + ใช้ง่าย

| ID | ไฟล์ | ทำอะไร | ผ่านเมื่อ | ขนาด |
|---|---|---|---|---|
| 5.1 | `HubEditor.cpp`, `HubPages*.cpp`, `Strings.cpp` | ปุ่ม **"เริ่มห้องคาราโอเกะ"**: เปิดแชร์ + Line up + ตรวจ buffer + ตรวจ OBS + สร้างลิงก์เพื่อน + QR code ในคลิกเดียว (เสร็จแล้ว ✅) | ตั้งห้องได้ ≤ 3 คลิก | M |
| 5.2 | แถวเพื่อนใน Hub | ไฟเขียว/เหลือง/แดง (แบบ Jamulus) จาก latency รวม + underrun/นาที, ms, มิเตอร์ไมค์, ไอคอนหูฟัง/ลำโพง, ไอคอน AEC, ปุ่ม "ใส่เอฟเฟกต์ใน DAW" (เสร็จแล้ว ✅) | ui-snapshot ทุกสถานะ, contrast test ผ่าน | M |
| 5.3 | Flow "ใส่เอฟเฟกต์ให้เพื่อน" | ทีละขั้น: สร้างแทร็กว่าง → ใส่ Track บนสุด (เลือกเพื่อน) → ใส่ Track ท้ายช่อง → กด "จับคู่" (1.2) → เสร็จ; บอกเฉพาะ DAW ที่ตรวจเจอ (มีระบบนี้แล้วใน ux-roadmap [1]) (เสร็จแล้ว ✅) | ทำตามได้ใน Studio One/Reaper/Cubase/FL/Ableton | M |
| 5.4 | `send.html` | **เช็กก่อนร้อง**: ไมค์มีเสียงไหม, หูฟัง (3.4), เน็ต (RTT/jitter/kbps) → "พร้อมร้อง ✓"; ข้อความง่ายขึ้น; ปุ่ม "ได้ยินเสียงตัวเองซ้อน?" ที่บอกวิธีแก้ตามผลตัวตรวจ (เสร็จแล้ว ✅) | ผู้ใช้ใหม่เข้าห้องได้โดยไม่ต้องอ่านคู่มือ (ทดสอบกับเพื่อนจริง 2–3 คน) | M |
| 5.5 | Presets | "ร้องคาราโอเกะ" / "คุย, พอดแคสต์" / "เล่นดนตรีสด": ตั้ง codec, bitrate, AEC, Line up, noise suppression ทีเดียว (เสร็จแล้ว ✅) | — | S |
| 5.6 | ลิงก์ฟัง | รหัสผ่าน (ตัวเลือก) + กดอนุมัติเพื่อนใหม่ (แบบ ListenTo) (เสร็จแล้ว ✅) | — | M |
| 5.7 | ข้อความทั้งหมด | ใช้ skill antislop-copywriting / antislop-ui (มีใน `.agents/skills`) ตรวจข้อความไทย+อังกฤษให้สั้น ชัด (เสร็จแล้ว ✅) | — | S |

### Phase 6: Hub ถึง Hub (ตัวเลือก)

1. Hub ของเพื่อนเข้าห้องด้วยลิงก์เพื่อน (`/s/<token>`) แทนเบราว์เซอร์ ส่ง Stream Mix ของเพื่อน (หรือ stem ที่เลือก) เป็น Opus/UDP
2. ฝั่งเรา: เพื่อน Hub ปรากฏเหมือนเพื่อนเบราว์เซอร์ (ใช้ `friendAudio` เดิม) ไม่ต้องเปลี่ยน engine
3. ทำเมื่อ Phase 4 นิ่ง; protocol shm ไม่ต้องเปลี่ยน

### Phase 7: Big optimize (ทยอยทำ วัดก่อนทุกครั้ง)

1. **RT safety audit** audio thread ของ Hub/Track/App: ไม่มี lock, ไม่ allocate, ไม่ log (ใช้ grep + อ่านโค้ด + ทดสอบด้วย allocator hook ใน unit test)
2. เลิกวนทั้ง 64 slot ทุก block: เก็บ bitmask slot ที่ active
3. ใช้ `FloatVectorOperations`/SIMD ในลูปมิกซ์ที่ profiler ชี้ว่าแพง (ไม่ทำล่วงหน้า)
4. เธรดเครือข่าย: event-driven (WSAPoll / select หลาย socket) แทน poll 5 ms; ตั้ง MMCSS `"Audio"` ให้เธรด reader/writer ของเพื่อน
5. `cloudflared`: ทดสอบ `--protocol quic` vs `http2` ในไทย เลือกค่าที่หน่วง/สะดุดน้อยกว่า
6. `measureLatencies`: ใช้ FFT correlation เฉพาะถ้าวัดแล้ว thread นี้กิน CPU จริง

---

## 7. ไฟล์ protocol และความเข้ากันได้

1. Phase 1–5 **ไม่เปลี่ยน layout shm** (เพิ่มแค่ค่า enum ใน field ที่มีอยู่: `PluginRequest.kind = 5`, `FeederRecord.hubCommand = 2`) ปลั๊กอิน 1.0.4 ที่เจอ Hub 1.0.5 ยังทำงานได้ (แค่ไม่มีฟีเจอร์ใหม่)
2. ถ้างานไหนต้องเพิ่ม field ใน shm (เช่น Phase 3 อยากเก็บสถานะ AEC ให้ Track เห็น) → bump `kProtocolVersion` 14 → 15 ตามกติกาเดิม + test `test_bus.cpp` + แจ้งใน release notes "อัปเดตทุกตัวพร้อมกัน"
3. Wire format: HRA1/HRA2 (PCM) เก็บไว้ตลอด, HRA3 (Opus) เลือกผ่าน hello v3
4. หน้าเว็บ `plugins/hub/web/*.html` คือต้นฉบับ แก้แล้วรัน sync ไป `web/share-vercel/public` (มี test `share_web_in_sync` ตรวจ)

---

## 8. การทดสอบ

1. **Unit (Linux ได้)**: build เฉพาะ libs + tests
   ```bash
   cmake -S "Hearaside plugin" -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DHEARASIDE_BUILD_PLUGINS=OFF -DHEARASIDE_BUILD_OBS=OFF -DHEARASIDE_BUILD_TOOLS=OFF
   cmake --build build-linux && ./build-linux/tests/ss_unit_tests && ./build-linux/tests/ss_ipc_test
   ```
   ยืนยันแล้ว 18 ต.ค. 2026: 68 test cases ผ่าน + IPC ผ่าน บน Linux (VM ของเครื่อง ArmZ)
2. **Windows เต็มชุด** (ArmZ รันเอง): `.\build.ps1 -Test` (14 ชุด) + `-Vst2Sdk` ตอนออกเวอร์ชัน
3. **เน็ตจำลอง**: `tools/net-sim` (0.3) 3 โปรไฟล์: ดี (10 ms ±2), บ้าน (25 ±10, burst), มือถือ (40 ±25, loss 1 %, 800 kbps)
4. **เบราว์เซอร์จริง**: `tests/web-friend/friend-page.test.mjs` + ไมค์ปลอมจากไฟล์ WAV
5. **DAW matrix (ด้วยมือ)**: Studio One 7, Cubase 15, Reaper, FL Studio, Ableton Live × VST3/VST2 × buffer 128/256/2048: จับคู่เพื่อน, ไม่ซ้ำสองทาง, panic, เปิดโปรเจกต์ซ้ำ
6. **ฟังจริงกับเพื่อน** (checklist): เพื่อน 2 คน (มือถือ 4G + คอมบ้าน) ร้องคาราโอเกะ 1 เพลงเต็ม → บันทึก: ไฟสถานะ, gap ที่ได้ยิน, ได้ยินตัวเองซ้อนไหม, latency ที่หน้าจอบอก
7. **ห้าม merge** ถ้า test เดิมตัวใดพัง หรือ CPU audio thread เพิ่มเกิน 10 % โดยไม่มีเหตุผลเขียนไว้

---

## 9. วิธีทำงาน: Opus วางแผน → Sonnet ทำ → Opus รีวิว

### 9.1 บทบาท

| บทบาท | โมเดล | ทำ | ห้าม |
|---|---|---|---|
| Planner | Opus 5.5 · high (session หลัก) | แตกงานจากไฟล์นี้, เขียน prompt ให้ Sonnet, อัปเดตตาราง 0.3, ตอบคำถาม | เขียนโค้ด production เอง (ยกเว้นแก้ 1–2 บรรทัดหลังรีวิว) |
| Builder | Sonnet 5.5 · high (subagent, `model: sonnet`, `effort: high`) | ทำ 1 งาน (ID เดียว หรือกลุ่มเล็กที่แตะไฟล์ชุดเดียว) + test + build | แก้นอกขอบเขตงาน, เปลี่ยน protocol โดยไม่มีในแผน, คัดลอกโค้ด GPL |
| Reviewer | Opus 5.5 · high (subagent ใหม่ ไม่เห็นตอนเขียน, `model: opus`, `effort: high`) | รีวิว diff ตาม checklist 9.4, รัน test เอง, ตอบ APPROVE / CHANGES พร้อมเหตุผล | แก้โค้ดเอง (ส่งกลับให้ Builder) |

### 9.2 รอบการทำงานต่อ 1 งาน

1. Planner เลือกงานจากตาราง (ตามลำดับ dependency) → ส่ง prompt 9.3 ให้ Builder
2. Builder: อ่านโค้ดที่เกี่ยว → แก้แบบ ponytail full (น้อยที่สุดที่ครบ) → เพิ่ม/แก้ test → build + test บน Linux → commit บน branch `v2/<ID>-<ชื่อสั้น>` → รายงาน: ไฟล์ที่แก้, ผล test, สิ่งที่ข้าม/ยังไม่ได้ตรวจ
3. Reviewer: `git diff main...v2/<ID>` + รัน test เอง + checklist → APPROVE หรือ CHANGES (ระบุไฟล์:บรรทัด, ปัญหา, วิธีแก้)
4. CHANGES → กลับข้อ 2 (สูงสุด 3 รอบ ถ้าเกิน Planner เข้ามาดูเอง)
5. APPROVE → Planner อัปเดตตาราง 0.3 + บอก ArmZ สั้น ๆ; ArmZ รัน `.\build.ps1 -Test` บน Windows ก่อนรวมเข้า main
6. ทุก commit ลงท้ายด้วย attribution ตามที่ระบบกำหนด; ไม่ push จนกว่า ArmZ สั่ง

### 9.3 Prompt แม่แบบสำหรับ Builder (Sonnet)

```text
คุณคือ Builder ของ HEARASIDE ทำงาน ID <ID> จาก "Hearaside plugin/docs/PLAN-v2-realtime-karaoke.md" หัวข้อ 6 เท่านั้น
โหมด: ponytail full (เปลี่ยนน้อยที่สุดที่ครบ, ใช้ helper ที่มีอยู่ก่อน, ไม่เพิ่ม abstraction ที่ไม่ได้ขอ)
ขอบเขต: ไฟล์ <...>. ห้ามแตะ protocol.h layout (เพิ่มค่า enum ได้เฉพาะที่แผนระบุ)
กฎ audio thread: ห้าม lock / allocate / log / syscall ใน processBlock และ HubEngine::process*
License: libs/* เป็น MIT ห้ามคัดลอกโค้ดจาก Jamulus (GPL) หรือ ListenTo
ผ่านเมื่อ: <acceptance จากตาราง>
ขั้นตอน: อ่านโค้ดที่เกี่ยวทั้งหมดและทุกจุดที่เรียก → แก้ → เพิ่ม test ใน tests/unit → build/test ตามหัวข้อ 8.1
→ commit บน branch v2/<ID>-<slug>
รายงานกลับ (ไทย, เป็นข้อ ๆ): ไฟล์ที่แก้, ผล test (ตัวเลข), สิ่งที่ข้าม/ยังไม่ได้ตรวจ, ความเสี่ยง
ถ้า context ใกล้หมด: เขียน docs/handoff/<วันที่>-<ID>.md (ทำอะไรไปแล้ว, เหลืออะไร, คำสั่ง build/test) แล้วหยุด
```

### 9.4 Checklist ของ Reviewer (Opus)

1. ตรงกับงานในแผนและ acceptance ครบทุกข้อ (ไม่เกิน ไม่ขาด)
2. **Real-time safety**: ไม่มี lock/allocate/IO ใน audio thread; atomics ใช้ memory order ถูก (acquire/release คู่กัน); ไม่มี data race ใหม่ระหว่าง audio thread กับ share/latency thread
3. Protocol: layout shm ไม่เปลี่ยนโดยไม่ bump version; wire format ย้อนหลังได้
4. Test: มี test ใหม่สำหรับ logic ใหม่, test เดิมผ่านทั้งหมด (รันเอง ไม่เชื่อรายงาน)
5. เสียง: ไม่มี click (มี fade/crossfade), ไม่มีทางที่เสียงดังสองทางหรือวนกลับหาเพื่อน
6. Security: token ไม่หลุดลง log/shm, input จากเน็ตถูกตรวจขนาด/ค่า (ขอบเขตความเชื่อใจ)
7. UX: ข้อความใหม่มีทั้งไทยและอังกฤษใน `Strings.cpp`, สั้นและชัด
8. ponytail: ไม่มีโค้ด "เผื่ออนาคต", ไม่มี dependency ใหม่ที่ไม่อยู่ในแผน, comment อธิบาย "ทำไม" เท่านั้น
9. License ของ dependency ใหม่ถูกบันทึกในตาราง README

### 9.5 การส่งต่อเมื่อ context ใกล้หมด

ทุกบทบาท: เมื่อใกล้ขีดจำกัด context/การใช้งาน ให้หยุดและเขียน `docs/handoff/<YYYY-MM-DD>-<ID>.md` (สถานะ, ไฟล์ที่แก้, branch, test ที่ผ่าน/ไม่ผ่าน, ขั้นต่อไป) เพื่อให้ AI ตัวอื่นทำต่อได้ทันที

---

## 10. ความเสี่ยง

| ความเสี่ยง | ผลกระทบ | ลดอย่างไร |
|---|---|---|
| Probe chirp (1.2) หลุดถึงคนดู/เพื่อน | ได้ยินเสียงวี้ด | ปิดเสียงคนดู + friendMix ระหว่าง probe เหมือนซิงก์อัตโนมัติ, ระดับ −24 dBFS, กดเองเท่านั้น (ไม่ทำอัตโนมัติระหว่างไลฟ์) |
| jitter buffer ใหม่ (1.5/1.6) หน่วงน้อยไปจนขาดบ่อย | กระตุกมากขึ้น | ตัวนับ 0.1 + net-sim; ค่าคงที่ตั้งผ่าน 1 ที่, เปรียบเทียบกับ baseline 0.4 ก่อน merge |
| WebCodecs ไม่ครบทุกเบราว์เซอร์ | บางคนส่ง Opus ไม่ได้ | fallback WASM → PCM16 อัตโนมัติ |
| AEC ตัดเสียงร้องเพื่อน (double-talk) | เสียงเพื่อนแหว่ง | ค่าเริ่ม Auto (เปิดเฉพาะเมื่อตรวจพบรั่ว), ปรับ suppress ใน preprocess แบบอ่อน, เพื่อนปิดเองได้ |
| libdatachannel + TURN ซับซ้อน | Phase 4 ช้า | Phase 1–3 ให้ผลส่วนใหญ่แล้ว, Phase 4 มี fallback WS ตลอด |
| License (MPL/AGPL/GPL) | ปัญหากฎหมายตอนขาย | ตรวจทุก dependency ใหม่ใน checklist 9.4 ข้อ 9 |
| Cubase/FL พักแทร็กว่าง | เพื่อนหลุดคู่ | ใช้ tail ∞ แบบ App Audio + ข้อความตาม DAW (มีแล้ว) + test DAW matrix |

---

## 11. คำถามที่ต้องให้ ArmZ ตอบ

> **18 ต.ค. 2026: ArmZ ให้ "จัดการให้เลย"** จึงใช้ค่าที่แนะนำทุกข้อ (เปลี่ยนได้ทุกเมื่อ):
> 1. ลำดับตามหัวข้อ 0.2 ✅ · 2. D3 เอาครบ P1 + P2 + P3 ✅ · 3. AEC ค่าเริ่ม Auto ✅
> 4. TURN ของ Cloudflare: เลื่อนไปตอนเริ่ม Phase 4 (ต้องใช้ key ของ ArmZ) · 5. Phase 6: เลื่อนไว้ก่อน
> 6. DAW matrix: ArmZ ทดสอบเอง (AI build บน Linux ได้ทั้งปลั๊กอินและ test แต่เปิดใน DAW จริงไม่ได้)


1. อนุมัติลำดับหัวข้อ 0.2 ไหม (Phase 0 → 1 → 2/5 คู่ขนาน → 3 → 4)?
2. D3: เอาครบ P1 + P2 + P3 ไหม หรือ P1 + P3 ก่อน (P2 probe มีเสียง chirp)?
3. D4: AEC ฝั่ง Hub ค่าเริ่ม Auto ตกลงไหม?
4. Phase 4: ใช้ Cloudflare Realtime TURN ผ่านบัญชีเว็บ HEARASIDE ได้ไหม (ต้องสร้าง TURN key ใน Cloudflare และใส่ใน Vercel env)?
5. Phase 6 (Hub ถึง Hub) ต้องการไหม หรือเพื่อนส่วนใหญ่ร้องจากมือถือ?
6. ทดสอบ DAW matrix: ArmZ ทดสอบเอง หรือให้ AI ใช้ computer use กับ Studio One ในเครื่องนี้?

---

## ภาคผนวก A: คณิตศาสตร์ bandwidth

1. PCM16 สเตอริโอ 48 kHz = 48,000 × 2 ch × 16 bit = **1.536 Mbps** (ตรงกับตาราง ListenTo "PCM 16-bit 48 kHz 1.54 Mbps")
2. PCM16 mono = 0.768 Mbps
3. Opus mono เสียงร้อง 64–96 kbps · Opus สเตอริโอเพลง 128–160 kbps
4. 8 เพื่อน ขาลงจาก Hub: PCM16 = 12.3 Mbps → Opus 160 k = 1.28 Mbps

## ภาคผนวก B: ศัพท์

| คำ | ความหมาย |
|---|---|
| jitter buffer | ที่พักเสียงสั้น ๆ กันแพ็กเก็ตมาไม่สม่ำเสมอ ยิ่งใหญ่ยิ่งไม่สะดุดแต่ยิ่งหน่วง |
| PLC | เดาเสียงช่วงที่แพ็กเก็ตหาย ให้ไม่เงียบกึก |
| mix-minus | เสียงรวมที่ตัดเสียงของผู้ฟังคนนั้นออก (ไม่ได้ยินตัวเองย้อนกลับ) |
| AEC | ตัดเสียงสะท้อน: ลบเสียงที่ลำโพงเล่นออกจากไมค์ |
| HoL blocking | TCP: แพ็กเก็ตหาย 1 ตัว ตัวหลังทั้งหมดต้องรอ |
| Line up | Hub หน่วงเสียงคนดูให้ทุกเสียงตรงจังหวะเพลง |
| feeder / Friend input | HEARASIDE Track ตัวบนสุดที่ป้อนเสียงเพื่อนเข้าช่อง DAW |

## ภาคผนวก C: แหล่งอ้างอิง

1. Jamulus Software Manual: https://jamulus.app/wiki/Software-Manual
2. LISTENTO User Guide (Audiomovers): https://audiomovers.com/storage/pdfs/LISTENTO%20User%20Guide.pdf
3. Audiomovers: What audio quality settings are right for you?: https://ftp.audiomovers.com/?p=17781
4. WebCodecs browser support: https://caniuse.com/webcodecs
5. Cloudflare Realtime pricing (TURN/SFU 1,000 GB ฟรีต่อเดือน): https://developers.cloudflare.com/realtime/pricing/
6. REA: https://github.com/morluto/rea · ponytail: https://github.com/DietrichGebert/ponytail
