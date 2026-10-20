# Handoff: ทำงานต่อใน branch ที่ค้างอยู่

เอกสารนี้ให้เพื่อนที่เป็น collaborator ใน GitHub (ตอนนี้ `Sorryhxm` มีสิทธิ์ write) หยิบงานต่อได้โดยไม่ต้องถามเจ้าของ repo

## 1. Branch ที่มีอยู่ (ตรวจเมื่อ 20 ต.ค. 2026)

| Branch | ข้างหน้า main | สถานะ | ควรทำอะไร |
|---|---|---|---|
| `claude/elegant-gates-lgxsh8` | 1 commit (`6abe8d9`) | ยังไม่ merge: Share directory ปฏิเสธ in-memory store บน Netlify (ไฟล์ `web/site/server/share/redis.js`) | เปิด PR เข้า `main` แล้ว merge ได้เลย (แก้เล็ก) |
| `claude/great-albattani-rmb266` | 0 commit | เนื้อหา merge เข้า `main` ครบแล้ว (PR 1–3) | ถ้าจะทำต่อให้ reset จาก `main` ล่าสุดก่อน (ข้อ 3) |

> PR #1–#5 ปิดไปแล้ว ห้ามใช้ซ้ำ งานใหม่ = PR ใหม่

## 2. งานค้างหลัก

แผนอยู่ใน `docs/` และเป็น "แผนหลักฉบับเดียว" ให้แก้ตารางสถานะที่เดิม:

- [PLAN-v2-realtime-karaoke.md](PLAN-v2-realtime-karaoke.md): Phase 0–7 (จับคู่เพื่อน, jitter buffer, Opus, กันเสียงสะท้อน, UDP, โหมดคาราโอเกะ)
- [PLAN-karaoke-exe.md](PLAN-karaoke-exe.md): โปรแกรม HAS Karaoke (.exe) ขั้น K0–K6

ลำดับที่แนะนำในแผน: Phase 0+1 → Phase 2 → Phase 3 → Phase 5 (ขนานกันได้) → Phase 4 ; และ K0 → K1 ก่อน

### ข้อควรรู้: ตารางสถานะใน repo ล้าหลังงานจริง
session วางแผนบนเครื่องเจ้าของ repo ทำไปไกลกว่านี้ (Builder B jitter buffer / วัดผล, Builder C เกรดเครือข่าย + หน้าเว็บ, รอบ 2: Builder D App Audio echo + ตัวเตือน buffer + UI ตัววัด, HAS Karaoke K0–K1, การแก้หลังรีวิว) แต่ **โค้ดเหล่านั้นยังไม่ถูก push ขึ้น GitHub** (ตรวจแล้ว: remote มีแค่ `main` กับ 2 branch ด้านบน)
เจ้าของ repo ต้อง push branch จากเครื่องก่อน เช่น

```
git push -u origin <ชื่อ-branch-บนเครื่อง>
```

แล้วบอกชื่อ branch ให้เพื่อน จากนั้นเพื่อนทำงานต่อบน branch นั้นได้เลย

## 3. วิธีเริ่มทำงาน (เพื่อน)

```
git clone https://github.com/ArmZOfficial/hearaside.git && cd hearaside
git fetch origin
git checkout claude/elegant-gates-lgxsh8      # หรือ branch งานต่อ
git pull --ff-only
# ... แก้ไข ...
git push origin HEAD
```

reset branch ที่ merge ไปแล้ว (ทำเฉพาะ branch ที่ไม่มีงานค้าง เช่น `great-albattani`):

```
git fetch origin main
git checkout -B claude/great-albattani-rmb266 origin/main
git push --force-with-lease -u origin claude/great-albattani-rmb266
```

ดูกติกาใน [CONTRIBUTING.md](../CONTRIBUTING.md)
