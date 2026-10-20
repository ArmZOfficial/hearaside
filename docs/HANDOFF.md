# Handoff: ทำงานต่อใน branch ที่ค้างอยู่

เอกสารนี้สรุปงานค้างในแต่ละ branch สำหรับ collaborator (เช่น `Sorryhxm` และทีมพัฒนา) ให้สามารถหยิบงานไปทำต่อ, ทดสอบ และเปิด PR ได้ทันที

---

## 1. สรุปสถานะ Branch ปัจจุบัน (ตรวจเมื่อ 21 ต.ค. 2026)

| Branch | ความคืบหน้า | สถานะ | สิ่งที่ต้องทำต่อ |
|---|---|---|---|
| **`v2/phase1`** *(งานหลัก)* | 38 commits (Builder A, B, C) | **Push ขึ้น GitHub แล้ว** (ผ่าน unit test 103 ข้อ & IPC บน Linux) | 1. ทดสอบกับ DAW จริงบน Windows (ดูคู่มือใน [docs/v2-phase1-HOWTO.md](v2-phase1-HOWTO.md))<br>2. สานต่องาน Phase 2 (App Audio echo 1.9, อัดเสียงเพื่อนแยกไฟล์ 1.10, เตือน buffer 1.11, UI metrics 1.12) ตาม [PLAN-v2-realtime-karaoke.md](PLAN-v2-realtime-karaoke.md)<br>3. โปรแกรม HAS Karaoke (.exe) ขั้น K0–K1 ตาม [PLAN-karaoke-exe.md](PLAN-karaoke-exe.md) |
| **`claude/elegant-gates-lgxsh8`** | 1 commit (`6abe8d9`) | **Push ขึ้น GitHub แล้ว** (ยังไม่ merge) | Share directory ปฏิเสธ in-memory store บน Netlify (ไฟล์ `web/site/server/share/redis.js`) เพื่อป้องกัน session หลุด: ตรวจสอบและเปิด PR เข้า `main` ([คลิกเปิด PR ที่นี่](https://github.com/ArmZOfficial/hearaside/compare/main...claude/elegant-gates-lgxsh8)) |
| `claude/great-albattani-rmb266` | 0 commit | Merge เข้า `main` ครบแล้ว | ไม่มีงานค้าง หากจะเริ่มงานใหม่ให้ sync จาก `main` ล่าสุด |

---

## 2. รายละเอียดงานในแต่ละ Branch

### Branch 1: `v2/phase1` (Audio Engine & Network v2 Phase 1)

เป็นงานใหญ่ที่มีการปรับปรุงสถาปัตยกรรม 3 ส่วนหลัก (รวม 38 commits):
1. **A: จับคู่เพื่อนเข้าแทร็ก DAW (Pairing)**
   - เลือกแทร็กที่เสียงเพื่อนจะออกทางหน้า Track ("Goes out through")
   - ปุ่ม "Pair automatically": Hub ส่งเสียง sweep สั้น ๆ ผ่านแทร็กเพื่อตรวจจับอัตโนมัติ (รองรับ 44.1 และ 48 kHz)
   - จำคู่การเชื่อมต่อไว้ในโปรเจกต์ DAW เมื่อเปิดโปรเจกต์ใหม่จะจับคู่ให้อัตโนมัติ
2. **B: Jitter Buffer แบบ Jamulus**
   - เมื่อเน็ตสะดุดสั้น ๆ ระบบจะ fade แล้วเล่นต่ออย่างนุ่มนวล ไม่ตัดเป็นเสียงเงียบกระตุกยาว
   - Buffer ขนาดเริ่มต้นเล็กลง (block + 20 ms) และเรียนรู้ latency จากเน็ตจริง
3. **C: เครือข่ายและหน้าเว็บ**
   - Hub แยก 2 เธรด (เธรดอ่านไมค์ / เธรดส่งเสียงให้เพื่อน) คิวส่งมีขอบเขต (bounded queue) ไมค์ไม่ค้างแม้เน็ตขาลงช้า
   - วัด RTT (Ping/Pong) และ kbps ต่อเพื่อน แสดงใน `Hub › Report`
   - หน้า `send.html` ไมค์เริ่มต้นเป็น Mono (มีสวิตช์เปิด Stereo สำหรับเครื่องดนตรี)

#### สิ่งที่ต้องการให้เพื่อนช่วยทำต่อใน `v2/phase1`:
- **ทดสอบกับ DAW จริงบน Windows**: อ่านขั้นตอนละเอียดใน [docs/v2-phase1-HOWTO.md](v2-phase1-HOWTO.md) (ทดสอบ Pair automatically, ฟังว่าเสียงสะดุดไหม, ดู RTT ใน Hub Report)
- **พัฒนารอบ 2 (Phase 2)**:
  - งาน 1.9: App Audio echo cancellation (กันเสียงตัวเองซ้อนจากระบบ)
  - งาน 1.10: อัดเสียงเพื่อนแยกไฟล์ (Per-friend separate track recording)
  - งาน 1.11 & 1.12: ตัวเตือน buffer underrun และ UI แสดงตัววัด network/jitter
  - แผนคาราโอเกะ K0–K1 ตาม [PLAN-karaoke-exe.md](PLAN-karaoke-exe.md)

---

### Branch 2: `claude/elegant-gates-lgxsh8` (Netlify Share Store Fix)

- **ไฟล์ที่แก้**: `web/site/server/share/redis.js`
- **ปัญหาเดิม**: บน Netlify functions แต่ละ instance มีหน่วยความจำของตัวเอง ทำให้การใช้ in-memory store ส่งผลให้ permanent link หลุดเสมือนห้อง offline
- **การแก้ไข**: ปฏิเสธ in-memory store บน Netlify (ให้ fail loudly แบบเดียวกับ Vercel เมื่อไม่มี Redis)
- **สิ่งที่ต้องทำต่อ**: ตรวจความถูกต้อง และเปิด PR รวมเข้า `main`

---

## 3. วิธีดึงโค้ดไปทำงานต่อ (สำหรับเพื่อน/Collaborator)

### ทำงานต่อใน `v2/phase1`:
```bash
git fetch origin
git checkout v2/phase1
git pull --ff-only

# Build และทดสอบบน Windows
.\build.ps1 -Test
```

### ทำงานต่อหรือตรวจใน `claude/elegant-gates-lgxsh8`:
```bash
git fetch origin
git checkout claude/elegant-gates-lgxsh8
git pull --ff-only
```
หรือเปิด PR เข้า `main` ผ่าน GitHub:
👉 https://github.com/ArmZOfficial/hearaside/compare/main...claude/elegant-gates-lgxsh8

---

## 4. แผนงานอ้างอิง

- **[docs/PLAN-v2-realtime-karaoke.md](PLAN-v2-realtime-karaoke.md)**: แผนงานหลัก Phase 0–7
- **[docs/PLAN-karaoke-exe.md](PLAN-karaoke-exe.md)**: แผนงานโปรแกรม HAS Karaoke K0–K6
- **[docs/v2-phase1-HOWTO.md](v2-phase1-HOWTO.md)**: คู่มือทดสอบ v2 Phase 1 ใน DAW
- **[CONTRIBUTING.md](../CONTRIBUTING.md)**: กฎการทำงานร่วมกันและข้อห้ามใน Audio Thread
