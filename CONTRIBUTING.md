# Contributing

สำหรับ collaborator ที่มีสิทธิ์ write ใน repo นี้ (สถานะงานค้างดูที่ [docs/HANDOFF.md](docs/HANDOFF.md))

1. ทำงานใน branch ไม่ใช่ `main` ตรง ๆ จะต่อ branch เดิมหรือแตกใหม่จาก `main` ก็ได้
2. คุยกันก่อนถ้าจะแก้ branch ที่คนอื่นกำลังทำ และอย่า force-push branch ที่คนอื่นใช้อยู่ (ใช้ merge แทน rebase)
3. แผนอยู่ที่ `docs/PLAN-v2-realtime-karaoke.md` และ `docs/PLAN-karaoke-exe.md` ปิดงานแล้วให้อัปเดตตารางสถานะในไฟล์นั้น
4. เปิด PR เข้า `main` ให้อีกคนดูก่อน merge ห้ามเปิดซ้ำบน PR ที่ merge/ปิดไปแล้ว
5. Build: ดู `README.md` และ `build.ps1` (ต้องใช้ Windows + `scripts/fetch-deps.ps1`) ห้าม commit ของใน `external/JUCE`, `build/`, `dist/`
6. ห้ามใส่ token, key หรือ secret ลง repo
