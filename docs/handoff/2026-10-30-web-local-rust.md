# ส่งต่องานเว็บให้ Claude / Rust

วันที่: 30 ตุลาคม 2026 · branch `main` · ยังไม่ commit/push/deploy

## เริ่มจากตรงนี้

อ่าน [WEB-PLUGIN-CONTRACT.md](../WEB-PLUGIN-CONTRACT.md) เป็นสัญญาของเว็บบัญชี แล้วรัน:

```powershell
Set-Location 'E:\CandyCore\Hearaside\Hearaside plugin\web\site'
npm run dev:local
```

- เว็บ: http://localhost:5173/th
- Admin: http://localhost:5173/admin
- API base สำหรับปลั๊กอิน: http://127.0.0.1:3001
- บัญชี local: `admin@example.test` / `member@example.test`, รหัส `HearasideLocal123!`
- local เก็บข้อมูลใน memory และมีบัญชีผู้ดูแลเฉพาะโหมด `--memory --demo` อย่าใช้บัญชีนี้เป็น production credential

## สิ่งที่ทำแล้ว

แก้ 11 ข้อใน audit, ทำ local แบบไม่ต้องต่อ Supabase, ปรับ Admin/navbar/footer, ย่อข้อความ, เพิ่มภาพคู่มือจริง และทำ image viewer ในเว็บ รายละเอียดและหลักฐานอยู่ใน [follow-up](../../anti-slop/follow-up-001-2026-10-30.md)

คำขอล่าสุด: login/verify/reset กลับหน้าตั้งค่าหรือ next ทันที, device/link/friend lists sync ทุก 15 วินาทีและเมื่อ reconnect, Admin ทุก 30 วินาที, กรอกรหัสครบแล้ว lookup เองแต่ไม่ auto approve, dialog อยู่กลางจอ และ hero tilt กลับมาโดยไม่มี transform ตอนนิ่ง

ฝั่ง plugin ยังใช้ API เดิม `/api/v1/device/*`, `/token/refresh`, `/me`, `/me/settings` และ directory `/api/register`, `/api/resolve/:token`, `/api/turn` มี test contract สำหรับ `hub`, `track`, `app` รุ่น `2.0.0-rust` ที่ผ่าน flow จริงของ Express ไม่ได้สร้าง backend จำลองคนละ implementation

ไม่แตะโค้ดปลั๊กอิน C++/Rust หรือแผน migration งาน Rust, workflow และ integration tests ที่มีการแก้ใน working tree เป็นงานอีกส่วนที่มีอยู่ร่วมกัน ห้ามย้อนทับหรือรวมโดยไม่ตรวจ diff

ไฟล์หลักที่เปลี่ยน: `web/site/src`, `server/lib/limit.ts`, `server/routes/admin.ts`, `scripts/dev.mjs`, `server/dev.ts`, `scripts/copy-screens.mjs`, `package.json`, `playwright.config.ts`, `tests`, ภาพใน `public/screens` และเอกสารข้างต้น

## การตรวจที่ผ่าน

- TypeScript และ production build (SSR/prerender 20 หน้า)
- Unit/API 65 tests; subset สำหรับปลั๊กอิน 31 tests (`npm run test:plugin`)
- Playwright 21 ผ่าน, 1 ข้าม (`npm run test:e2e`), desktop + mobile touch/dark 320px; ข้าม hover บนมือถือ
- public layout/axe scan 36 กรณี ไม่มี page errors หรือ overflow ในกรณีที่ตรวจ

## ต่อ Rust อย่างไร

ใช้ base URL เป็น setting และคง `HEARASIDE_API_URL` ของ client เดิม อ่าน snake_case ใน token responses และ camelCase ใน profile/settings แยกกัน Poll ตาม interval, refresh แบบล็อกข้าม process และบันทึก token ที่หมุนใหม่อย่าง atomic ห้ามส่ง Supabase/server secrets เข้า binary และห้ามใช้ HTTP บน audio thread

งานถัดไปของ Rust คือเขียน client ให้ผ่าน contract นี้ แล้วทดสอบกับ binary/DAW จริง ยังไม่ได้อ้างว่ามี Rust client ที่เชื่อมเว็บสำเร็จแล้ว

ก่อน production ต้องตรวจ Supabase Redirect URLs ให้รองรับ `/verify`, `/th/verify`, `/reset`, `/th/reset` พร้อม query string, อีเมล/OAuth, TURN relay และ secure token storage จริง main JS chunk ยังมี warning เกิน budget 600 kB ซึ่งมีอยู่ก่อนงานนี้
