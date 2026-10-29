# HEARASIDE: แผนย้ายจาก C++ / JUCE ไป Rust

> เขียนโดย Opus 5.5 · 29 ต.ค. 2026 · จากโค้ดบน `main` ที่ `7f4eccc` (1.0.10, `kProtocolVersion = 14`)
> แผนนี้ใช้คู่กับ [PLAN-v2-realtime-karaoke.md](PLAN-v2-realtime-karaoke.md) และ [PLAN-karaoke-exe.md](PLAN-karaoke-exe.md): เป้าคือ **ได้ของเดิมทุกอย่างใน Rust** ไม่ใช่เพิ่มฟีเจอร์ใหม่
> ลำดับการทำงานเหมือนแผนหลัก: **Opus 5.5 high วางแผน → Sonnet 5.5 high ลงมือ → Opus 5.5 high รีวิว** (หัวข้อ 10) · โหมด ponytail full
> สถานะงานอยู่ในตารางหัวข้อ 0.4 ที่เดียว แก้ทุกครั้งที่ปิดงาน

---

## 0. สรุปสั้น

### 0.1 คำแนะนำหลัก

1. **ย้ายทีละชั้น ไม่ rewrite ทีเดียว** (แบบ strangler): ทำ Rust ไว้ข้าง ๆ โค้ดเดิมในโฟลเดอร์ `rust/` แล้วเปลี่ยนทีละส่วน โดยของที่ส่งให้ผู้ใช้ต้องใช้ได้ทุกช่วง
2. **เริ่มจากล่างขึ้นบน**: `libs/*` (ไม่มี JUCE, มี test เยอะ, ย้ายง่ายสุด) → network → OBS source → HAS Karaoke → ปลั๊กอิน Track → App Audio → Hub (ใหญ่สุด ทำท้ายสุด)
3. **ห้ามเปลี่ยน shared memory (v14) และรูปแบบ wire ระหว่างย้าย**: ช่วงกลางทาง ตัว C++ กับตัว Rust ต้องคุยกันได้ในเครื่องเดียวกัน (เช่น Hub ยังเป็น C++ แต่ OBS source เป็น Rust แล้ว)
4. **โปรเจกต์ DAW เก่าต้องเปิดได้เหมือนเดิม 100 %**: VST3 class ID, parameter ID และรูปแบบ state ต้องตรงกับ 1.0.10 ทุกไบต์ที่สำคัญ (หัวข้อ 3) ข้อนี้คือความเสี่ยงอันดับ 1 ของทั้งแผน
5. **มีจุดตัดสินใจ 3 จุด (G0, G1, G2)** ถ้าถึง G1 แล้วพบว่า UI / host ใน Rust ยังไม่พร้อม หยุดที่ "แกนเป็น Rust, เปลือกเป็น JUCE" ได้เลย (hybrid ถาวร) โดยไม่เสียงานที่ทำไป
6. **ใช้โค้ด C ที่มีอยู่แล้วต่อ** (libopus, speexdsp) ผ่าน `cc` crate ก่อน เพื่อให้เสียงตรงกับของเดิม ค่อยพิจารณาเปลี่ยนเป็น Rust ล้วนทีหลัง

### 0.2 ได้อะไร เสียอะไร

| เรื่อง | ได้ | ต้นทุน / ความเสี่ยง |
|---|---|---|
| ความปลอดภัยของหน่วยความจำ | โค้ด network (ShareServer, ห้องเพื่อน, REST, WebRTC) รับข้อมูลจากเน็ต ถ้าเป็น Rust บั๊กแบบ buffer overflow / use-after-free หายไปเกือบหมด | ต้องเขียน `unsafe` ในส่วน shm, COM (WASAPI / ASIO), FFI และต้องรีวิวละเอียด |
| เธรด / data race | คอมไพเลอร์บังคับให้แบ่งข้อมูลระหว่างเธรดถูกต้อง (ตอนนี้มีหลายเธรด: audio, message, ShareServer, LatencyMeter, VirtualMic) | ต้องคิดเรื่อง ownership ใหม่ บางส่วนเขียนยากกว่า C++ |
| เครื่องมือ | `cargo test`, `clippy`, `cargo deny` (ตรวจ license), build เร็วกว่า CMake + JUCE | ต้องดูแล 2 build system ระหว่างย้าย (CMake + Cargo) |
| ความเข้ากันได้กับ DAW | — | JUCE แก้ปัญหา host มาเป็นสิบปี framework ของ Rust (nice-plug) ยังใหม่และ "experimental" เราเคยต้อง patch JUCE เอง 2 จุด (หัวข้อ 3.3) ต้องทำซ้ำใน Rust |
| UI | ได้เลือก UI framework ที่รองรับภาษาไทยดี | **งานใหญ่ที่สุด**: `plugins/common/ui` + หน้า Hub รวม ~15,000 บรรทัด ต้องเขียนใหม่ทั้งหมด |
| VST2 | — | framework ของ Rust ไม่มี VST2 ต้องตัดสินใจ (D3) |
| CLAP | ได้ฟรี (Reaper, Bitwig, Studio One, FL รองรับ) | เป็นตัวเลือก ไม่บังคับ |

### 0.3 ลำดับ Phase

1. **R0** เตรียมของ: เก็บ "ลายนิ้วมือ" ของ 1.0.10 (ID, state, layout, เสียงอ้างอิง) + spike framework / UI → **G0** ArmZ เลือก D1–D8
2. **R1** workspace + `hs-bus` (shared memory) ข้ามภาษาได้
3. **R2** `hs-dsp` + `hs-engine` + สะพาน C ABI ให้ปลั๊กอิน JUCE ใช้แกน Rust → **G1** ออกรุ่นทดสอบ "แกน Rust" แล้วตัดสินใจไปต่อหรือหยุด
4. **R3** network (wire, Opus, AEC, WebRTC, ShareServer, REST, บัญชี) ทำคู่ขนานกับ R2 ได้
5. **R4** OBS source เป็น Rust (ของชิ้นแรกที่ส่งผู้ใช้)
6. **R5** HAS Karaoke เป็น Rust (ที่ทดลอง UI framework กับของจริงโดยไม่มีเรื่อง DAW มาเกี่ยว)
7. **R6** ปลั๊กอิน Track → **G2** ทดสอบกับโปรเจกต์จริงทุก DAW
8. **R7** App Audio · **R8** Hub
9. **R9** build / installer / ลบ C++

### 0.4 สถานะ (อัปเดตทุกครั้งที่ปิดงาน)

| Phase | ชื่อ | ขึ้นกับ | ขนาด | สถานะ |
|---|---|---|---|---|
| R0 | ลายนิ้วมือ 1.0.10 + spike | — | M | 🟨 R0.1 ครบ; golden C++ source และ toolchain ผ่านในเครื่อง; เหลือ spike/CI |
| G0 | ArmZ เลือก D1–D8 | R0 | — | ⬜ |
| R1 | workspace + `hs-bus` | G0 | M | ⬜ |
| R2 | `hs-dsp` + `hs-engine` + C ABI (hybrid) | R1 | L | ⬜ |
| G1 | รุ่นทดสอบแกน Rust: ไปต่อ / หยุดที่ hybrid | R2 | — | ⬜ |
| R3 | `hs-net` + เซิร์ฟเวอร์ | R1 | L | ⬜ |
| R4 | OBS source (Rust) | R2 | S | ⬜ |
| R5 | HAS Karaoke (Rust) + `hs-ui` | R2, R3, G0 | XL | ⬜ |
| R6 | HEARASIDE Track (Rust) | R2, R5 (`hs-ui`) | L | ⬜ |
| G2 | host matrix กับโปรเจกต์เก่า | R6 | — | ⬜ |
| R7 | HEARASIDE App Audio (Rust) | G2 | M | ⬜ |
| R8 | HEARASIDE Hub (Rust) | G2, R3 | XL | ⬜ |
| R9 | build, installer, ลบ C++ | R4–R8 | M | ⬜ |

ขนาด: S ≈ 1–3 งานของ Sonnet · M ≈ 4–8 · L ≈ 9–15 · XL ≈ 16+ (1 งาน = 1 branch ที่รีวิวจบได้ในรอบเดียว)

ความคืบหน้า 29 ต.ค. 2026:

- **R0.1 ✅ ตรวจในเครื่อง Windows แล้ว**: layout v14 ครบ 16 struct / 157 ฟิลด์; `hs-compat-dump` อ่าน factory/class ID/parameter ID จาก VST3 จริง (Track 10, Hub 8, App Audio 9 พารามิเตอร์ รวม bypass); `plugin-host-test --dump-state` เก็บและ restore state 6 แบบ × 3 ปลั๊กอิน × 2 format ครบ 36 กรณี จากไฟล์ที่แตกจาก installer 1.0.10 จริง เก็บ SHA-256 ของ installer/ไบนารีทั้ง 6 ใน `compat/fixtures/plugins-1.0.10/provenance.json` ตัวตรวจ offline เทียบ bin/XML, metadata กับ host และ hash ผ่านทั้งหมด ข้อจำกัด range/color ดู `compat/README.md`
- **R0.2 ✅ ตรวจในเครื่อง Windows และ Linux แล้ว**: `compat/golden/core` มี 20 f32 captures (DSP/LUFS/resampler/PI/Track/Hub/StreamConsumer/FriendReader); `compat/golden/vocal` มี input 4 แบบและ VocalChain 20 กรณี; `compat/golden/netsim` มี trace เวลาถึงของ packet ที่บันทึกผ่าน net-sim proxy จริง 3 โปรไฟล์ (ดี/บ้าน/มือถือ) และ FriendReader ที่เล่นซ้ำจาก trace รวม 58 ไฟล์พร้อม manifest/provenance/hash สร้างซ้ำได้ทุกบิตบน toolchain อ้างอิงของแต่ละชุด (core/vocal: MSVC, netsim: GCC 13.3.0) Linux GCC ผ่านเกณฑ์ `1e-6` (core 19/20 ไฟล์ตรงทุกบิต, แย่สุด −138.5 dBFS ใน VocalChain) `--rt-audit` นับ alloc/lock ใน audio thread: ทุกบล็อกเป็น 0 ยกเว้น baseline C++ 2 จุด (`StreamConsumer::pull` ครั้งแรก, Speex realloc ใน `FriendReader::pull`) ที่ Rust ต้องเป็น 0 ตาม R2.4 มี CTest และ workflow Linux `compat-golden.yml` ผ่านบน GitHub แล้ว (รวม `compat-layout.yml` Windows/Linux) merge เข้า `main` แล้ว packet loss/reorder ยังไม่ทดสอบเพราะ net-sim เป็น TCP ดู `compat/golden/README.md`
- **R0.5 ตรวจในเครื่อง Windows แล้ว**: workspace เฉพาะ `hs-tools`, Rust 1.99.0, static CRT, unwind, lockfile, fmt/clippy/test/release และ `cargo deny` ผ่าน มี workflow Windows/Linux ยังต้องยืนยันผล CI บน GitHub ก่อนปิดงาน
- ยังไม่เลือก D1–D8 หรือผ่าน G0 และยังไม่เปลี่ยนโค้ด audio/runtime การ optimize รอบนี้ลดภาระ build ของตัวตรวจ ABI โดยแยกให้ compile เฉพาะ header ได้

---

## 1. เป้าหมายและตัวชี้วัด (Definition of done)

| ตัวชี้วัด | เป้า | วัดอย่างไร |
|---|---|---|
| โปรเจกต์ที่เซฟด้วย 1.0.10 เปิดใน Rust แล้วค่าเหมือนเดิม | 100 % ของ fixture + host matrix | `compat/fixtures` (R0.1) + เปิดโปรเจกต์จริงใน 5 DAW |
| automation เดิมยังผูกกับพารามิเตอร์เดิม | 100 % | เทียบ parameter ID กับ fixture |
| เปิดโปรเจกต์ที่เซฟด้วยรุ่น Rust ใน 1.0.10 (downgrade) | ได้ | Rust เขียน state รูปแบบเดิม (หัวข้อ 3.2) |
| C++ ↔ Rust คุยผ่าน shm v14 | ตรงทุก sample | `ipc` test ทั้ง 2 ทิศ (R1.5) |
| เสียงที่ออกเทียบของเดิม | ต่างไม่เกิน −120 dBFS (บล็อก DSP), ตรงทุกบิตสำหรับ ring / PCM / wire | golden test (R0.2) |
| CPU audio thread ของ Hub (8 เพื่อน, 48 k, 256) | ไม่เกินของเดิม +5 % | ตัววัดจาก PLAN-v2 Phase 0 |
| ความหน่วง | ไม่เพิ่มแม้ 1 sample (Hub `setLatencySamples(0)` เหมือนเดิม) | plugin-host-test null test |
| test ที่มีอยู่ | ผ่านครบ 14/14 ชุด + unit 68 ชุด (port มาเป็น `cargo test`) | `.\build.ps1 -Test` / `cargo test` |
| pluginval / Steinberg validator | strictness 10 ผ่าน | CI |
| ขนาดตัวติดตั้ง | ไม่ใหญ่กว่าเดิมเกิน 20 % | เทียบ `HEARASIDE-Setup-*.exe` |
| ไม่มี GPL ใน `libs` / crate แกน | 0 รายการ | `cargo deny check licenses` |

---

## 2. ของที่มีตอนนี้ (inventory)

นับเฉพาะ `.cpp/.h/.c` ของเรา (ไม่รวม `external/`)

| โฟลเดอร์ | บรรทัด | พึ่ง JUCE | ไป crate ไหน | ความยาก | หมายเหตุ |
|---|---|---|---|---|---|
| `libs/ssbus` | 1,689 | ไม่ | `hs-bus` | กลาง | shm Win (security descriptor) + POSIX, broadcast ring, slot claim/reclaim, mailbox, remote queue, paced_send |
| `libs/ssdsp` | 771 | ไม่ | `hs-dsp` | ง่าย | smoother, pan, limiter, LUFS, PI drift, resampler (speexdsp), `StreamConsumer` |
| `libs/ssengine` | 2,446 | ไม่ | `hs-engine` | กลาง–ยาก | `TrackPublisher`, `HubEngine`, `hub_friends`, `FriendReader` (jitter, drift) |
| `libs/ssnet` | 959 | ไม่ | `hs-net` | กลาง | wire HRA1/2/3, Opus, AEC (speexdsp), WebRTC (libdatachannel + mbedtls) |
| `plugins/common` (ไม่รวม ui) | ~3,500 | ใช่ | `hs-net` (ShareServer), `hs-app` (Settings, Account, Strings) | กลาง | ShareServer, AccountManager, Settings, Strings ไทย/อังกฤษ, Links, FriendDirectory |
| `plugins/common/ui` | ~7,200 | ใช่ (ทั้งหมด) | `hs-ui` | **ยากมาก** | Frosted Studio, ธีม, ฟอนต์ Anuphan, scale 100–200 %, ลดการเคลื่อนไหว, QR, AccountPanel, SettingsPanel |
| `plugins/track` | 1,846 | ใช่ | `hs-plugin-track` | กลาง | 10 พารามิเตอร์, role friend/track, state XML |
| `plugins/app` | 2,108 | ใช่ | `hs-plugin-app` | กลาง–ยาก | WASAPI process loopback, LinkReceiver, TakeRecorder (WAV) |
| `plugins/hub` | 7,592 | ใช่ | `hs-plugin-hub` | **ยากมาก** | หน้า UI 4,400+ บรรทัด, FriendRoom, ShareDirectory, ControlServer, REST API, VirtualMic, LatencyMeter, auto-sync |
| `apps/karaoke` | 4,786 | ใช่ | `hs-karaoke` | ยาก | ASIO host เขียนเอง (HAS Direct), VocalChain (`juce::dsp::IIR`, `juce::dsp::Reverb`), MusicPlayer, RoomHost, JoinClient |
| `obs/hearaside-obs` | 430 | ไม่ | `hs-obs` | ง่าย | C API ของ libobs, hotkey `hearaside.mute_stream`, `hearaside.preview` |
| `obs/streamlabs-setup` | 281 | ใช่ (core) | `hs-streamlabs-setup` | ง่าย | |
| `tools/*` | 2,253 | บางตัว | `hs-tools` | ง่าย–กลาง | bus-inspector, ui-snapshot (test runner ด้วย), plugin-host-test (**เก็บเป็น C++ ไว้ใช้ทดสอบ**) |
| `tests/unit` + `integration` | 4,234 | ไม่ | `cargo test` ในแต่ละ crate | กลาง | port ทีละไฟล์ ชื่อ test เดิม |
| **รวม** | **~43,000** | | | | |

ของที่ไม่ใช่ C++ และ **ไม่ต้องย้าย**: `web/site`, `web/share-vercel`, `plugins/hub/web/*.html` (ฝังด้วย `include_bytes!` แทน `juce_add_binary_data`), `tests/web-friend/*.mjs`, `installer/windows/*.iss`, `design/tokens.json` (เพิ่ม output `theme.rs` ใน `gen_theme.py`)

---

## 3. สัญญาความเข้ากันได้ (ห้ามพัง)

ทุกข้อในหัวข้อนี้ต้องมี test อัตโนมัติก่อนเริ่ม Phase ที่แตะมัน

### 3.1 Shared memory v14

1. `#[repr(C)]` + `#[repr(align(64))]` ให้ตรงกับ `alignas(64)` ใน `protocol.h` ใช้ `AtomicU32/AtomicU64/AtomicI64` แทน `std::atomic<>` (ขนาดและ layout เท่ากัน)
2. ตรวจขนาดและ offset ตอนคอมไพล์: `const _: () = assert!(size_of::<BusHeader>() == 4096);` และ `offset_of!` ทุกฟิลด์เทียบกับไฟล์ `compat/fixtures/shm-v14-layout.json` (R0.1)
3. ชื่อ segment, magic `'SSP1'`, `kProtocolVersion = 14`, security descriptor บน Windows (ให้ DAW / OBS ที่รันแบบ admin เปิดได้) ต้องเหมือนเดิม
4. memory ordering ต้องแปลตรงตัว: C++ `memory_order_release` → `Ordering::Release` ฯลฯ ห้าม "ปรับให้ดีขึ้น" ระหว่างย้าย
5. ห้าม bump protocol จนกว่าทุกส่วนเป็น Rust แล้ว (R9) ถ้าจำเป็นจริงต้องทำตามกฎเดิมใน CLAUDE.md ทั้งฝั่ง C++ และ Rust พร้อมกัน

### 3.2 โปรเจกต์ DAW (VST3 / VST2)

1. **VST3 class ID** (16 ไบต์ต่อปลั๊กอิน) ของ Track (`HrTk`), Hub (`HrHb`), App Audio (`HrAp`) ผู้ผลิต `Hrsd`: อ่านจากไบนารี 1.0.10 จริงด้วย `hs-compat-dump` (R0.1) แล้ว hardcode ใน Rust ห้ามคำนวณเอง
2. **Parameter ID**: JUCE แปลง string ID (`trackparam::Mon` ฯลฯ) เป็นเลข 32 บิตด้วย hash ของมันเอง และมีพารามิเตอร์ที่ JUCE เพิ่มให้เอง (bypass ฯลฯ) ต้อง dump เลขจริง ชื่อ ช่วงค่า ค่าเริ่มต้น และลำดับ จากไบนารีจริง ไม่เดา
3. **State**: ตอนนี้ `getStateInformation` เขียน `ValueTree` เป็น XML แล้ว `copyXmlToBinary` (หัว `VC2!` + ขนาด + XML) และ wrapper VST3 ของ JUCE อาจต่อข้อมูลของตัวเองท้าย blob Rust ต้อง
   1. อ่าน blob ของ 1.0.10 ได้ทุกแบบใน fixture (ค่าเริ่มต้น, เปลี่ยนทุกพารามิเตอร์, role friend, ชื่อ/สี/stem/chainMs)
   2. **เขียนกลับเป็นรูปแบบเดิม** (XML ชื่อ tag / property เดิม) เพื่อให้ downgrade เป็น 1.0.10 ได้ในช่วง beta
4. **ชื่อปลั๊กอิน / หมวด / ผู้ผลิต** เหมือนเดิม (`HEARASIDE Track`, `Fx Tools`) เพื่อให้ DAW ไม่มองเป็นปลั๊กอินใหม่
5. **VST2**: unique ID 4 ตัวอักษรเดิม และ chunk รูปแบบเดิม (ขึ้นกับ D3)

### 3.3 พฤติกรรมของ host ที่เราเคย patch JUCE

| patch | เหตุผล | ใน Rust ต้อง |
|---|---|---|
| `juce-early-channel-context.patch` | Studio One ส่งชื่อ/สีแทร็ก (`IInfoListener::setChannelContextInfos`) ก่อน processor พร้อม | เก็บค่าไว้แล้วใช้ทีหลัง ถ้า framework ไม่เปิด `IInfoListener` ต้อง fork / patch (R0.4 ตรวจ) |
| `juce-output-silence-flags.patch` | Studio One ส่ง silence flag ของ input ที่เงียบมาให้ output ตอนแทร็กถูก arm แล้วทิ้งเสียงที่ปลั๊กอินสร้าง | ล้าง `silenceFlags` ของ output ทุกบล็อก |
| (ใช้ในโค้ด) `isNonRealtime()`, playhead, `updateTrackProperties`, `setLatencySamples` | export offline, timeline tag, ชื่อแทร็ก, latency 0 | ต้องมีครบใน framework ที่เลือก (R0.4) |

### 3.4 สิ่งที่อยู่นอก DAW

1. wire `HRA1`, `HRA2`, `HRA3` (`libs/ssnet/include/ssnet/wire.h`) ตรงทุกไบต์ หน้าเว็บ `listen.html` / `send.html` ไม่ต้องแก้
2. พอร์ต: ShareServer `47810–47829`, UDP WebRTC `47830–47999` (ตัวติดตั้งเปิด firewall ไว้แล้ว)
3. REST API ตาม [rest-api.md](rest-api.md) (path, JSON, error) และการตรวจ `Origin`
4. cloudflared: วิธีเรียกและอ่านผลตาม [cloudflared-protocol.md](cloudflared-protocol.md) ที่อยู่ `C:\Program Files\HEARASIDE\cloudflared`
5. ไฟล์ผู้ใช้: `%APPDATA%\HEARASIDE\` (บัญชี, ค่าตั้ง), `Documents\HEARASIDE\Recordings`, รูปแบบไฟล์ค่าตั้งเดิม (อ่านได้ทั้งเก่าและใหม่)
6. OBS: source id `hearaside_source`, ชื่อ property และ hotkey `hearaside.mute_stream` / `hearaside.preview` เดิม (scene collection และ Stream Deck ของผู้ใช้ไม่หาย)
7. API เว็บ (`/api/register`, `/api/resolve`, device sign-in, `/me`) และ `release.json` สำหรับเช็กอัปเดต

---

## 4. สถาปัตยกรรมเป้าหมาย

### 4.1 Cargo workspace (อยู่ใน repo เดิม ข้างโค้ด C++)

```
Hearaside plugin/
├─ rust/
│  ├─ Cargo.toml                 # [workspace], profile, lints
│  ├─ rust-toolchain.toml        # ปักเวอร์ชัน stable + target x86_64-pc-windows-msvc
│  ├─ .cargo/config.toml         # +crt-static (เหมือน /MT ตอนนี้: ไม่ต้องมี VC++ redist)
│  ├─ deny.toml                  # license allow-list
│  ├─ crates/
│  │  ├─ hs-bus/                 # ← libs/ssbus        (MIT, no_std ได้ถ้าไม่ต้องการ OS)
│  │  ├─ hs-dsp/                 # ← libs/ssdsp        (+ speexdsp resampler ผ่าน cc)
│  │  ├─ hs-engine/              # ← libs/ssengine
│  │  ├─ hs-net/                 # ← libs/ssnet + ShareServer + REST + ControlServer
│  │  ├─ hs-app/                 # Settings, Account, Strings (ไทย/อังกฤษ), Links, FriendDirectory
│  │  ├─ hs-ui/                  # ← plugins/common/ui (ธีมจาก tokens.json)
│  │  ├─ hs-win/                 # COM: WASAPI process loopback, ASIO (HAS Direct), shm SD
│  │  ├─ hs-ffi/                 # C ABI ชั่วคราวให้ปลั๊กอิน JUCE เรียกแกน Rust (ลบใน R9)
│  │  ├─ hs-plugin-track/
│  │  ├─ hs-plugin-app/
│  │  ├─ hs-plugin-hub/
│  │  ├─ hs-karaoke/             # .exe
│  │  ├─ hs-obs/                 # cdylib ของ OBS
│  │  └─ hs-tools/               # bus-inspector, testkit (แทน ui-snapshot --test-*), compat-dump
│  └─ xtask/                     # cargo xtask bundle / installer / sync-web
├─ libs/ plugins/ apps/ obs/     # โค้ด C++ เดิม (ค่อย ๆ หายไปจนถึง R9)
└─ compat/                       # fixtures + golden จาก 1.0.10 (R0)
```

### 4.2 ใครพึ่งใคร

```
hs-bus ◄── hs-dsp ◄── hs-engine ◄──┬── hs-plugin-track / app / hub
   ▲                    ▲          ├── hs-karaoke
   │                    │          └── hs-ffi ──► ปลั๊กอิน JUCE เดิม (ช่วง hybrid)
   └── hs-obs ──────────┘
hs-net ◄── hs-plugin-hub, hs-plugin-app, hs-karaoke
hs-ui  ◄── ปลั๊กอินทุกตัว, hs-karaoke
hs-win ◄── hs-bus (Windows), hs-plugin-app, hs-karaoke
```

กฎ: `hs-bus`, `hs-dsp`, `hs-engine`, `hs-net` ห้ามพึ่ง UI หรือ framework ปลั๊กอิน (เหมือน `libs/*` ตอนนี้ที่ห้ามพึ่ง JUCE) และเป็น MIT

### 4.3 แผนที่ JUCE → Rust (ใช้ตอนแปลงโค้ด)

| ของเดิม | ใช้แทน |
|---|---|
| `juce::String`, `StringArray`, `CharPointer_UTF8` | `String`, `Vec<String>`, `&str` (UTF-8 อยู่แล้ว) |
| `juce::var`, `DynamicObject`, `JSON` | `serde` + `serde_json` |
| `ValueTree` + XML (state) | struct + `quick-xml` (อ่าน/เขียน XML เดิมเท่านั้น) |
| `juce::File`, `getSpecialLocation` | `std::fs` + `dirs` / `known-folders` (Windows) |
| `juce::Thread`, `Timer`, `MessageManager` | `std::thread`, timer ของ UI framework, channel (`crossbeam-channel`) |
| `StreamingSocket`, `WSAPoll` | `tokio` (เธรดของตัวเอง ไม่ใช่ audio thread) |
| `juce::URL`, `WebInputStream` | `reqwest` + `rustls` |
| `AudioBuffer`, `FloatVectorOperations` | slice `&mut [f32]` (ให้ compiler vectorize; วัดก่อนจะเขียน SIMD เอง) |
| `AudioProcessorValueTreeState` | พารามิเตอร์ของ framework (ID ตาม fixture) |
| `AudioDeviceManager` (karaoke, VirtualMic) | `cpal` (WASAPI) + `hs-win::asio` (HAS Direct ที่มีอยู่) |
| `WavAudioFormat` | `hound` |
| ตัวอ่านไฟล์เพลง (MusicPlayer) | `symphonia` |
| `juce::dsp::IIR` | biquad เขียนเอง (สูตร RBJ เดียวกับ JUCE) |
| `juce::dsp::Reverb` | port อัลกอริทึม Freeverb แบบเดียวกับที่ JUCE ใช้ แล้วเทียบ golden |
| `juce_add_binary_data` (ฟอนต์, html) | `include_bytes!` |
| `qrcodegen.cpp` | crate `qrcodegen` (ผู้เขียนเดียวกัน) |
| `ScopedLock` บน message thread | `Mutex` ได้ (ห้ามใน audio thread) |

---

## 5. การตัดสินใจ (ตัวเลือก + คำแนะนำ) ← ArmZ เลือกที่ G0

### D1 framework ปลั๊กอิน

| ตัวเลือก | ข้อดี | ข้อเสีย |
|---|---|---|
| **A. nice-plug** (community fork ของ NIH-plug, ISC) ✅ แนะนำ | VST3 + CLAP + standalone, ใช้ bindings `vst3` (MIT/Apache) ไม่ติด GPL, มี adapter UI หลายตัว | ยัง "experimental", API เปลี่ยนบ่อย (0.4 เปลี่ยน editor API) ต้องปักเวอร์ชันและอาจ fork |
| B. NIH-plug ตัวเดิม | เสถียรกว่า มีคนใช้มาก | อยู่ในโหมด maintenance และ VST3 ใช้ `vst3-sys` ที่เป็น **GPLv3** ทำให้ปลั๊กอินทั้งตัวต้อง GPL |
| C. เขียนเองบน crate `vst3` (+ `clap-sys`) | ควบคุมเองทั้งหมด แก้เรื่อง host ได้ตรงจุด | งานเยอะ ต้องทำ parameter, state, bus layout, editor เอง |

คำแนะนำ: **A** ปักเวอร์ชันใน `Cargo.lock` และเตรียม fork ไว้ใน org ถ้าต้อง patch (เช่น channel context) · ถ้า R0.4 พบว่าแก้ไม่ได้ ไป C

### D2 UI framework (ตัดสินด้วย spike R0.3 เท่านั้น)

| ตัวเลือก | ภาษาไทย (สระ/วรรณยุกต์ซ้อน) | ฝังใน DAW | ข้อสังเกต |
|---|---|---|---|
| **A. Slint** | shaping ครบ | adapter ของ nice-plug ยัง WIP | เขียน UI แบบ declarative ใกล้ design export, มี software renderer ใช้ทำ ui-snapshot ได้, license: GPLv3 / royalty-free (ต้องใส่เครดิต) / เชิงพาณิชย์ |
| **B. Vizia** (vizia-plug, MIT) | ใช้ cosmic-text (มี shaping) | adapter ใช้ baseview ตรง ๆ | สไตล์แบบ CSS เข้ากับ design tokens, API เปลี่ยนบ่อย |
| C. egui (MIT/Apache) | **ต้องพิสูจน์** ว่าวางสระบน/ล่างถูก | adapter เสถียรที่สุด | immediate mode ทำหน้าตา Frosted Studio ให้สวยได้ยากกว่า |
| D. WebView (WebView2) + HTML จาก design export | ดีมาก (เบราว์เซอร์) | เสี่ยง: Track มีหลายสิบตัวต่อโปรเจกต์ แต่ละหน้าต่างกิน RAM สูง, focus / คีย์บอร์ดใน DAW มีปัญหา, ต้องพึ่ง runtime ของ Windows | เร็วที่สุดถ้าจะได้หน้าตาตรง mock |

เกณฑ์ผ่าน spike (ต้องผ่านครบ 6 ข้อ):

1. ข้อความ `คุณได้ยิน · คนดูได้ยิน · ฟังแบบคนดู · ซิงค์เสียงร้องอัตโนมัติ · ผู้ที่ได้ยินเสียงตัวเองซ้ำ` ด้วยฟอนต์ Anuphan 400/500/600 แสดงถูกที่ 100 / 150 / 200 %
2. เปิด/ปิด editor 50 รอบใน Studio One 7, Cubase 15, Reaper, FL, Ableton ไม่ค้าง ไม่ leak
3. เปิด editor ภายใน 150 ms, RAM ต่อ editor ไม่เกิน 40 MB
4. ทำพื้นหลังกระจกขุ่นแบบ pre-render ได้ และธีมสว่าง/มืด/ตามระบบ
5. เรนเดอร์เป็น PNG แบบไม่มีหน้าต่างได้ (แทน `ui-snapshot`)
6. พิมพ์ภาษาไทยใน TextEditor ได้ (IME) ใน DAW อย่างน้อย 3 ตัว

คำแนะนำเบื้องต้น: spike **A กับ B** ก่อน ถ้าผ่านทั้งคู่เลือก A (เครื่องมือและเอกสารดีกว่า) · D ใช้เฉพาะถ้า A/B ไม่ผ่าน และใช้กับ Hub ตัวเดียว ไม่ใช้กับ Track

### D3 VST2

| ตัวเลือก | ผล |
|---|---|
| A. ไม่ทำ VST2 ในรุ่น Rust | ง่ายสุด แต่โปรเจกต์ที่ใช้ VST2 ของ HEARASIDE จะหาปลั๊กอินไม่เจอ |
| **B. shim VST2 บาง ๆ เป็น C++ (ไม่ใช้ JUCE, ~500 บรรทัด, ใช้ SDK ที่มี license)** เรียกแกน Rust ผ่าน C ABI ✅ แนะนำ | เก็บ ID และ chunk เดิมได้ editor ใช้ `hs-ui` ฝังใน HWND ที่ host ให้ |
| C. VST2 ใน Rust บน ABI ของ AEffect | ไม่มี C++ เหลือ แต่ต้องระวังเรื่อง license ของ header |

คำแนะนำ: **B** ถ้ายังมีคนใช้ VST2 อยู่ (คำถาม Q2) ถ้าไม่มีเลย **A** แล้วแจ้งใน release notes

### D4 network runtime

คำแนะนำ: `tokio` บนเธรดของตัวเอง (2 worker) สำหรับ HTTP / WebSocket / REST / cloudflared / บัญชี ใช้ `axum` (หรือ `hyper` ตรง ๆ) + `tokio-tungstenite` + `reqwest` (rustls) · audio thread ติดต่อผ่าน ring แบบไม่ใช้ lock (`rtrb`) เท่านั้น
ทางเลือกที่เล็กกว่า: `tiny_http` + `tungstenite` แบบเธรดเหมือนโค้ดเดิม (ถ้าอยากให้ diff ใกล้ C++ มากที่สุด)

### D5 WebRTC DataChannel

| ตัวเลือก | ข้อดี | ข้อเสีย |
|---|---|---|
| **A. `str0m`** (Rust ล้วน, sans-IO, MIT) ✅ แนะนำ | ไม่ต้อง build C++ / mbedtls อีก, คุม socket เอง (ใช้พอร์ต 47830–47999 ได้ตรง) | ต้องพิสูจน์กับ Chrome / Safari มือถือผ่าน test `friend_page` |
| B. `datachannel` (binding ของ libdatachannel ตัวเดิม) | พฤติกรรมเหมือนเดิมแน่นอน | ยังต้อง build C++ + mbedtls + patch เดิม |
| C. `webrtc` (webrtc-rs) | ครบเครื่อง | ใหญ่ ช้ากว่า เกินความจำเป็น |

คำแนะนำ: A ถ้า test `friend_page` / `friend_opus` / net-sim ผ่าน ถ้าไม่ผ่านถอยไป B ได้โดยไม่กระทบ API ของ `hs-net`

### D6 โค้ด C ที่ใช้อยู่ (libopus, speexdsp)

คำแนะนำ: **ใช้ต่อ** compile จาก `external/` ด้วย `cc` crate (เหมือนที่ CMake ทำอยู่: `FLOATING_POINT`, `USE_KISS_FFT`, `USE_SSE`) เสียงจะตรงกับเดิม · การเปลี่ยน resampler เป็น `rubato` หรือ AEC เป็น Rust ล้วน เป็นงานหลังจบแผนนี้ (และต้องผ่าน golden test)

### D7 สะพานระหว่างย้าย (hybrid)

คำแนะนำ: `hs-ffi` ส่งออก `extern "C"` + header ที่สร้างด้วย `cbindgen`, CMake นำเข้าด้วย **Corrosion** มีสวิตช์ `-DHEARASIDE_USE_RUST_CORE=ON/OFF` สลับระหว่าง `libs/*` (C++) กับแกน Rust เพื่อเทียบผลและถอยกลับได้ทันที
ทางเลือก: `cxx` crate (ส่ง type C++ ข้ามได้สะดวกกว่า แต่ผูกแน่นกว่า และต้องลบทิ้งทีหลังอยู่ดี)

### D8 ปลายทาง

| ตัวเลือก | ความหมาย |
|---|---|
| A. Rust ทั้งหมด (R0–R9) | ไม่มี JUCE / C++ ของเราเหลือ (ยกเว้น VST2 shim ถ้าเลือก D3-B และ plugin-host-test) |
| **B. ตัดสินที่ G1** ✅ แนะนำ | ทำ R0–R4 ก่อน (แกน + OBS) แล้วดูผล spike UI + รุ่นทดสอบแกน Rust ค่อยเลือกว่าไป A หรือหยุดที่ C |
| C. hybrid ถาวร | แกน + network + OBS เป็น Rust, ปลั๊กอินและ UI ยังเป็น JUCE (ได้ความปลอดภัยส่วนที่รับข้อมูลจากเน็ตเกือบทั้งหมด ด้วยงานราว 40 %) |

---

## 6. แผนงานแต่ละ Phase

ทุกงาน: 1 งาน = 1 branch `rs/<ID>-<slug>` (เช่น `rs/R1.2-bus-protocol`) · ต้องมี test · รีวิวตามหัวข้อ 10.4 ก่อน merge

### R0: ลายนิ้วมือ 1.0.10 + spike (ไม่แก้โค้ดที่ส่งผู้ใช้)

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R0.1 | `hs-compat-dump` (Rust, crate `vst3`): โหลด `.vst3` 1.0.10 แล้วอ่าน factory: class ID, ชื่อ, หมวด, parameter ID/ชื่อ/ช่วง/ค่าเริ่ม/flag · ให้ `plugin-host-test` เพิ่ม `--dump-state` บันทึก state blob 6 แบบต่อปลั๊กอิน (VST3 + VST2) · โปรแกรม C++ เล็ก ๆ พิมพ์ `sizeof`/`offsetof` ทุกฟิลด์ใน `protocol.h` → `compat/fixtures/*.json`, `*.bin` | fixture ครบ 3 ปลั๊กอิน × 2 format + layout v14 commit แล้ว |
| R0.2 | golden เสียง: รัน `ssdsp` / `ssengine` / `FriendReader` / VocalChain C++ กับสัญญาณทดสอบ (sine, noise, impulse, เพลงสั้น, jitter จาก net-sim) → `compat/golden/*.f32` + สคริปต์สร้างใหม่ | สร้างซ้ำแล้วได้ไฟล์เดิมทุกบิต |
| R0.3 | spike UI ตาม D2 (A และ B): ปลั๊กอินเปล่า 1 หน้า (การ์ด Track 1 แถว + สไลเดอร์ + ข้อความไทย) | รายงานผลเกณฑ์ 6 ข้อต่อตัวเลือก + รูป PNG |
| R0.4 | spike framework (D1): ปลั๊กอินเปล่าทดสอบ channel context ใน Studio One, silence flags ตอน arm, `isNonRealtime`, playhead (timeSamples, playing), latency report, state ขนาดอิสระ, bus layout mono/stereo, VST3 + CLAP | ตาราง "ได้ / ต้อง patch / ไม่ได้" ต่อข้อในหัวข้อ 3.3 |
| R0.5 | toolchain + CI: `rust-toolchain.toml`, `+crt-static`, `cargo deny`, GitHub Actions (Windows + Linux: `cargo test`, `clippy -D warnings`, `fmt --check`) | CI เขียวบน workspace เปล่า |

**G0:** Opus สรุปผล R0.3 / R0.4 เป็นตารางเดียว ArmZ เลือก D1–D8 แล้วบันทึกในหัวข้อ 5 (ใส่ ✅ ตัวที่เลือก)

### R1: workspace + `hs-bus`

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R1.1 | โครง workspace ตามหัวข้อ 4.1 (เฉพาะ crate ที่ใช้ใน R1–R2) | `cargo build` ผ่านทั้ง Windows / Linux |
| R1.2 | `protocol.rs`: ทุก struct / enum / ค่าคงที่ของ `protocol.h` + assert ขนาดและ offset เทียบ fixture | test อ่าน `shm-v14-layout.json` แล้วเทียบ `offset_of!` ครบทุกฟิลด์ |
| R1.3 | shm Windows (`windows` crate: `CreateFileMappingW` + security descriptor เดิม) และ POSIX (`rustix`) | เปิด segment ที่ C++ สร้างได้ทั้ง 2 ทาง, ทดสอบ DAW แบบ admin + OBS ปกติ |
| R1.4 | ring, slot claim / reclaim (โปรเซสตาย), UUID ซ้ำ, mailbox, rename, remote queue, timeline tag, `paced_send` | port `test_bus.cpp`, `test_paced_send.cpp` ครบ ชื่อ test เดิม |
| R1.5 | `ipc` ข้ามภาษา: writer C++ → reader Rust และกลับกัน, kill โปรเซสแล้วคืน slot | 242,944 frames ตรงทุก sample ทั้ง 2 ทิศ |
| R1.6 | `loom` model ของ ring + seqlock (name, rename, tag), `miri` กับส่วนที่ไม่แตะ OS | ไม่มี data race ใน model |
| R1.7 | `bus-inspector` เป็น Rust | ใช้แทนตัวเดิมได้ |

### R2: `hs-dsp` + `hs-engine` + hybrid

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R2.1 | `hs-dsp`: Smoother, pan, limiter (lookahead, ไม่เกิน ceiling), LUFS M/S, PI drift, `VarResampler` (speexdsp ผ่าน `cc`), `StreamConsumer` | port `test_dsp.cpp` + golden R0.2 ผ่าน |
| R2.2 | `hs-engine`: `TrackPublisher`, `HubEngine` (re-anchor, sync safety, stream delay 500 ms, crossfade, solo, panic, stem 8, preview) | port `test_engine.cpp`, `test_autosync.cpp` + golden |
| R2.3 | `hub_friends` + `FriendReader` (jitter buffer, stall recovery แบบ `78863d5`, Line up, mix-minus) | port `test_friends.cpp`, `test_jitter.cpp` + golden |
| R2.4 | ความปลอดภัย real-time: `assert_no_alloc` ครอบ `process*` ใน test, port `test_rt_safety.cpp` | ไม่มี alloc / lock ใน audio path |
| R2.5 | `hs-ffi` + `cbindgen` + Corrosion + สวิตช์ `HEARASIDE_USE_RUST_CORE` ให้ Track / Hub / App Audio JUCE เรียกแกน Rust | `.\build.ps1 -Test` ผ่าน 14/14 ทั้ง ON และ OFF |
| R2.6 | วัด CPU ด้วยตัววัด PLAN-v2 Phase 0 เทียบ ON / OFF | ไม่เกิน +5 % |

**G1:** ออก `1.1.0-beta-rustcore` (ปลั๊กอิน JUCE + แกน Rust) ให้ ArmZ ใช้ไลฟ์จริงอย่างน้อย 2 สัปดาห์ แล้วตัดสิน D8 ต่อ: ไป R5–R9 หรือหยุดที่ hybrid (ทำ R3 + R4 ให้จบแล้วปิดแผน)

### R3: `hs-net` + เซิร์ฟเวอร์ (เริ่มได้หลัง R1 คู่ขนานกับ R2)

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R3.1 | wire HRA1/2/3 + Opus (libopus จาก `external/opus`) + AEC (speexdsp) | port `test_net.cpp`, ไบต์ตรงกับ C++ |
| R3.2 | WebRTC ตาม D5 + signaling เดิม, พอร์ต 47830–47999 | ต่อกับ `send.html` / `listen.html` เดิมได้ |
| R3.3 | ShareServer: HTTP + WebSocket, token, ตรวจ `Origin`, หา LAN / Radmin IP, เปิดและเฝ้า cloudflared, เธรดส่งแยกต่อเพื่อน, คิวส่งจำกัดขนาด | ตาม [cloudflared-protocol.md](cloudflared-protocol.md) |
| R3.4 | REST API + ControlServer ตาม [rest-api.md](rest-api.md) | test `rest_api` เดิมผ่าน |
| R3.5 | `hs-app`: AccountManager (device sign-in, `/me`), ShareDirectory (`/api/register`, `/api/resolve`), Settings (อ่านไฟล์เดิมได้), Strings ไทย/อังกฤษ | test `share_directory`, `share_api` ผ่าน |
| R3.6 | `hs-testkit`: CLI ที่รับ flag เดิมของ `ui-snapshot` (`--test-rest`, `--test-directory`, `--test-friends`, `--test-friend-app`, `--test-ui`) เพื่อให้ `tests/web-friend/*.mjs` รันกับ Rust ได้โดยไม่แก้ test | `friend_page`, `friend_wire`, `friend_opus` ผ่านกับ Rust |
| R3.7 | net-sim (loss / jitter / reorder) เทียบ C++ | gap ต่อนาทีไม่มากกว่าเดิม |

### R4: OBS source ใน Rust (ของชิ้นแรกที่ส่งผู้ใช้)

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R4.1 | `hs-obs` (cdylib): `bindgen` กับ header libobs 32.2.2 ใน `external/obs-studio` (หรือ crate `libobs` ถ้าตรงเวอร์ชัน), source id / property / hotkey เดิม, `StreamConsumer` จาก `hs-dsp`, auto buffer | scene collection ที่มี HEARASIDE เดิมเปิดแล้วค่าครบ |
| R4.2 | `remote.cpp` (คำสั่งจาก OBS → Hub ผ่าน remote queue) | hotkey ตัด/คืนเสียงคนดูทำงานกับ Hub C++ |
| R4.3 | `streamlabs-setup` เป็น Rust | ตัวติดตั้งยังติดตั้ง Streamlabs ได้ |
| R4.4 | พึ่งแค่ `obs.dll` (ตรวจด้วย `dumpbin /dependents`) | ไม่มี DLL อื่น |

ออกเป็น 1.0.11 หรือ 1.1.x ได้เลย (Hub / Track ยังเป็น C++ คุยผ่าน shm v14)

### R5: HAS Karaoke ใน Rust + `hs-ui`

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R5.1 | `hs-ui` ชุดแรก: ธีมจาก `tokens.json` (`gen_theme.py` สร้าง `theme.rs` ด้วย, test `design_tokens_in_sync` ครอบทั้งคู่), ฟอนต์ Anuphan, scale, ลดการเคลื่อนไหว, Controls, Overlay, ScrollArea, Icons, Backdrop, QrView | หน้าตัวอย่างผ่าน `design_contrast` |
| R5.2 | `hs-win::asio`: port HAS Direct (อ่านไดรเวอร์จาก `HKLM\SOFTWARE\ASIO` ผ่าน COM, แปลง sample type) **ไม่ใช้ ASIO SDK ของ Steinberg** (ไม่เปิด feature `asio` ของ cpal เพราะมันดึง SDK) + WASAPI ผ่าน `cpal` | port test ของ HasAsio, ใช้กับ interface จริง 2 ยี่ห้อ |
| R5.3 | VocalChain (gate, EQ biquad, comp, reverb แบบ Freeverb), MusicPlayer (`symphonia`), PreflightChecker, UpdateChecker | port `test_karaoke.cpp` + golden + ฟังเทียบ A/B |
| R5.4 | JoinClient (HRA2/HRA3 + UDP), RoomHost (ใช้ `hs-engine` + ShareServer เขียน bus ให้ OBS) | เป้าใน PLAN-karaoke-exe หัวข้อ 1 (ไมค์ → หูฟัง ≤ 10 ms ที่ ASIO 128) |
| R5.5 | MainWindow + AudioSettings + AccountPanel + SettingsPanel ด้วย `hs-ui` | ui snapshot เทียบภาพเดิม, ArmZ ยอมรับหน้าตา |
| R5.6 | อ่านค่าตั้งของ HAS Karaoke เดิม | อัปเดตทับแล้วค่าไม่หาย |

### R6: HEARASIDE Track ใน Rust

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R6.1 | ประกาศพารามิเตอร์ 10 ตัว (+ ของที่ JUCE เพิ่มเอง) ตาม fixture: ID, ชื่อ, ช่วง, ค่าเริ่ม, ข้อความค่า (dB, ms, pan) | เทียบ fixture ตรงทั้งหมด |
| R6.2 | state: อ่าน blob 1.0.10 ทุกแบบ, เขียนกลับรูปแบบเดิม (uuid, name, bus, stem, chainMs, role, friendId) | round-trip กับ fixture + 1.0.10 อ่าน state จาก Rust ได้ |
| R6.3 | process: publish, คุณได้ยิน / คนดูได้ยิน, หน่วง 0–500 ms, friend input, คำสั่งจาก Hub ผ่าน host (undo / automation ได้), offline | `plugin_host_vst3` (C++ เดิม) ผ่านกับไฟล์ Rust |
| R6.4 | host quirks หัวข้อ 3.3 | ทดสอบ Studio One ตอน arm แทร็ก |
| R6.5 | editor ด้วย `hs-ui` | ui snapshot + audit layout 0 ปัญหา |
| R6.6 | VST2 ตาม D3 | `plugin_host_vst2` ผ่าน |
| R6.7 | pluginval strictness 10 + Steinberg validator | ผ่าน |

**G2:** host matrix (Studio One 7, Cubase 15, Reaper, FL, Ableton) กับ **โปรเจกต์จริงที่เซฟด้วย 1.0.10** อย่างน้อย DAW ละ 1 โปรเจกต์ (มี automation, friend input, stem) ต้องเปิดแล้วเสียงและค่าเหมือนเดิม Track Rust ทำงานคู่กับ Hub C++ ได้ ถ้าไม่ผ่าน กลับไปแก้ก่อนแตะ R7

### R7: HEARASIDE App Audio ใน Rust

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R7.1 | `hs-win::loopback`: `ActivateAudioInterfaceAsync` + `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (include / exclude process tree, "ทั้งเครื่องยกเว้น DAW"), buffer ปรับเอง | ดึงเสียง YouTube / Spotify ได้ ไม่ดึงเสียง DAW กลับ |
| R7.2 | LinkReceiver (รับลิงก์ HEARASIDE อื่น / เพื่อน), TakeRecorder (`hound`, อัดตาม DAW), `kFlagApp`, sync delay, "เฉพาะเสียงโปรแกรม" | test `friend_app_source` ผ่าน |
| R7.3 | พารามิเตอร์ / state / editor ตามแบบ R6 | fixture + plugin_host ผ่าน |

### R8: HEARASIDE Hub ใน Rust (ใหญ่สุด แบ่งย่อยมากที่สุด)

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R8.1 | processor: พารามิเตอร์ 7 ตัว + state, `HubEngine`, latency 0, offline, preview / panic | fixture + plugin_host ผ่าน |
| R8.2 | รวม ShareServer, FriendRoom (8 คน, Line up, REC แยกคน), ShareDirectory, REST, ControlServer | test `friends_room`, `rest_api`, `share_*` ผ่าน |
| R8.3 | LatencyMeter, auto-sync (นับถอยหลัง, วัด, ตั้ง delay ให้ App Audio / Track), VirtualMic (`cpal` + `StreamConsumer` บนเธรดของ device) | test auto-sync เดิมผ่าน, VB-Cable ใช้กับ TikTok LIVE Studio ได้ |
| R8.4 | UI หน้าหลัก (header, TrackView rows, SourceView, LUFS, สถานะ OBS) | snapshot เทียบ `ui-snapshots/redesign` |
| R8.5 | หน้า Share and friends, Settings, Track fine settings, Auto-sync, Manage tracks, Manage program audio, KaraokeRoomPanel (ทีละหน้า 1 งาน) | snapshot + audit 0 ปัญหา ต่อหน้า |
| R8.6 | session preset (Karaoke / Podcast / LiveJam), account ในปลั๊กอิน, Radmin | test `ui_values` ผ่าน |

### R9: build, installer, ลบ C++

| ID | งาน | เสร็จเมื่อ |
|---|---|---|
| R9.1 | `cargo xtask bundle` สร้าง `.vst3` bundle (+ `.clap` ถ้าเลือก) + `.dll` VST2 + `hs-obs.dll` + `HAS Karaoke.exe` | โครงไฟล์เหมือนที่ `hearaside.iss` คาด |
| R9.2 | `build.ps1` เรียก xtask, `build-installer.ps1` / `hearaside.iss` ชี้ไฟล์ใหม่, `release.json` อัปเดตเหมือนเดิม, เซ็นรหัส | ติดตั้งทับ 1.0.10 แล้วใช้งานต่อได้ทันที |
| R9.3 | beta กับผู้ใช้จริง 2 รอบ (ลิงก์ดาวน์เกรดเป็น 1.0.10 ไว้ใน release notes) | ไม่มีบั๊กระดับ "โปรเจกต์เปิดไม่ได้ / ไม่มีเสียง" |
| R9.4 | ลบ `libs/`, `plugins/`, `apps/`, `obs/` C++, JUCE, Corrosion, `hs-ffi` (หลัง stable 2 รุ่น) | CMake เหลือเฉพาะ plugin-host-test (+ VST2 shim ถ้ามี) |
| R9.5 | อัปเดต CLAUDE.md (build / test / กฎ audio thread แบบ Rust), README, CONTRIBUTING | |

---

## 7. สะพาน C ABI ช่วง hybrid (R2.5–R8)

1. `hs-ffi` เป็น `staticlib` ฟังก์ชันทุกตัว `extern "C"` + `#[no_mangle]` ชื่อขึ้นต้น `hs_`
2. object ข้ามภาษาเป็น opaque pointer (`hs_hub_engine*`) สร้างด้วย `hs_*_new`, ทำลายด้วย `hs_*_free` ฝั่ง C++ ห่อด้วย `std::unique_ptr` + custom deleter
3. ฟังก์ชันที่เรียกจาก audio thread (`hs_hub_process`, `hs_track_publish`, `hs_friend_pull`) รับ pointer ของ buffer ที่ host เป็นเจ้าของ ห้าม alloc, ห้าม lock, ห้าม panic
4. **ห้าม panic ข้าม FFI**: ทุกฟังก์ชันครอบ `std::panic::catch_unwind` ถ้า panic ให้คืนค่า error และฝั่ง audio ส่งความเงียบ (ไม่ทำ DAW ล่ม) · profile release ตั้ง `panic = "unwind"` ใน crate ที่อยู่ใต้ FFI/ปลั๊กอิน
5. string ข้ามภาษาเป็น UTF-8 + ความยาว (ไม่ใช่ NUL-terminated) ยกเว้นฟิลด์ใน shm ที่เป็นแบบเดิม
6. header สร้างด้วย `cbindgen` ตอน build และ commit ไว้ใน repo, CI ตรวจว่าไม่ล้าสมัย
7. สวิตช์ `HEARASIDE_USE_RUST_CORE` ต้องอยู่จนถึง R9.4 เพื่อเทียบผลและถอยกลับ

---

## 8. กฎ real-time ใน Rust (แทนข้อ 1 ของ Hard rules ใน CLAUDE.md)

1. โค้ดที่รันใน audio thread (process ของปลั๊กอิน, `HubEngine::process*`, `FriendReader::pull`, callback ของ cpal / ASIO / WASAPI, OBS audio) ห้าม: `Box::new`, `Vec::push` ที่อาจขยาย, `String` / `format!`, `Mutex` / `RwLock`, `println!` / `log`, syscall, `Arc` drop ที่อาจเป็นตัวสุดท้าย
2. จองหน่วยความจำทั้งหมดตอน `initialize` / `prepare` ด้วยขนาดสูงสุด (เหมือน C++ ตอนนี้)
3. รับ/ส่งข้อมูลกับเธรดอื่นผ่าน `rtrb` (SPSC), atomics หรือ `arc-swap`; ของที่ต้องทิ้งส่งกลับไปให้เธรดอื่น drop (เช่น `basedrop`)
4. test ทุกตัวที่เรียก process ครอบด้วย `assert_no_alloc` (debug) ใน CI
5. `unsafe` ทุกบล็อกต้องมี `// SAFETY:` อธิบายว่าทำไมถูก และรวมอยู่ใน module ไม่กี่ตัว (`hs-bus::shm`, `hs-win`, `hs-ffi`, `hs-obs`)
6. ห้ามใช้ `async` ใน audio thread ใด ๆ ทั้งสิ้น

---

## 9. การทดสอบ

| ระดับ | ทำอะไร | เมื่อไร |
|---|---|---|
| unit | port `tests/unit/*.cpp` มาเป็น `#[test]` ในแต่ละ crate ชื่อ test เดิม (ค้นหาข้ามภาษาได้) | ทุก PR |
| golden | เทียบเสียงกับ `compat/golden` (−120 dBFS สำหรับ DSP, ตรงทุกบิตสำหรับ ring / PCM / wire) | ทุก PR ที่แตะ dsp / engine / net |
| compat | layout v14, parameter ID, state blob, OBS settings, ไฟล์ค่าตั้ง | ทุก PR |
| concurrency | `loom` (ring, seqlock, mailbox), `miri` (ส่วนที่ไม่แตะ OS) | ทุก PR ที่แตะ `hs-bus` |
| ข้ามภาษา | `ipc` C++ ↔ Rust ทั้ง 2 ทิศ | จนถึง R9.4 |
| black-box | `plugin-host-test` (C++ / JUCE host เดิม **เก็บไว้ถาวร** เพราะเป็นตัวแทน DAW ที่ไม่ใช่โค้ดของเรา), pluginval 10, Steinberg validator | R6–R8 และทุก release |
| เว็บ | `tests/web-friend/*.mjs` กับ `hs-testkit` (Chrome จริงผ่าน Playwright), `npm test` ใน `web/site` | R3 ขึ้นไป |
| เน็ต | net-sim (loss / jitter / reorder) | R3, R8 |
| คน | host matrix 5 DAW + OBS 32 + Streamlabs + TikTok LIVE Studio (VirtualMic) กับโปรเจกต์ 1.0.10 จริง | G2, ก่อนทุก beta |

---

## 10. วิธีทำงาน: Opus วางแผน → Sonnet ทำ → Opus รีวิว

### 10.1 บทบาท

1. **Opus 5.5 high**: แตกงานในหัวข้อ 6 ให้เล็กพอ, เขียน prompt ต่องาน, รีวิว, อัปเดตตาราง 0.4 ไม่เขียนโค้ด production เอง
2. **Sonnet 5.5 high** (subagent `model: sonnet`, `effort: high`): ทำ 1 งานต่อ 1 branch `rs/<ID>-<slug>` พร้อม test
3. **Opus 5.5 high ตัวใหม่** (ไม่เห็นงานตอนทำ): รีวิว diff ตาม 10.4

### 10.2 รอบต่อ 1 งาน

1. Opus เขียน prompt จากแม่แบบ 10.3 (ระบุไฟล์ C++ ต้นทาง, crate ปลายทาง, test ที่ต้อง port, fixture / golden ที่ต้องผ่าน)
2. Sonnet อ่านไฟล์ C++ ทั้งไฟล์ก่อนเขียน, แปลแบบตรงตัวก่อน (ไม่ refactor), รัน `cargo test`, `clippy -D warnings`, `fmt`, `deny`
3. Opus ตัวใหม่รีวิว → แก้ → merge → อัปเดต 0.4
4. **กฎแก้บั๊กสองฝั่ง**: ระหว่างย้าย ถ้าแก้บั๊กในโค้ด C++ ที่ port ไปแล้ว ต้องเปิดงาน `rs/fix-<ID>` แก้ฝั่ง Rust ด้วยในรอบเดียวกัน และเพิ่ม test ที่จับบั๊กนั้นทั้งสองฝั่ง

### 10.3 Prompt แม่แบบสำหรับ Builder (Sonnet)

```
งาน: <ID> <ชื่อ> จาก docs/PLAN-rust-migration.md หัวข้อ 6
branch: rs/<ID>-<slug>
ต้นทาง C++: <ไฟล์> (อ่านทั้งไฟล์ก่อน)
ปลายทาง: rust/crates/<crate>/src/<ไฟล์>.rs
ต้องผ่าน: <test ที่ port>, <fixture>, <golden>
กฎ:
1. แปลตรงตัวก่อน ห้ามเปลี่ยนอัลกอริทึม / ลำดับคำนวณ float / memory ordering
2. ห้ามแตะ shm layout, wire, parameter ID, state format
3. audio path: ห้าม alloc / lock / log / syscall (หัวข้อ 8) ครอบ test ด้วย assert_no_alloc
4. unsafe ต้องมี // SAFETY:
5. ห้ามคัดลอกโค้ด Jamulus (GPL) หรือ ListenTo
6. ข้อความ UI ใหม่ต้องมีทั้งไทยและอังกฤษใน hs-app::strings
7. จบด้วย: สิ่งที่ทำ, test ที่รัน + ผล, สิ่งที่ข้าม / ยังไม่ได้ตรวจ
```

### 10.4 Checklist ของ Reviewer (Opus)

1. ตรงกับ C++ ต้นทาง: เทียบทีละฟังก์ชัน ไม่มีพฤติกรรมหาย (โดยเฉพาะ edge case: seek, stop, sample rate change, slot reclaim, stall)
2. test ที่ port มาครบทุกกรณีของไฟล์เดิม และ golden / fixture ผ่าน ถ้าแก้ golden ต้องมีเหตุผลเขียนไว้
3. audio path ไม่มี alloc / lock / panic ที่เป็นไปได้ (`unwrap`, index เกินขอบเขต, หารด้วยศูนย์)
4. `unsafe` น้อยที่สุด มี `// SAFETY:` ที่ถูกจริง atomics ordering ตรง C++
5. FFI: `catch_unwind` ครบ, ownership ชัด, header ไม่ล้าสมัย
6. `clippy -D warnings`, `fmt`, `cargo deny` ผ่าน ไม่มี dependency ใหม่ที่ไม่จำเป็น (ponytail: ใช้ของที่มีก่อน)
7. ไม่มี abstraction ที่ไม่ได้ขอ, diff เล็กที่สุดที่ครบงาน
8. compat contract หัวข้อ 3 ไม่ถูกแตะ

### 10.5 ใกล้หมด context / usage

หยุดแล้วเขียน `docs/handoff/<YYYY-MM-DD>-rust-<ID>.md`: สถานะ, ไฟล์ที่แตะ, branch, test ที่ผ่าน / ไม่ผ่าน, ขั้นต่อไป

---

## 11. ความเสี่ยง

| # | ความเสี่ยง | โอกาส | ผลกระทบ | ลดอย่างไร |
|---|---|---|---|---|
| 1 | โปรเจกต์เก่าเปิดแล้วค่าหาย / ปลั๊กอินหาย (ID / state ไม่ตรง) | กลาง | สูงมาก | fixture จากไบนารีจริง (R0.1), round-trip test, G2 กับโปรเจกต์จริง, beta ที่ดาวน์เกรดได้ |
| 2 | framework Rust ไม่รองรับพฤติกรรม host ที่ JUCE รองรับ | กลาง | สูง | spike R0.4 ก่อนตัดสิน, เตรียม fork, ทางถอย D1-C หรือหยุดที่ hybrid (D8) |
| 3 | UI ภาษาไทยแสดงผิด / พิมพ์ไทยไม่ได้ใน DAW | กลาง | สูง | เกณฑ์ spike 6 ข้อ, ไม่เลือก framework ก่อนผ่าน |
| 4 | UI ใช้เวลานานกว่าที่คิดมาก (~15k บรรทัด) | สูง | กลาง | ทำ Karaoke ก่อน (ฝึกบน app เดียว), แบ่งหน้า Hub ทีละงาน, ทางเลือกหยุดที่ hybrid |
| 5 | เสียงเปลี่ยนเล็กน้อย (ลำดับ float, reverb) | กลาง | กลาง | ใช้ C เดิม (D6), golden, ฟัง A/B ก่อนเปลี่ยน reverb |
| 6 | ต้องดูแลสองโค้ดเบสนานเกินไป | สูง | กลาง | กฎแก้บั๊กสองฝั่ง (10.2.4), จำกัดฟีเจอร์ใหม่ระหว่างย้าย (Q8), ลบ C++ ทันทีที่ stable 2 รุ่น |
| 7 | WebRTC Rust ไม่เข้ากับเบราว์เซอร์มือถือบางตัว | ต่ำ–กลาง | กลาง | test Chrome จริง + ทางถอย D5-B |
| 8 | license (GPL หลุดเข้ามา, Slint, ASIO SDK, VST2 SDK) | ต่ำ | สูง | `cargo deny`, ASIO แบบเขียนเองจาก ABI, เลี่ยง NIH-plug VST3 (GPL), ตัดสิน Slint license ตาม Q7 |
| 9 | panic ใน Rust ทำ DAW ล่ม | ต่ำ | สูง | `catch_unwind` ทุกขอบ, ไม่มี `unwrap` ใน audio path, fuzz wire parser (`cargo fuzz`) |
| 10 | API ของ nice-plug เปลี่ยนกลางทาง | สูง | ต่ำ–กลาง | ปักเวอร์ชัน, อัปเดตเป็นงานแยกเท่านั้น |

---

## 12. คำถามที่ต้องให้ ArmZ ตอบ (ก่อน G0)

1. **Q1** เหตุผลหลักที่อยากย้ายคืออะไร (ความปลอดภัย / ความเร็วในการพัฒนา / ฝึกภาษา / อยากเลิกพึ่ง JUCE) ถ้าเป็นเรื่องความปลอดภัยของ network อย่างเดียว แผนแบบ hybrid (D8-C) ได้ประโยชน์เกือบทั้งหมดด้วยงานน้อยกว่ามาก
2. **Q2** ยังมีผู้ใช้ VST2 อยู่ไหม (เลือก D3)
3. **Q3** อยากได้ CLAP เพิ่มไหม (ได้ฟรีจาก nice-plug แต่ต้องทดสอบเพิ่ม)
4. **Q4** macOS อยู่ในแผนไหม (ถ้าอยู่ ต้องใส่ใน spike R0.3 / R0.4 ตั้งแต่ต้น)
5. **Q5** ชื่อเวอร์ชัน: รุ่น Rust เต็มตัวเป็น 2.0.0 หรือไม่ (PLAN-v2 จอง 2.0 ไว้ให้ Hub ถึง Hub)
6. **Q6** ระหว่างย้ายรับบั๊กแก้อย่างเดียว หรือยังเพิ่มฟีเจอร์ใน C++ ด้วย (แนะนำ: ฟีเจอร์ใหม่เฉพาะส่วนที่ยังไม่เริ่ม port)
7. **Q7** license ของทั้งโปรเจกต์ (ปลั๊กอิน + app) เป็นอะไร: มีผลกับการเลือก Slint (GPLv3 / royalty-free ต้องใส่เครดิต / เชิงพาณิชย์)
8. **Q8** ยอมให้หน้าตา UI ต่างจากเดิมได้แค่ไหน (เหมือน pixel / เหมือน design export / ปรับได้)

---

## ภาคผนวก A: crate ที่วางแผนจะใช้ (ตรวจ license ซ้ำด้วย `cargo deny` ตอนเพิ่ม)

| crate | ใช้ทำ | license (ตามที่ทราบ ต้องตรวจซ้ำ) |
|---|---|---|
| `nice-plug` | framework ปลั๊กอิน | ISC |
| `vst3` (coupler-rs) | bindings VST3 (และ `hs-compat-dump`) | MIT / Apache-2.0 |
| `clap-sys` | CLAP (ถ้าเลือก) | MIT / Apache-2.0 |
| `slint` / `vizia` / `egui` | UI (D2) | GPLv3·royalty-free·commercial / MIT / MIT·Apache |
| `windows` | Win32, COM, WASAPI, ASIO, shm | MIT / Apache-2.0 |
| `rustix` | POSIX shm | Apache-2.0 / MIT |
| `cpal` | audio device (karaoke, VirtualMic) ไม่เปิด feature `asio` | Apache-2.0 |
| `tokio`, `axum`, `tokio-tungstenite` | server | MIT |
| `reqwest` + `rustls` | HTTPS client | MIT / Apache-2.0 |
| `str0m` | WebRTC DataChannel | MIT |
| `opus` / `audiopus_sys` | libopus | MIT / Apache (libopus BSD-3) |
| `cc` | compile speexdsp | MIT / Apache-2.0 |
| `serde`, `serde_json`, `quick-xml` | JSON, state XML | MIT / Apache-2.0 |
| `hound` | WAV | Apache-2.0 |
| `symphonia` | ถอดรหัสไฟล์เพลง | MPL-2.0 |
| `qrcodegen` | QR | MIT |
| `rtrb`, `arc-swap`, `basedrop` | ส่งข้อมูลกับ audio thread | MIT / Apache-2.0 |
| `assert_no_alloc` | ตรวจ alloc ใน audio thread | BSD-2 |
| `loom` | test atomics | MIT |
| `cbindgen`, Corrosion | สะพาน C ABI (เครื่องมือ build เท่านั้น) | MPL-2.0, MIT |

## ภาคผนวก B: ศัพท์

1. **strangler**: วิธีย้ายระบบที่ค่อย ๆ แทนทีละส่วน ของเดิมยังทำงานตลอด
2. **hybrid**: ปลั๊กอิน / UI ยังเป็น JUCE แต่แกนเสียงและ network เป็น Rust ผ่าน C ABI
3. **fixture**: ไฟล์ที่เก็บค่าจริงของ 1.0.10 (ID, state, layout) ใช้เป็นตัวตัดสินว่าเข้ากันได้
4. **golden**: เสียงผลลัพธ์อ้างอิงจากโค้ด C++ ใช้เทียบกับ Rust
5. **spike**: โปรแกรมทดลองสั้น ๆ เพื่อตอบคำถามเดียว (ทิ้งได้)
6. **sans-IO**: ไลบรารีที่ไม่เปิด socket เอง เราส่งข้อมูลเข้า/ออกเอง คุมพอร์ตและเธรดได้

## ภาคผนวก C: แหล่งอ้างอิง

1. [NIH-plug (maintenance mode, VST3 bindings GPLv3)](https://github.com/robbert-vdh/nih-plug)
2. [nice-plug: community fork, VST3 + CLAP, adapter egui / iced / slint / vizia](https://codeberg.org/prokopyl/nih-plug) · [docs.rs nice-plug](https://docs.rs/crate/nice-plug/0.3.0)
3. [crate `vst3` (coupler-rs, MIT / Apache-2.0)](https://docs.rs/crate/vst3/0.3.0)
4. [VST 3 เปลี่ยนเป็น MIT (ต.ค. 2025)](https://sonicstate.com/news/2025/10/30/vst-3-now-available-under-mit-license)
5. [cpal (ASIO ต้องใช้ SDK + LLVM)](https://docs.rs/crate/cpal/latest) · [crate `wasapi`](https://docs.rs/crate/wasapi)
6. [str0m (sans-IO WebRTC)](https://gittrend.io/repo/algesten/str0m) · [webrtc-rs](https://docs.rs/crate/webrtc/latest)
7. [libobs-rs](https://github.com/libobs-rs) · [obs-wrapper](https://docs.rs/obs-wrapper)
8. เอกสารในโปรเจกต์: [plan.md](plan.md), [PLAN-v2-realtime-karaoke.md](PLAN-v2-realtime-karaoke.md), [PLAN-karaoke-exe.md](PLAN-karaoke-exe.md), [rest-api.md](rest-api.md), [cloudflared-protocol.md](cloudflared-protocol.md)
