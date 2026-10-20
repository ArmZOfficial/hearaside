# Contributing

สำหรับ collaborator ที่มีสิทธิ์ write ใน repo นี้ (สถานะงานค้างและขั้นตอนทำต่อดูที่ [docs/HANDOFF.md](docs/HANDOFF.md))

1. **ทำงานใน branch ไม่ใช่ `main` ตรง ๆ**: จะต่อ branch เดิมหรือแตกใหม่จาก `main` หรือ `v2/phase1` ก็ได้
2. **ห้าม Force Push branch ที่มีคนอื่นใช้อยู่**: คุยกันก่อนถ้าจะแก้ branch เดียวกัน และใช้ merge แทน rebase เมื่อซิงก์สายงานร่วมกัน
3. **แผนงานและตารางสถานะ**: อยู่ใน [docs/PLAN-v2-realtime-karaoke.md](docs/PLAN-v2-realtime-karaoke.md) และ [docs/PLAN-karaoke-exe.md](docs/PLAN-karaoke-exe.md) เมื่องานไหนเสร็จให้อัปเดตสถานะในตาราง
4. **เปิด PR เข้า `main` หรือ parent branch**: ให้อีกคนตรวจ/รีวิวก่อน merge เสมอ (ห้ามเปิดซ้ำบน PR ที่ปิดไปแล้ว)
5. **กติกา Audio Thread (สำคัญมาก)**: ใน `processBlock`, `HubEngine::process*`, `FriendReader::pull` ห้าม lock (mutex), ห้าม allocate memory, ห้าม log และห้าม syscalls เด็ดขาด
6. **Build & Test**:
   - Windows: รัน `.\build.ps1 -Test` (ต้องใช้ Windows + JUCE ตามที่ `scripts/fetch-deps.ps1` เตรียมไว้)
   - ห้าม commit โฟลเดอร์ `external/JUCE`, `build/`, `dist/`
7. **ความปลอดภัย**: ห้ามใส่ token, private key หรือ secret ใด ๆ ลงใน git เด็ดขาด
