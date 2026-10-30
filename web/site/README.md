# ทดสอบเว็บไซต์ในเครื่อง

ต้องมี Node.js 20.19 ขึ้นไป เปิด PowerShell ในโฟลเดอร์นี้แล้วรัน:

```powershell
npm ci
npm run dev:local
```

ถ้ามี `node_modules` อยู่แล้วและ dependencies ไม่เปลี่ยน ข้าม `npm ci` ได้

- เว็บภาษาไทย: http://localhost:5173/th
- หลังบ้าน: http://localhost:5173/admin
- API สำหรับปลั๊กอิน: http://127.0.0.1:3001/api/v1
- ตรวจ API ผ่านเว็บ: http://localhost:5173/api/v1/health

บัญชีทดสอบ:

| สิทธิ์ | อีเมล | รหัสผ่าน |
| --- | --- | --- |
| Admin | admin@example.test | HearasideLocal123! |
| สมาชิก | member@example.test | HearasideLocal123! |

คำสั่งนี้ใช้ข้อมูลใน memory แยกจาก production ไม่ต้องตั้งค่า Supabase หรือ CAPTCHA ไม่ส่งอีเมล และไม่เปิด OAuth จริง สมัครบัญชีทดสอบแล้วเข้าสู่ระบบด้วยอีเมล/รหัสผ่านได้ทันที ข้อมูลรีเซ็ตเมื่อ API เริ่มใหม่ รวมถึงเมื่อแก้ไฟล์ server ใน watch mode

ทดสอบเชื่อมปลั๊กอินด้วย device flow: ให้ client ทดสอบเรียก `/api/v1/device/start` แล้วเปิด `verification_uri` ในผลลัพธ์ เข้าสู่ระบบและพิมพ์รหัสที่ client แสดง จากนั้น client เรียก `/device/token` ตาม interval ที่ได้รับ ห้ามใส่ device_code ลงใน browser URL

ปลั๊กอินรองรับ environment variable `HEARASIDE_API_URL` อยู่แล้ว ตั้งค่าใน PowerShell แล้วเปิด DAW จากหน้าต่างเดียวกันเพื่อให้ DAW และปลั๊กอินรับค่า:

```powershell
$env:HEARASIDE_API_URL = 'http://127.0.0.1:3001'
# เปิด DAW จาก PowerShell นี้ด้วย path ของ DAW ที่ติดตั้งไว้
```

ปิด DAW เดิมก่อนเปิดใหม่ หลังทดสอบให้ปิด DAW แล้วใช้ `Remove-Item Env:HEARASIDE_API_URL` ก่อนเปิด DAW อีกครั้ง คำสั่งนี้มีผลเฉพาะ process ที่รับ environment นี้ ไม่เปลี่ยนปลายทาง production ถาวร

สำหรับ Supabase จริง ให้เพิ่ม callback `/verify`, `/th/verify`, `/reset`, `/th/reset` (รวม query string) ของ origin ที่ใช้ในรายการ Redirect URLs ข้อมูล `next` จะรักษาปลายทางหลังยืนยันอีเมลหรือเปลี่ยนรหัสผ่าน

กด Ctrl+C เพื่อหยุดทั้งเว็บและ API ส่วน `npm run dev` เดิมยังใช้ `.env.local` สำหรับการพัฒนากับบริการที่ตั้งค่าไว้

ทดสอบอัตโนมัติ:

```powershell
npm test
npm run test:plugin
npm run test:e2e
```

E2E เปิด local ให้เองเมื่อยังไม่ได้รัน และตรวจทั้ง desktop กับจอ 320px ธีมมืด บน Windows ใช้ Microsoft Edge ที่ติดตั้งอยู่ บนระบบอื่นติดตั้ง Chromium ด้วย `npx playwright install chromium` ก่อน

สัญญา API สำหรับ Claude ที่ต่อปลั๊กอิน Rust: [WEB-PLUGIN-CONTRACT.md](../../docs/WEB-PLUGIN-CONTRACT.md)
