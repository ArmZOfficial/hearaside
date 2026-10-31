# พี่ ArmZ อ่านอันนี้ก่อน: branch `Sorryham` (R5.3 + R5.4)

สรุปสั้นสำหรับรีวิว เขียนให้อ่านจบใน 5 นาที รายละเอียดเต็มอยู่ใน [handoff R5.3](2026-10-31-rust-R5.3.md) และ [handoff R5.4](2026-10-31-rust-R5.4.md)

## 1. สามบรรทัด

1. **R5.3** ✅ ส่วนเสียงของ HAS Karaoke ที่ไม่ใช่หน้าจอ: vocal chain (gate / EQ / comp / reverb / echo), เครื่องเล่นเพลง, ตรวจหูฟัง-ลำโพงก่อนร้อง, เช็กอัปเดต. **เสียง vocal ตรงกับ C++ ทุกบิต**
2. **R5.4** ✅ ฝั่งโค้ด: เข้าห้องเพื่อน (`join.rs`) และเปิดห้องเอง (`room.rs`) ทดสอบกับ ShareServer ของ Rust จริงบน loopback รวม WebRTC DataChannel
3. ทุกอย่างอยู่ใน crate ใหม่ `rust/crates/hs-karaoke` (lib, ยังไม่มีหน้าต่าง = R5.5) **ไม่แตะ** shm layout / wire / parameter ID / state format, ไม่แตะโค้ด C++ ที่ส่งผู้ใช้ (แก้เฉพาะ tool `tools/compat-vocal-golden` เพิ่มโหมด `--render`)

## 2. รูปแบบเดียวทั้ง crate (เข้าใจอันนี้แล้วอ่านทุกไฟล์ได้)

C++ ใช้ object เดียวที่ทั้ง audio thread และ UI thread เรียก (มี mutex / atomics ปนกัน และ `callAsync([this])`) ใน Rust ทุกคลาสถูกแบ่งเป็น **2 ครึ่ง** คุยกันผ่าน atomics / ring ที่ไม่มี lock:

```
          UI thread (timer ~100 ms)                 audio thread (callback)
 ┌──────────────────────────────┐   atomics / ring   ┌─────────────────────────┐
 │ MusicPlayer / PreflightChecker│ ◄───────────────► │ MusicOutput / PreflightAudio │
 │ JoinClient / RoomHost         │   (ไม่มี lock)     │ JoinAudio / RoomAudio        │
 │  + poll() แทน callAsync       │                   │  0 allocation (มี test)      │
 └──────────────┬───────────────┘                   └─────────────────────────┘
                │ งานหนักอยู่บนเธรดของตัวเอง: ถอดรหัสเพลง, network (WebSocket + WebRTC + Opus), เขียน WAV
```

| C++ | Rust (UI ครึ่ง, audio ครึ่ง) | ไฟล์ |
|---|---|---|
| `VocalChain` | `VocalChain` + `Arc<VocalParams>` | `src/vocal.rs` |
| `MusicPlayer` | `music_player() -> (MusicPlayer, MusicOutput)` | `src/player.rs` |
| `PreflightChecker` | `preflight() -> (PreflightChecker, PreflightAudio)` | `src/preflight.rs` |
| `UpdateChecker` | `compare_versions`, `release_info`, `check_for_updates` | `src/update.rs` |
| `JoinClient` | `join_client() -> (JoinClient, JoinAudio)` | `src/join.rs` |
| `RoomHost` | `RoomHost::start() -> (RoomHost, RoomAudio)` | `src/room.rs` |

ลำดับอ่านที่แนะนำ (เล็กไปใหญ่): `update.rs` → `preflight.rs` → `vocal.rs` → `room.rs` → `player.rs` → `join.rs` ทุกไฟล์ขึ้นต้นด้วย doc ที่บอก "ทำอะไร" และ "บั๊ก C++ ที่แก้"

## 3. บั๊ก C++ ที่ผู้ใช้เจอจริง (เลือกมาเฉพาะที่สำคัญ รายการเต็มอยู่ใน handoff)

1. **เปิดห้องเอง เพื่อนได้ยินเสียงเพื่อนผิดคน และอาจได้ยินเสียงตัวเองย้อน** (`RoomHost` เขียน `friendVoiceAudio` ทุกคู่ด้วย buffer เดียวกัน) ผิดเป้า "ไม่มีเสียงตัวเองย้อนจากเน็ต" ใน PLAN-karaoke-exe
2. **เปิดห้องเอง ล้าง shared memory ทั้งก้อน** (`memset`) ถ้ามีปลั๊กอิน Track อยู่บน bus `Main` อยู่แล้ว slot ของมันหายหมด
3. **เข้าห้องเพื่อนผ่าน UDP แล้วเสียงกระตุก**: packet ที่มาช้าถูกเล่นซ้ำผิดที่ (บั๊กเดียวกับที่แก้ฝั่ง Hub ใน 1.0.8 แต่ JoinClient ไม่เคยได้)
4. **เพลงกระตุกเมื่อดิสก์ช้า**: `MusicPlayer` อ่านไฟล์ + ถอดรหัส + ถือ lock บน audio thread
5. **use-after-free** จาก `callAsync([this])` ใน JoinClient / PreflightChecker ถ้าปิดหน้าต่างระหว่างทำงาน
6. block ที่ใหญ่กว่าที่ prepare ไว้: `VocalChain` ปล่อยเสียงค้าง, `RoomHost` เขียนเกิน buffer (memory corruption)

## 4. ตรวจเองใน 5 นาที

```powershell
cd rust
cargo test -p hs-karaoke           # 28 test: golden vocal, player, preflight, update, join (loopback + WebRTC), room, rt_safety
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo deny --locked check licenses bans sources
```

ฟัง A/B vocal (C++ กับ Rust ควรแยกไม่ออก ตัวเลขตรงทุกบิตแล้ว): ดูหัวข้อ "ฟังเทียบ A/B" ใน [handoff R5.3](2026-10-31-rust-R5.3.md)

## 5. ที่ต้องให้พี่ตัดสิน

1. **WASAPI** (ย้ายมาจาก R5.2): ยังไม่ทำ เพราะต้องมี "ตัวจัดการ device + audio callback" ของแอปก่อน ซึ่งเป็นงาน R5.5 (หน้าต่าง) เสนอให้ทำคู่กับ R5.5 หรือแยกเป็น R5.4b
2. **เป้า ไมค์ → หูฟัง ≤ 10 ms ที่ ASIO 128**: วัดได้เมื่อมีแอปที่รันเสียงจริง (R5.5) ตอนนี้ทุกชิ้นใน path นี้ไม่ alloc / ไม่ lock / ไม่เพิ่ม buffer
3. dependency ใหม่: `symphonia` (MPL-2.0, อยู่ในภาคผนวก A แล้ว; เพิ่ม MPL-2.0 ใน `deny.toml`), `tungstenite` (MIT/Apache, WebSocket client; TLS ใช้ `rustls` + cert ของ Windows ตัวเดียวกับ `hs-app`)
4. ข้อความไทยของ Preflight ยังไม่อยู่ใน `Strings` (เหมือน C++) ควรย้ายตอน R5.5
5. UpdateChecker: `installer` ใน `release.json` เป็น path สัมพัทธ์ แอปจึงพาไปหน้า download เสมอ (เหมือน C++) จะเปลี่ยนไหม
6. `hs-net` golden Opus 2 ตัวล้มบนเครื่องผม **ทั้งที่มีและไม่มีงานนี้** (ลองบน `rs/R3.3-share-server` สะอาดแล้ว) น่าจะเป็น SIMD ของ libopus ตาม CPU ฝากดูครับ
7. **CI Windows ยังแดง 1 test (ของ `hs-testkit` ไม่ใช่งานนี้):** `serve_friend_prints_the_link_and_what_the_hub_saw` ล้มบน `windows-latest` 2 รอบติด เสียงไมค์ถึง bus แค่ 480 / 960 sample (ต้อง ≥ 57,600) บนเครื่องผมผ่าน 7/7 รอบ (รวมรันทั้งไฟล์พร้อมกัน 4 รอบ) ไม่ได้เพิ่มเวลาให้ เพราะเสียงแทบไม่ถึงเลย อาจเป็นบั๊กจริงของ friend path ตอนเครื่องช้า ฝากพี่ดูครับ (ก่อนหน้านี้ test Windows ไม่เคยรันถึงตรงนี้ เพราะ Linux ล้มแล้ว fail-fast ยกเลิก Windows)

### CI ที่แก้ใน PR นี้ (ของเดิมบน `rs/R3.3-share-server` ล้มบน Linux อยู่แล้ว)

1. `rust.yml`: ติดตั้ง `libfontconfig1-dev` บน Linux (slint ของ `hs-ui` ต้องใช้) และ `fail-fast: false` ให้ Windows รันจบแม้ Linux ล้ม
2. `hs-obs`: `Host::{Streamlabs, UnknownFork}` ไม่ถูกใช้บน Linux, clippy `-D warnings` ล้ม → `allow(dead_code)` เฉพาะนอก Windows
3. `hs-app` test account: เช็ก "token ไม่อยู่ในไฟล์แบบ plain text" เฉพาะ Windows (นอก Windows เป็น JSON อ่านได้แค่เจ้าของ ตามที่ `vault.rs` ออกแบบไว้) และเพิ่มเวลารอ sign-in 5 → 20 s (CI Windows ค้างหลายวินาที 7 test ล้มพร้อมกัน)

## 6. Commit

| commit | เนื้อหา |
|---|---|
| merge `origin/main` | ไม่มีไฟล์เปลี่ยน (main ตามหลัง R3.3) |
| `d8d08a0` | R5.3 |
| `0c45da5` | R5.4 + เอกสารนี้ |
| `a949682` `58db48f` `cbd34c2` `8885e27` | แก้ CI (ดูหัวข้อ "CI ที่แก้ใน PR นี้") |

PR: https://github.com/ArmZOfficial/hearaside/pull/26 (draft, base `rs/R3.3-share-server`)
