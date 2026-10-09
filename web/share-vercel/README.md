# HEARASIDE share (Vercel): ลิงก์แชร์ถาวร

ลิงก์ฟังสดและลิงก์ส่งเสียงเข้าแบบ `https://<โปรเจกต์ของคุณ>.vercel.app/l/<token>` คงที่ต่อโปรเจกต์ DAW ส่งให้เพื่อนครั้งเดียวก็ใช้ได้ทุกไลฟ์ ไม่เปลี่ยนแม้ปิดเปิด DAW หรือ cloudflared ได้ที่อยู่ใหม่

```
Hub ──(1) เปิดแชร์ → cloudflared ได้ https://<สุ่ม>.trycloudflare.com
  └──(2) POST /api/register {tokens, tunnel, secret}   ทุก 30 วิ (หมดอายุใน 90 วิ)  ──► Vercel ──► Redis
เบราว์เซอร์ ── เปิด https://<site>/l/<token>  (หน้าเว็บ static)
  ├──(3) GET /api/resolve/<token> → {"online":true,"ws":"wss://<สุ่ม>.trycloudflare.com/ws/l/<token>"}
  └──(4) WebSocket ตรงไปที่ Hub ผ่าน cloudflared   ◄── เสียงทั้งหมดวิ่งเส้นนี้ ไม่ผ่าน Vercel
```

- **ไม่มีเสียงผ่าน Vercel** Vercel เก็บแค่ token → ที่อยู่ tunnel + เวลา (หมดอายุใน 90 วินาที) ไม่มีข้อมูลผู้ฟังและไม่มีเสียง
- **กันคนอื่นแย่งลิงก์**: ครั้งแรก Hub สร้าง secret 256 บิต เก็บในไฟล์โปรเจกต์ DAW ฝั่งเว็บเก็บแค่ SHA-256 ของ secret token ที่มีเจ้าของแล้วจะเขียนทับได้ด้วย secret ตัวเดิมเท่านั้น (ไม่งั้นได้ 403)
- **ใช้หลอกให้ไปเว็บอื่นไม่ได้**: รับเฉพาะ `https://<ชื่อ>.trycloudflare.com` (และโดเมนที่ตั้งใน `HEARASIDE_TUNNEL_HOSTS`) และ CSP ของหน้าเว็บให้ต่อ WebSocket ได้เฉพาะ `wss://*.trycloudflare.com`
- **Rate limit**: register 30 ครั้ง/นาที, resolve 240 ครั้ง/นาที ต่อ IP (ควรเปิด Vercel Firewall เพิ่มด้วย) token ที่ไม่มี หมดอายุ หรือผิดรูปแบบ ตอบ `{"online":false}` เหมือนกันหมด
- Hub ตรวจ `Origin` ของ WebSocket: ยอมเฉพาะเว็บแชร์ที่ตั้งไว้ ลิงก์ tunnel ของตัวเอง และที่อยู่ในเครื่อง/WiFi

## Deploy (ในบัญชี Vercel ของคุณเอง)

1. ติดตั้ง Node.js LTS แล้ว `npm i -g vercel`
2. `vercel login`
3. `cd web/share-vercel` แล้ว `vercel link` (สร้างโปรเจกต์ เช่น `my-hearaside`)
4. Vercel Dashboard › Storage / Marketplace › **Upstash for Redis** › Connect กับโปรเจกต์นี้ (ได้ env `UPSTASH_REDIS_REST_URL` / `UPSTASH_REDIS_REST_TOKEN` หรือ `KV_REST_API_*`)
   ถ้าจะทดสอบในเครื่องกับ Redis จริง: `vercel env pull .env.local` (ห้าม commit — อยู่ใน `.gitignore`)
5. `npm test` แล้ว `vercel deploy --prod`
6. ใน Hub › ตั้งค่า (เฟือง) › **ที่อยู่เว็บแชร์** ใส่ `https://my-hearaside.vercel.app` (หรือ build ด้วย `-DHEARASIDE_SHARE_BASE=https://...` ให้เป็นค่าเริ่มต้น)
7. (เสริม) ผูกโดเมนของตัวเองใน Project › Domains แล้วใส่โดเมนนั้นใน Hub แทน
8. เงื่อนไขแผน: Hobby เหมาะกับใช้ส่วนตัว ถ้าจะให้คนอื่นใช้เว็บแชร์ของคุณ (แจก/ขายปลั๊กอิน) ให้เช็คเงื่อนไขเชิงพาณิชย์ของ Vercel และ quota ของ Upstash ก่อน

ลิงก์เดิมของโปรเจกต์เก่า (token 12 ตัว) จะได้ token ใหม่ 26 ตัว (130 บิตจาก CSPRNG ของระบบ) ครั้งแรกที่เปิดแชร์พร้อมลิงก์ถาวร เพราะลิงก์จะอยู่บนอินเทอร์เน็ตนาน

## พัฒนาในเครื่อง

```bash
npm run sync      # คัดลอก listen.html / send.html จาก plugins/hub/web (ต้นฉบับเดียว ห้ามแก้ใน public/)
npm test          # node:test - register / heartbeat / owner / allowlist / TTL / rate limit
npm run dev       # http://127.0.0.1:3000 = Vercel จำลอง (store ในหน่วยความจำ)
```

E2E ในเครื่อง (ใช้ cloudflared จริง ไม่ต้องมีบัญชี Vercel):

```powershell
node scripts/dev-server.mjs 3911
ui-snapshot.exe --demo 120 --share-base http://127.0.0.1:3911 --state demo-state.bin
# เปิด http://127.0.0.1:3911/l/<token> กดฟัง แล้วปิด ui-snapshot และรันคำสั่งเดิมอีกครั้ง: หน้าเดิมเล่นต่อเองภายใน ~7 วินาที
```

ผลทดสอบ 9 ต.ค. 2026: ลงทะเบียนได้ภายใน 4.6 วินาทีหลังเปิด Hub, ได้ 160 เฟรมเสียงใน 3 วินาทีผ่าน tunnel ที่ resolve มา, Origin แปลกปลอมได้ 403, และหลังปิดเปิด Hub ผู้ฟังกลับมาได้ยินเองใน 7 วินาทีด้วยลิงก์เดิม
