# เว็บบัญชีสำหรับปลั๊กอิน Rust

อัปเดต: 30 ตุลาคม 2026

สำหรับ Claude ที่ทำ `hs-net` / บัญชีใน Rust ต่อจาก C++ ใช้ HTTP contract เดิม ไม่ต้องรอเปลี่ยนเว็บ และไม่ต้องนำ Supabase SDK หรือ secret ฝั่ง server ไปไว้ในปลั๊กอิน

เอกสารนี้ครอบคลุมบัญชี แชร์ลิงก์ และ ICE credentials ของ `web/site` ส่วน REST ควบคุมเสียงบน Hub ในเครื่องเป็นอีกระบบ ดู [rest-api.md](rest-api.md) ห้ามเปลี่ยน shared memory, audio wire หรือ state ของ DAW ตามงานนี้

## เริ่ม local

```powershell
Set-Location 'E:\CandyCore\Hearaside\Hearaside plugin\web\site'
npm run dev:local
```

เว็บ: [หน้าภาษาไทย](http://localhost:5173/th), [Admin](http://localhost:5173/admin)

| บัญชี local | รหัสผ่าน | สิทธิ์ |
| --- | --- | --- |
| admin@example.test | HearasideLocal123! | ผู้ดูแล |
| member@example.test | HearasideLocal123! | สมาชิก |

บัญชีเหล่านี้สร้างเฉพาะ `server/dev.ts --memory --demo` ข้อมูลอยู่ใน RAM และหายเมื่อ API restart ไม่ใช้บัญชี production ไม่ส่งอีเมลหรือเรียก OAuth จริง

ให้ Rust client ใช้ base URL `http://127.0.0.1:3001` และให้ HTTP layer เติม `/api/v1` ตามแต่ละ endpoint เอง อย่าต่อ `/api/v1` สองครั้ง ฝั่ง C++ เดิมอ่าน `HEARASIDE_API_URL` เป็น base origin โดยไม่มี suffix; ฝั่ง Rust ควรคงชื่อ setting นี้

`SITE_URL=http://localhost:5173` ถูกตั้งให้ใน local เพื่อให้ลิงก์ยืนยันเปิดเว็บที่ Vite ไม่ใช่ API port

คำสั่งเทสต์ contract โดยไม่เปิด DAW:

```powershell
npm run test:plugin
```

ชุดนี้ใช้ Express + MemoryStore ของจริงผ่าน Supertest ครอบคลุม Hub/Track/App Audio, device authorization, ปฏิเสธ/หมดอายุ, refresh rotation, revoke, profile/settings, share directory และ TURN authentication ไม่ใช่การทดสอบ Rust binary หรือ audio transport จริง

## 1. ขอเข้าสู่ระบบ

```http
POST /api/v1/device/start
Content-Type: application/json
```

```json
{
  "client_info": {
    "plugin": "hub",
    "version": "2.0.0-rust",
    "os": "Windows 11",
    "daw": "Studio One",
    "computer": "Studio-PC"
  }
}
```

- `plugin` ต้องเป็น `hub`, `track`, `app` เท่านั้น อย่าเปลี่ยนเป็น `app-audio`
- ความยาวสูงสุดของ `version` 20, `os` 40, `daw` 40, `computer` 64 ตัวอักษร
- schema เป็น strict ห้ามแนบชื่อแทร็ก ไฟล์โปรเจกต์ หรือข้อมูลเสียง

ผลลัพธ์ 200:

```json
{
  "device_code": "<secret opaque value: 43 base64url characters>",
  "user_code": "ABCD-EFGH",
  "verification_uri": "http://localhost:5173/link",
  "verification_uri_complete": "http://localhost:5173/link?code=ABCD-EFGH",
  "expires_in": 600,
  "interval": 5
}
```

ค่าในวงเล็บแหลมเป็นคำอธิบาย ไม่ใช่ token ที่ใช้ทดสอบได้ `device_code` ต้องเก็บอยู่ใน client ห้ามใส่ URL, log หรือ clipboard แสดงเฉพาะ `user_code` และเปิด verification URI ที่ server ส่งกลับ

เว็บไม่อนุมัติหรือกรอกรหัสจาก query ให้อัตโนมัติ ผู้ใช้ต้องพิมพ์/วางรหัสจากปลั๊กอินเอง เมื่อครบ 8 ตัวเว็บจะ lookup และแสดงรายละเอียดให้ทันที ผู้ใช้ยังต้องตรวจชื่อปลั๊กอินและคอมพิวเตอร์ก่อนกดอนุญาต

หลัง login หรือยืนยันอีเมล เว็บจะกลับไป `next` หรือหน้าตั้งค่าทันที ไม่ต้องกดยืนยันการเข้าสู่ระบบซ้ำ ลิงก์ลืม/เปลี่ยนรหัสผ่านรักษา `next` และภาษาด้วย

## 2. รออนุมัติ

```http
POST /api/v1/device/token
Content-Type: application/json
```

```json
{ "device_code": "<the secret returned by device/start>" }
```

รออย่างน้อย `interval` วินาทีระหว่าง poll ใช้ worker/async task ห้ามเรียก HTTP บน audio callback

| HTTP / error | client ทำอะไรต่อ |
| --- | --- |
| 400 `authorization_pending` | รอตาม interval แล้ว poll ใหม่ |
| 400 `slow_down` | เพิ่ม interval อีก 5 วินาทีตามพฤติกรรม client เดิม |
| 400 `access_denied` | หยุดและแสดงว่าไม่อนุญาต |
| 400 `expired_token` | หยุด ขอรหัสใหม่เมื่อผู้ใช้สั่ง |
| 400 `invalid_grant` | รหัสไม่ถูกต้องหรือใช้แล้ว หยุด flow นี้ |
| 429 | ชะลอและลองใหม่ภายหลัง ห้ามวนยิงทันที |
| network / 5xx | retry แบบมี backoff ภายในอายุรหัส ห้ามถือว่าเข้าสู่ระบบสำเร็จ |

อนุมัติแล้วได้ 200:

```json
{
  "token_type": "Bearer",
  "access_token": "<opaque JWT>",
  "expires_in": 900,
  "refresh_token": "<opaque 43-character base64url token>",
  "device_id": "<UUID>",
  "user": {
    "id": "<UUID>",
    "email": "member@example.test",
    "emailConfirmed": true,
    "handle": "<server-generated handle>",
    "displayName": "Local Member",
    "about": "",
    "avatarUrl": null,
    "locale": "th",
    "hideEmail": true,
    "plan": "free",
    "providers": ["email"],
    "passwordChangedAt": "<ISO 8601 or null>",
    "createdAt": "<ISO 8601>"
  }
}
```

รหัสขออุปกรณ์ใช้ได้ครั้งเดียว การ poll พร้อมกันหลังอนุมัติต้องได้อุปกรณ์เดียว อย่าทำ polling หลาย task ใน client เดียว

## 3. ใช้และต่ออายุ token

request ที่ต้องมีสิทธิ์ใช้ `Authorization: Bearer <access_token>` อย่าส่ง refresh token เป็น Authorization

```http
POST /api/v1/token/refresh
Content-Type: application/json
```

```json
{ "refresh_token": "<current refresh token>" }
```

ผลลัพธ์มี `token_type`, `access_token`, `expires_in`, `refresh_token` ชุดใหม่ เก็บ token ใหม่ให้ครบอย่าง atomic ก่อนใช้คำตอบนี้ต่อ

สำคัญ: refresh token ใช้ได้ครั้งเดียว ถ้าเอาตัวเก่ากลับมา server ถือว่าอาจถูกขโมยและ revoke อุปกรณ์ ดังนั้น process ของ DAW หลายตัวที่แชร์บัญชีต้องล็อกข้าม process และอ่าน token ล่าสุดจาก storage ก่อน refresh แบบเดียวกับ C++ เดิม ห้าม retry POST refresh ด้วย token เดิมอย่างไม่มีเงื่อนไขหลัง network timeout

401 จาก `/me` หรือ refresh ต้องจัดการสถานะ signed-out ตาม error ไม่เปิด browser ใหม่โดยไม่มีคำสั่งผู้ใช้ เก็บ credential ด้วย OS-protected storage ตามสัญญา migration ห้าม log token

## 4. Profile, settings และอุปกรณ์

| Method / path | ข้อมูลและสิทธิ์ |
| --- | --- |
| GET `/api/v1/me` | profile ของบัญชีปัจจุบัน ใช้ plugin token ได้ |
| PATCH `/api/v1/me` | patch profile ตาม schema `profilePatch` |
| GET `/api/v1/me/settings` | appearance, stemNames, updatedAt |
| PUT `/api/v1/me/settings` | appearance และ stemNames เท่านั้น |
| GET `/api/v1/me/devices` | `{ devices: [...] }`, device ปัจจุบันมี `current: true` |
| PATCH `/api/v1/device` | `{ "name": "Studio PC" }`, plugin token เท่านั้น |
| POST `/api/v1/device/revoke` | ออกจากระบบอุปกรณ์ที่เป็นเจ้าของ token |
| GET/POST/DELETE `/api/v1/me/friends[/:id]` | เพื่อนที่บันทึกไว้ สูงสุด 50 คน ชื่อ 1–40 ตัวอักษร |

ตัวอย่าง PUT settings:

```json
{
  "appearance": {
    "language": "th",
    "theme": "dark",
    "uiScale": 1.25,
    "glassAlpha": 0.8,
    "hostColours": false,
    "reduceMotion": true
  },
  "stemNames": ["Vocal", "Backing"]
}
```

`uiScale` 0.75–2, `glassAlpha` 0–1; stemNames ไม่เกิน 8 ชื่อ ชื่อละไม่เกิน 32 ตัวอักษร อย่าส่ง gain, routing หรือ audio state ไป sync ข้ามเครื่อง

เว็บเป็นผู้อนุมัติรหัสและเพิกถอนอุปกรณ์จาก `/me/devices/:id` ปลั๊กอินไม่มีสิทธิ์อนุมัติรหัสอื่น ลบบัญชี ดาวน์โหลดข้อมูลส่วนตัว หรือเข้า `/admin/*` ด้วย plugin token เส้นทางเฉพาะเว็บตอบ 403 ส่วน Admin อาจตอบ 404 เมื่อไม่ได้เปิดหรือไม่มีสิทธิ์

## 5. Share directory และ ICE

ส่วนนี้ใช้ `/api` โดยไม่มี `/v1` เป็น contract เดิมของ Hub:

| Method / path | พฤติกรรม |
| --- | --- |
| POST `/api/register` | register และ heartbeat ทุก 30 วินาที |
| POST `/api/unregister` | ปิดแชร์ token ที่ระบุโดยใช้ owner secret |
| GET `/api/resolve/:token` | `{ online: true, ws: "wss://..." }` หรือ `{ online: false }` |
| GET หรือ POST `/api/turn` | ต้องมี plugin access token ที่อุปกรณ์ยังไม่ถูก revoke |

register body:

```json
{
  "tunnel": "https://example-studio.trycloudflare.com",
  "secret": "<64 lowercase hex characters; keep persistent on the Hub>",
  "tokens": {
    "l": "<20–64 lowercase letters/digits>",
    "s": "<20–64 lowercase letters/digits>",
    "f": ["<friend token of the same format>"]
  }
}
```

ใช้ HTTPS quick tunnel ของ cloudflared หรือ host ที่ server อนุญาต `f` สูงสุด 8 รายการ ห้ามส่ง arbitrary URL และห้ามส่งเสียงผ่านเว็บบัญชี

ผล register มี `ok`, `ttl` (90 วินาที), `tokens` และ `revoked` ถ้า heartbeat พบ token ใน `revoked` ต้องสร้างลิงก์ใหม่ อย่าส่ง token เดิมวนกลับมา การ signed-in register ใช้ Bearer plugin token เพื่อให้ลิงก์ไปปรากฏในบัญชี แต่แชร์แบบไม่ล็อกอินยังทำงานได้ตามเดิม

เว็บเก็บ hash ของลิงก์เท่านั้น `/me/links` จึงมี `url: null` ห้ามตีความเป็น API เสียหรือคาดหวังให้เว็บกู้ token กลับมา การ copy ลิงก์จริงทำจาก Hub ที่ถือ token

TURN response มี `iceServers` ใช้ตามคำตอบและไม่บันทึก credentials ถาวร ถ้า local ไม่มี Cloudflare TURN key จะได้ STUN fallback เท่านั้น ไม่ใช่หลักฐานว่าเชื่อมข้าม NAT ได้จริง เพดานปัจจุบัน 30 requests/นาที และ 500 requests/วันต่อ IP ใช้ร่วมกันทั้ง GET/POST

## JSON casing และความเข้ากันได้

- device/token และ token/refresh ใช้ snake_case เช่น `device_code`, `access_token`, `expires_in`
- profile/settings/device info ที่เว็บใช้มี camelCase เช่น `displayName`, `hideEmail`, `pluginVersion`, `updatedAt`
- อย่าใช้ rename policy เดียวกับ struct ทุกชนิดใน Rust ให้กำหนดชื่อ field ให้ตรง endpoint
- request schema ปฏิเสธ field ส่วนเกิน แต่ client ควรอ่าน response โดยยอมรับ field ใหม่ที่ไม่รู้จัก
- แหล่งจริงคือ `web/site/shared/schemas.ts`, `server/routes/device.ts`, `server/routes/me.ts`, `server/routes/share.ts` และ `server/share/directory.js`
- ไม่เปลี่ยน path/JSON/token format เพื่อให้ Rust สะดวก ถ้าต้องเพิ่ม protocol ให้คง client เดิมตามแผน migration หัวข้อ 3.4

## ก่อนต่อ production

1. ให้ client tests ผ่านกับ local และทดสอบเปิด/อนุมัติใน browser จริง
2. ตั้ง base URL เป็น origin ของ deployment ที่เจ้าของเลือกเท่านั้น ไม่ใช้ localhost ใน release
3. ฝั่งเว็บต้องมี Supabase, plugin signing secret, Redis และ TURN key ตามบริการที่ใช้ ค่า secret ไม่ไปอยู่ใน VITE variables หรือ binary
4. Email signup/reset ต้อง allow callback `/verify`, `/th/verify`, `/reset`, `/th/reset` พร้อม query string ใน Supabase Redirect URLs เพื่อเก็บ `next` หลังยืนยันอีเมลหรือเปลี่ยนรหัสผ่าน
5. ทดสอบจริงแยกต่างหากสำหรับ OAuth, อีเมล, TURN ข้าม NAT, DAW หลาย process และ secure token storage งานเว็บนี้ไม่ได้อ้างว่าทดสอบ Rust binary หรือ audio engine แล้ว
