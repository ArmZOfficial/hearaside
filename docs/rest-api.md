# HEARASIDE REST API

สั่ง Hub จากโปรแกรมในเครื่องเดียวกันได้ เช่น Stream Deck, สคริปต์, bot แชต หรือ Companion

- เปิดที่ Hub › ตั้งค่า (เฟือง) › **REST API** แล้วกด **คัดลอก API key**
- ที่อยู่: `http://127.0.0.1:47800/api/v1` (ถ้าพอร์ตไม่ว่างจะใช้ 47801–47809 ตามที่แสดงในตั้งค่า)
- รับ/ส่ง JSON ทุกคำขอต้องมี `Authorization: Bearer <API key>`
- ฟังเฉพาะ `127.0.0.1` และตรวจ `Host` ทุกคำขอ: เครื่องอื่นในเน็ตเวิร์กต่อไม่ได้ และเว็บเพจในเบราว์เซอร์ก็สั่งไม่ได้ เพราะคำขอที่มี `Origin` และ CORS preflight จะถูกปฏิเสธ
- ทุกคำสั่งทำผ่านพารามิเตอร์ของ Hub หรือ mailbox ของ Track เหมือนกดในหน้าจอ DAW จึง undo, automate และ save ได้ตามปกติ
- ตอบเฉพาะ Hub ที่เป็นเจ้าของ bus (Hub ตัวที่สองบน bus เดียวกันไม่เปิด API)

## Endpoints

| Method | Path | Body | ผล |
|---|---|---|---|
| GET | `/api/v1` | — | รายการ endpoint |
| GET | `/api/v1/state` | — | สถานะ Hub + ทุกแทร็ก + App Audio |
| POST | `/api/v1/mute` | `{"on": true}` | ตัดเสียงคนดู (ไม่ใส่ `on` = สลับ) |
| POST | `/api/v1/preview` | `{"on": true}` | ฟังแบบคนดู |
| POST | `/api/v1/limiter` | `{"on": true}` | กันเสียงพีค |
| POST | `/api/v1/stream` | `{"db": -3}` | ระดับรวมไปหาคนดู |
| POST | `/api/v1/headphones` | `{"db": -6}` | ระดับหูฟังรวม |
| GET | `/api/v1/tracks` | — | รายการแทร็ก |
| POST | `/api/v1/tracks/<slot หรือชื่อ>` | `{"you_hear": false, "viewers_hear": true, "headphone_db": -6, "viewers_db": 0, "delay_ms": 40, "solo": false}` | ใส่เฉพาะช่องที่อยากเปลี่ยน |
| GET | `/api/v1/sources` | — | App Audio ทุกตัว |
| POST | `/api/v1/sources/<index หรือชื่อ>` | `{"on": true, "you_hear": true, "viewers_hear": false, "headphone_db": 0, "viewers_db": -3, "record": true}` | |
| POST | `/api/v1/sync` | `{"action": "start"}` หรือ `"cancel"` | ซิงค์เสียงร้องอัตโนมัติ |
| POST | `/api/v1/share` | `{"on": true}` | เปิด/ปิดแชร์ลิงก์ |

ค่า boolean ทุกตัว (`on`, `you_hear`, `viewers_hear`, `solo`, `record`) ถ้าส่ง `null` จะเป็นการสลับค่าปัจจุบัน ส่วนค่า dB จะถูกจำกัดให้อยู่ในช่วงของพารามิเตอร์เอง

รหัสตอบกลับ: `200` สำเร็จ · `400` body ผิด · `401` key ผิดหรือไม่มี · `403` ไม่ได้มาจากโปรแกรมในเครื่องนี้ · `404` ไม่มีแทร็ก/path นี้ · `503` DAW ไม่ตอบภายใน 2 วินาที

ตัวอย่าง `GET /api/v1/state`:

```json
{
  "hub": { "stream_muted": false, "hearing_viewers_mix": false, "stream_db": 0.0, "headphones_db": 0.0,
           "peak_protection": true, "loudness_lufs": -16.2, "obs_connected": true, "latency_to_obs_ms": 23.4,
           "daw_buffer": 256, "sharing": true, "listen_link": "https://my-hearaside.vercel.app/l/…" },
  "tracks": [ { "slot": 0, "name": "Vox", "active": true, "you_hear": false, "viewers_hear": true,
                "headphone_db": 0.0, "viewers_db": 0.0, "delay_ms": 0.0, "solo": false, "bypassed": false } ],
  "sources": [ { "index": 0, "name": "App Audio", "program": "brave.exe", "on": true, "active": true, "you_hear": true,
                 "viewers_hear": true, "headphone_db": 0.0, "viewers_db": 0.0, "delay_ms": 120.0, "recording": false } ]
}
```

## ตัวอย่าง

PowerShell:

```powershell
$h = @{ Authorization = "Bearer <API key>" }
Invoke-RestMethod http://127.0.0.1:47800/api/v1/state -Headers $h
Invoke-RestMethod http://127.0.0.1:47800/api/v1/mute -Method Post -Headers $h -ContentType application/json -Body '{}'
```

curl (Windows 10+ มีให้แล้ว):

```bash
curl -s -H "Authorization: Bearer <API key>" http://127.0.0.1:47800/api/v1/state
curl -s -X POST -H "Authorization: Bearer <API key>" -d "{\"you_hear\": false}" http://127.0.0.1:47800/api/v1/tracks/Vox
```

Python:

```python
import requests
api = "http://127.0.0.1:47800/api/v1"
h = {"Authorization": "Bearer <API key>"}
print(requests.get(f"{api}/state", headers=h).json()["hub"]["loudness_lufs"])
requests.post(f"{api}/preview", headers=h, json={"on": True})
```

**Stream Deck:** ใช้ปลั๊กอินที่ยิง HTTP request ได้ (เช่น "API Ninja" หรือ "Web Requests") ตั้ง Method `POST`, URL `http://127.0.0.1:47800/api/v1/mute`, Header `Authorization: Bearer <key>`, Body `{}` (สลับ) — หรือใช้ hotkey ของ OBS ที่มีอยู่แล้ว

## ทดสอบ

`ctest -R rest_api` (= `ui-snapshot --test-rest`): ตรวจ 401, 403 (Origin และ Host ปลอม), state, mute, คำสั่งถึงแทร็กผ่านพารามิเตอร์ของ host, 404, 400
