# ข้อเสนอ: ห้องเพื่อน 8 คน (S2) + มิกซ์เสียงเพื่อนใน HEARASIDE Track (S7) — โปรโตคอล shm v13

> สถานะ: **รออนุมัติ** (prompt หัวข้อ 2 ข้อ 7 และ S7 ข้อ 6 — ห้ามแก้ `protocol.h` จนกว่าจะได้คำตอบ)
> เขียนเมื่อ 9 ต.ค. 2026 จากโค้ดหลัง commit `cbd13c9` (`kProtocolVersion = 12`)

## 0. สรุปสั้น

| เรื่อง | ข้อเสนอ |
|---|---|
| เวอร์ชัน | `kProtocolVersion` 12 → **13** (ชื่อ segment เปลี่ยนตามเวอร์ชันอยู่แล้ว → ต่างเวอร์ชันไม่เห็นกัน ไม่ crash) + **beacon segment ไม่มีเลขเวอร์ชัน** ไว้แจ้ง "เวอร์ชันไม่ตรงกัน" |
| ขนาด segment | เดิม ≈ 46 MB (90 ring × 512 KB + tags) → ใหม่ ≈ **50 MB** (+8 friend ring × 512 KB + header) · ถ้า feeder ใช้ ring เต็มขนาดด้วยจะเป็น ≈ 58 MB จึงเสนอ ring เล็ก (หัวข้อ 1) |
| เพื่อน | `FriendHeader friends[8]` + `ChannelRing friendAudio[8]` (ring ละ 1 คน, ผู้อ่านหลายตัวอ่านพร้อมกันได้เพราะทุกผู้อ่านถือ cursor ของตัวเองอยู่แล้ว — เหมือน `slotAudio` ปัจจุบัน) |
| เส้นทางตรง (Hub) | Hub engine อ่าน friend ring → resample/drift (โค้ดเดียวกับ App Audio) → บวกเข้าหูฟัง + บวกเข้า Stream Mix **หลังดีเลย์ D ก่อน limiter** |
| S7 feeder | `FeederRecord feeders[16]` — HEARASIDE Track บทบาท "Friend input" ประกาศตัวเอง; Hub จับคู่กับ Track ท้ายช่อง เขียนผลกลับใน `SlotHeader` (`fedBy`, `fxLatencyBits`, flag `kFlagViewersViaHub`) |
| ไม่ซ้ำสองทาง | Hub เป็นผู้ตัดสินเดียวว่าเพื่อน j อยู่ "ทางตรง" หรือ "ทาง DAW" (`FriendHeader::route`), เปลี่ยนด้วย crossfade 20 ms + toast |
| ช่องสั่งงาน ปลั๊กอิน → Hub | `PluginRequest requests[16]` (multi-writer แบบเดียวกับ `RemoteCommand`) + คำตอบใน `FeederRecord` — `resolveToken`, `createFriend`, `release`, `bringBack` |
| token | อยู่ใน state ของ Hub เท่านั้น ปลั๊กอินอื่นเห็นแค่ **friend id** (32 bit, คงที่ตลอดอายุเพื่อน) และ SHA-256 ของ token ไม่ได้ (Hub เทียบให้) |

## 1. Layout ใหม่ (v13)

```cpp
constexpr uint32_t kProtocolVersion = 13;   // v13: friends room (S2), friend input feeders (S7)
constexpr int      kMaxFriends      = 8;
constexpr int      kMaxFeeders      = 16;
constexpr int      kRequestQueue    = 16;

enum FriendState : uint32_t { kFriendFree = 0, kFriendWaiting = 1, kFriendLive = 2, kFriendOffline = 3 };
enum FriendRoute : uint32_t { kRouteDirect = 0, kRouteDawPending = 1, kRouteDaw = 2 };

struct alignas(64) FriendHeader {           // Hub (message thread + share server thread) writes
    std::atomic<uint32_t> state;            // FriendState
    std::atomic<uint32_t> id;               // ไม่ซ้ำตลอดอายุ Hub (0 = ว่าง), เก็บใน state ของ Hub
    std::atomic<uint32_t> nameSeq;          // seqlock เหมือน SlotHeader::nameSeq
    char                  name[kNameBytes];
    std::atomic<uint32_t> sampleRate;       // ของเบราว์เซอร์เพื่อน
    std::atomic<uint64_t> heartbeatNs;      // แพ็กเก็ตล่าสุด
    std::atomic<uint32_t> delayBits;        // d_i (ms, float bits) — S3, วัดต่อเนื่องจาก timestamp
    std::atomic<uint32_t> route;            // FriendRoute — ผู้ตัดสินคือ Hub
    std::atomic<int32_t>  feeder;           // index ใน feeders[] ที่กำลังป้อนเพื่อนคนนี้, -1 = ไม่มี
    std::atomic<int32_t>  outSlot;          // slot ของ HEARASIDE Track ท้ายช่องที่จับคู่แล้ว, -1
    std::atomic<uint32_t> peakBits;         // มิเตอร์ไมค์
    // S3: ตำแหน่ง Stream Mix ที่เพื่อนได้ยินตอนอัดตัวอย่างแรกของบล็อกล่าสุด (frames ของ Hub)
    std::atomic<uint64_t> streamPosOfWrite; // คู่กับ writePos ด้านล่าง (เขียนก่อน writePos, release)
    alignas(64) std::atomic<uint64_t> writePos;
};

struct alignas(64) FeederRecord {            // HEARASIDE Track บทบาท Friend input เขียน (message thread / audio thread)
    std::atomic<uint32_t> state;            // SlotState: free / claiming / active
    std::atomic<uint32_t> ownerPid;
    std::atomic<uint32_t> friendId;         // เพื่อนที่เลือก (0 = ไม่มี)
    std::atomic<uint32_t> nameSeq;
    char                  trackName[kNameBytes];  // ชื่อแทร็กจาก host (updateTrackProperties)
    uint32_t              colorARGB;
    std::atomic<uint64_t> heartbeatNs;      // processBlock ล่าสุด (audio thread)
    std::atomic<uint64_t> blockCount;       // นับบล็อก — Hub ใช้ดูว่า DAW พักแทร็ก
    std::atomic<uint32_t> status;           // 0 flowing, 1 bypassed, 2 offline render, 3 no friend
    std::atomic<uint32_t> reply;            // คำตอบ request ล่าสุด (ดูหัวข้อ 3)
    std::atomic<uint32_t> replySeq;
    std::atomic<uint32_t> hubCommand;       // Hub → feeder: 1 = "Bring back to the Hub" (กลับเป็น This track's sound)
    std::atomic<uint32_t> hubCommandSeq;
    // ที่ Track ท้ายช่องบอก Hub ไม่ได้ (ใช้ correlation, หัวข้อ 2): สำเนาสัญญาณที่ feeder ใส่ลง DAW
    alignas(64) std::atomic<uint64_t> writePos;
};

struct PluginRequest {                       // multi-writer (ปลั๊กอินใดก็ได้) → Hub อ่านตามลำดับ
    std::atomic<uint32_t> seq;
    std::atomic<int32_t>  feeder;           // ใครถาม (คำตอบไปที่ feeders[feeder].reply)
    std::atomic<uint32_t> kind;             // 1 resolveToken, 2 createFriend, 3 release, 4 copyLink
    std::atomic<uint32_t> arg;              // friendId (release / copyLink)
    char                  text[kNameBytes]; // token (resolve) หรือชื่อเพื่อน (create) — ใช้ครั้งเดียว Hub ล้างทันทีที่อ่าน
};
```

**SlotHeader** (Track ท้ายช่อง) — ใช้ที่ว่างใน `pad[56]` เดิม ไม่ขยาย struct:
```cpp
    std::atomic<uint32_t> fedBy;            // friendId ที่ Hub จับคู่ไว้, 0 = แทร็กธรรมดา
    std::atomic<uint32_t> fxLatencyBits;    // L_i (ms) ที่ Hub วัดได้
    // flag ใหม่ใน SlotFlags (Track เขียน): kFlagViewersViaHub = 1u << 7  — ส่งฝั่งคนดูขึ้น bus แทน DAW (Line up เปิด)
```
และ `BusLayout` ต่อท้าย: `FriendHeader friends[8]; FeederRecord feeders[16]; PluginRequest requests[16]; std::atomic<uint32_t> requestReserve; ChannelRing friendAudio[8]; ChannelRing feederAudio[16];` (feederAudio = สัญญาณที่ feeder ส่งลง DAW, Hub ใช้หาคู่ด้วย correlation — เสนอ ring เล็ก 16384 frames ต่อ feeder (128 KB × 16 = 2 MB) เพราะใช้แค่ correlation ~0.7 วินาที; ring ขนาดเต็มจะกิน 8 MB)

### ตรวจเวอร์ชันไม่ตรง (prompt S2 ข้อ 3)
ชื่อ segment มีเลขเวอร์ชันอยู่แล้ว (`HEARASIDE_<bus>_v12`) จึงไม่มีทาง crash แต่ผู้ใช้จะเห็นแค่ "No Hub yet" — เสนอเพิ่ม **beacon segment** ขนาด 64 byte ชื่อ `HEARASIDE_<bus>_beacon` (ไม่มีเลขเวอร์ชัน, layout ตายตัวตลอดไป: magic + protocolVersion ของ Hub + pid + heartbeat) Hub เขียนทุก 0.5 วิ · Track/App/OBS ที่หา bus ของตัวเองไม่เจอแต่เห็น beacon ที่เวอร์ชันต่าง → Banner `This Hub is a different HEARASIDE version (v12). Update every HEARASIDE plug-in to the same version.` (string ใหม่)

## 2. วัด L_i (ความหน่วงของปลั๊กอินในช่องของเพื่อน) และ CPU

- มีข้อมูลอยู่แล้ว: Hub เก็บ history แบบ decimate ÷4 ของทุก slot (`capSlots_`) ที่ `measureLatencies()` ใช้ (thread "HEARASIDE latency" 4 ครั้ง/วิ)
- เพิ่ม: feeder เขียนสำเนาสัญญาณที่ใส่ลง DAW (หลัง level, ก่อนปลั๊กอินถัดไป) ลง `feederAudio[k]` · Hub decimate เข้า history เหมือน slot
- จับคู่ + L_i: cross-correlation (normalised, FFT 8192 จุดที่ 12 kHz ≈ 0.68 วิ) ระหว่าง feeder k กับ slot ที่ยังไม่ถูกจับคู่ ช่วงหน่วง 0–200 ms · score > 0.6 สองครั้งติดกัน = คู่กัน · ทำเฉพาะเมื่อ feeder มีเสียง (RMS > −50 dBFS)
- CPU (ประมาณจากโค้ด auto sync ที่วัดแล้ว): FFT 8192 + inverse ต่อคู่ ≈ 0.15 ms บน CPU ปัจจุบัน · กรณีแย่สุด 16 feeders × 64 slots = 1024 คู่ × 4 ครั้ง/วิ → **จำกัด**: จับคู่ด้วยชื่อ/สีก่อน (ข้อ S7-3.1), ทำ correlation เฉพาะคู่ที่ชื่อไม่ตรง และไม่เกิน 8 คู่ต่อรอบ (= ~1.2 ms ทุก 250 ms บน thread แยก, ไม่แตะ audio thread) · หลังจับคู่แล้ววัด L_i ซ้ำแค่คู่เดียวทุก 2 วิ
- fallback ตอนเงียบ: ใช้ `chainLatencyBits` ของ Track ท้ายช่อง (มีอยู่แล้ว)

## 3. ช่องสั่งงาน ปลั๊กอิน → Hub

| kind | จาก | Hub ทำ | ตอบ (`feeders[k].reply`) |
|---|---|---|---|
| 1 resolveToken | Track วางลิงก์ `/s/<token>` | เทียบ token กับเพื่อนในห้อง | friendId หรือ `0xFFFF0001` = ลิงก์ห้องอื่น |
| 2 createFriend | Track "Invite a new friend…" | สร้างเพื่อน (ชื่อ = ชื่อแทร็ก) | friendId หรือ `0xFFFF0002` = ห้องเต็ม |
| 3 release | Track เลิกใช้เพื่อน | route กลับ Direct (crossfade + toast) | — |
| 4 copyLink | Track "Copy Mint's send-in link" | Hub วางลิงก์ลง clipboard เอง (ปลั๊กอินไม่เคยถือ token) | 1 = คัดลอกแล้ว |

token ไม่เคยถูกเขียนกลับลง shm (ข้อ S2-7 "ไม่ส่ง token กลับมาเก็บในปลั๊กอิน") — `copyLink` ให้ Hub เป็นคนคัดลอก (clipboard เป็นของ user session เดียวกันทุก process)

## 4. เส้นทางเสียง (สรุปเทียบ S7 ข้อ 4)

```
ทางตรง (route = Direct):  friendAudio[j] ─► Hub: resample → level/pan → +หูฟัง
                                                        └─► delay(D − d_j) → +Stream Mix (หลัง D, ก่อน limiter)
ทาง DAW (route = Daw):    friendAudio[j] ─► Track(feeder) บนสุด ─► EQ/comp/reverb ─► Track ท้ายช่อง (slot s, fedBy=j)
     Line up ปิด:  Track ท้ายช่อง → DAW master (คนดูได้ยินช้า d_j + L_j) · หูฟัง = slot s ตามปกติ
     Line up เปิด: Track ท้ายช่องตั้ง kFlagViewersViaHub: output ลง DAW = เงียบ, Hub อ่าน slot s ฝั่งคนดู → delay(D − d_j − L_j) → +Stream Mix
```
- เปลี่ยน route: Hub ตั้ง `route = DawPending` → รอ feeder heartbeat นิ่ง ≥ 500 ms → crossfade 20 ms ทางตรงออก/ทาง DAW เข้า → `route = Daw` → toast · feeder หาย > 300 ms → กลับ Direct ทันที (crossfade) + toast `FriendBackToHub`
- feeder 2 ตัวเลือกเพื่อนคนเดียวกัน: Hub ยอมรับตัวแรก (`friends[j].feeder`) ตัวหลังเห็นว่า `feeder != ตัวเอง` → ใส่ความเงียบ + แสดง `Fah · in “Audio 7”`
- Mute/panic: Hub ตัดทั้ง Stream Mix (ทางตรง + ทาง bus) และส่ง `kHubPanic` ที่ Track อ่านอยู่แล้ว (DAW master ถูกตัดโดย Hub บน master อยู่แล้ว)
- ไม่ซ้ำสองทาง: Hub ไม่บวกทางตรงเมื่อ route = Daw; ตอนยังไม่จับคู่ (feeder มีแต่ไม่มี Track ท้ายช่อง) เพื่อนก็ยังไหลลง DAW → master → คนดู **และ** Hub ยังส่งทางตรง = ซ้ำ! → จึงตั้ง **route = Daw ทันทีที่ feeder flowing แม้ยังไม่จับคู่** (คนดูได้ยินผ่าน DAW อย่างเดียว ไม่เข้าหูฟัง) + Banner warn ตามสเปก 3.8 · test integration S7 ตรวจ correlation ว่ามี peak เดียว

## 5. DAW ที่พักปลั๊กอินบนแทร็กว่าง (S7 ข้อ 6.3)

**ยังไม่ได้ทดสอบกับ DAW จริงในรอบนี้** (เครื่องนี้ไม่มี automation ของ DAW ให้ agent กดได้) ข้อมูลจากเอกสาร/พฤติกรรมที่รู้ — ต้องยืนยันด้วยมือก่อน merge S7:

| DAW | แทร็ก audio ว่าง (ไม่มี clip) | สิ่งที่ผู้ใช้ต้องทำ |
|---|---|---|
| Studio One | เรียกปลั๊กอินต่อ แต่ทิ้งเสียงที่ปลั๊กอินสร้างถ้า input เงียบ → แก้แล้วด้วย patch ล้าง silence flags (เดิม) | ไม่ต้อง (ตรวจซ้ำ) |
| Cubase / Nuendo | "Suspend VST3 plug-in processing when no audio signals are received" (ค่าเริ่ม เปิด) → พัก | ปิดตัวเลือกนี้ หรือ monitor แทร็ก · App Audio มี tail ∞ อยู่แล้ว → ใช้ทริกเดียวกัน (`getTailLengthSeconds = ∞`) |
| Reaper | เรียกตลอด (ยกเว้นตั้ง "anticipative FX" กับแทร็กเงียบ — ไม่พัก) | ไม่ต้อง |
| FL Studio | Mixer insert ที่ไม่มี input ยังเรียก (smart disable ถ้าเปิด → พัก) | ปิด Smart disable ของปลั๊กอินนั้น |
| Ableton Live | แทร็กเงียบยังเรียก effect | ไม่ต้อง (track ต้องไม่ freeze) |

ใน UI: Banner `FriendDawPaused` + ขั้นตอนตาม DAW ที่ตรวจพบ (มี string แล้ว) · Hub รู้ว่าพักจาก `blockCount` ไม่ขยับ > 300 ms

## 6. ทางเลือกที่พิจารณาแล้วไม่ใช้

1. **ใส่เพื่อนที่ Track ท้ายช่องตัวเดียว** — ปลั๊กอินก่อนหน้า (EQ/reverb) ไม่ได้ประมวลผลเพื่อน เพราะ Track อยู่ท้ายสุด → ผิดเป้าหมาย S7
2. **App Audio บนช่อง Input + input monitoring** — ทำได้วันนี้แต่ตั้งค่ายาก (ต้องสร้าง input channel, เปิด monitoring) และ latency ของ monitoring ทำให้ L_i คาดเดายาก
3. **ส่งเสียงเพื่อนผ่าน network เข้าปลั๊กอิน Track ตรง ๆ (ไม่ผ่าน Hub)** — ต้องให้ทุก Track ถือ token และเปิด socket ในปลั๊กอิน (ขัดหลัก "ไม่มี network ในปลั๊กอินสำหรับ /s/") และ line-up ทำไม่ได้เพราะ Hub ไม่รู้ d_i
4. **ring เดียวแบบ multi-reader ที่มี read index ใน shm** — ไม่จำเป็น: ring ปัจจุบันเป็นแบบ "อ่านที่ cursor ของตัวเอง" (`ringReadAt`) ผู้อ่านหลายตัวไม่แย่งกันอยู่แล้ว
5. **เก็บ token ใน FeederRecord** — โปรเจกต์ DAW ถูกส่งต่อได้ และ shm เปิดอ่านได้ทุก process ของ user → ใช้ friend id + ให้ Hub ทำ copyLink แทน

## 7. งานที่ตามมาเมื่ออนุมัติ (ลำดับ)

1. `protocol.h` v13 + beacon + test `test_bus.cpp` (8 friends write/read พร้อมกัน, ผู้อ่าน 3 ตัว, เพื่อนออก/เข้าใหม่, request queue multi-writer)
2. ShareServer: 8 friend slot, `/s/<token>` ต่อคน, newest-wins ภายใน slot เดียว, Origin check เดิม, state ใน Hub (token ไม่ลง log)
3. HubEngine: friend sources (resampler + drift เหมือน App Audio), level/pan/solo/stem, route
4. S3 timestamp ใน listen stream + send.html, S4 line-up D + crossfade, S5 App Audio เลือกเพื่อน + take shift
5. S7 feeder ใน TrackProcessor + matching ใน Hub + UI U11 (string พร้อมแล้ว)
6. Share page / Friends rows / App Audio / Track SourcePicker ใช้ข้อมูลจริงแทนสถานะ "No friends yet"

**คำถามที่ต้องตอบ:**
1. อนุมัติ layout v13 + beacon ตามข้างบนไหม (หรือให้ลด `feederAudio` เป็น ring เล็ก 8192 frames)?
2. ตกลงใช้ "route = Daw ทันทีที่ feeder flowing แม้ยังไม่จับคู่" (คนดูได้ยินผ่าน DAW อย่างเดียว ไม่ซ้ำ แต่ไม่เข้าหูฟังจนกว่าจะใส่ Track ท้ายช่อง) ไหม?
3. ขอเวลาทดสอบ DAW จริง 5 ตัว (หัวข้อ 5) โดยคุณ — หรือให้ agent ใช้ computer-use กับ Studio One ในเครื่องนี้?
