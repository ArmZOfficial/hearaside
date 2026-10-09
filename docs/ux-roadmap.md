# HEARASIDE: UX audit + แผนงาน (ต.ค. 2026)

ไฟล์นี้คือผลของ prompt `HEARASIDE-UX-Automation-Prompt.md`: รายงาน audit (Phase 0), แผนงานทุกข้อ และสถานะของงานแต่ละข้อ

- snapshot ก่อนแก้: `ui-snapshots/audit-before/` · หลังแก้: `ui-snapshots/audit-after/`
- สร้างใหม่ได้ด้วย `build\Release\plugins\hearaside_ui_snapshot_artefacts\Release\ui-snapshot.exe --audit <โฟลเดอร์>`
  (matrix: Hub 6 ขนาด × ไทย/อังกฤษ × สว่าง/มืด, Track/App 2 ขนาด, แผงย่อยทุกแผง, สถานะ: ว่าง, 40+ แทร็ก + ชื่อยาวมาก, ตัดเสียง + ฟังแบบคนดู, Hub ซ้ำ)
- ตัวตรวจ layout อัตโนมัติใน `--audit`: component ที่มองเห็นห้ามทับกันและห้ามล้นพ่อ ผลอยู่ใน `layout-report.txt` (exit 1 ถ้าเจอปัญหา)

---

## 1. Audit (ก่อนแก้)

### 1.1 ผลตัวตรวจ layout

| หน้าจอ / ขนาด content | ปัญหาที่ตรวจเจอ |
|---|---|
| Hub ค่าเริ่ม 1040×790 | 0 (ชื่อแทร็กเหลือ ~90 px: "ดนต...", "เมโทรนอม / ไ...") |
| Hub **ขนาดขั้นต่ำของตัวเอง** 880×750 | 19: คอลัมน์ชื่อกว้าง ≤ 0 px → **ชื่อแทร็กหายทุกแถว**, ปุ่ม ⋯ ทับ pill, badge "40 ms" ล้นออกนอกแถว, คำอธิบายใต้หัวการ์ดถูกปุ่มซิงค์ทับ |
| Hub 1000×560 (โน้ตบุ๊ก) | 6 + การ์ดสรุปถูกบีบ: สไลเดอร์ระดับหูฟังรวมทับข้อความ "คุณได้ยินในหูฟัง" |
| Hub 560×480 / 400×640 | 88–89 / 7: header ซ้อนกัน (wordmark, chip DAW, chip OBS), pill ล้นออกนอกแถว, หัวคอลัมน์ทับกัน |
| Hub 40+ แทร็ก ที่ 560×480 | 544 |
| Track 380×520 | 0 จากตัวตรวจ แต่ประโยคสรุป (วาดเอง) ทับป้ายสไลเดอร์ "ระดับในหูฟัง" |
| App Audio 460×480 | 0 แต่คำอธิบายการอัดถูกตัดท้าย |
| แผงย่อย (แชร์, ตั้งค่า, ซิงค์, ตั้งค่าละเอียด) | 0 ที่ขนาดตายตัว แต่ไม่ย่อตามหน้าต่าง (แชร์ 460×510, ตั้งค่า 396×540) |

### 1.2 หน้าต่างพอดีจอไหม (หน่วย logical px หลังหักทาสก์บาร์ ~48 และกรอบหน้าต่างปลั๊กอินของ DAW ~32)

| จอ (Windows scaling) | พื้นที่ใช้ได้ | Hub ขั้นต่ำ 880×750 @UI 100 % | @125 % (1100×938) | Track 380×664 (ย่อไม่ได้) |
|---|---|---|---|---|
| 1366×768 (100 %) | 1366×688 | ⛔ สูงเกิน 62 | ⛔ | ✅ |
| 1366×768 (125 %) | 1093×534 | ⛔ สูงเกิน 216 | ⛔ | ⛔ ล่างหาย 130 px และย่อไม่ได้ |
| 1920×1080 (125 %) | 1536×784 | ✅ | ⛔ | ✅ |
| 1920×1080 (100 %) | 1920×1000 | ✅ | ✅ | ✅ |

### 1.3 ตารางปัญหา

| ปัญหา | หน้าจอ | ความรุนแรง | ผลกับผู้ใช้ | ข้อเสนอ | งาน |
|---|---|---|---|---|---|
| Hub ขั้นต่ำ 880×750 ล้นจอโน้ตบุ๊ก | Hub | สูง | เห็นไม่ครบ ปุ่มตัดเสียง/การ์ดสรุปอาจตกจอ | breakpoint Compact/Regular/Wide + min ~420×460 | M (7) |
| ชื่อแทร็กหายที่ขนาดขั้นต่ำ | Hub | สูง | ไม่รู้ว่าแถวไหนคือแทร็กอะไร = ไม่รู้ "ใครได้ยินอะไร" | แถว 3 ระดับ (กว้าง/กลาง/แคบ) ชื่อมีที่อย่างน้อย 120 px | M (7) |
| การ์ดสรุปถูกบีบจนทับกันเมื่อหน้าต่างเตี้ย | Hub | สูง | อ่านสรุปไม่ได้ | สรุปแบบแถบเดียวขยายได้เมื่อความสูงไม่พอ | M (7) |
| Track/App ย่อไม่ได้ | Track, App | สูง | 1366×768@125 % ล่างหาย (สไลเดอร์, ดีเลย์) | resize แนวตั้ง + เลื่อนได้ | M (8) |
| ประโยคสรุปทับสไลเดอร์ใน Track เมื่อเตี้ย | Track | กลาง | อ่านไม่ออก | layout จากบนลงล่าง + scroll | M (8) |
| แผงย่อยขนาดตายตัว | ทุกแผง | กลาง | ที่ UI 150–200 % แผงใหญ่กว่าจอ | ขนาดตามเนื้อหา ≤ หน้าต่างแม่ − 32 + scroll | M (8) |
| UI scale ไม่ดูขนาดจอ | ทุกหน้า | กลาง | เปิดครั้งแรกที่ 200 % บนโน้ตบุ๊กล้นจอ | ครั้งแรกเลือก scale ให้ ≤ 85 % ของจอ | S (5) |
| ไม่จำขนาดหน้าต่าง | ทุกหน้า | ต่ำ | ต้องลากขยายใหม่ทุกครั้ง | จำขนาดต่อปลั๊กอินใน Settings + state | S (5) |
| ข้อความยาวรวมทุก DAW | App, Sync, Track footer | กลาง | อ่านยาก หาขั้นตอนของ DAW ตัวเองไม่เจอ | แตกเป็นขั้นตอนมีหมายเลข เฉพาะ DAW ที่ตรวจพบ | S (1) |
| Empty state ประโยคเดียว | Hub | กลาง | มือใหม่ไม่รู้ขั้นต่อไป | เช็คลิสต์ 3 ขั้นที่ติ๊กเองจากสถานะจริง | S (2) |
| token `muted` 3.1:1 | ทุกหน้า | ต่ำ | ถ้าใช้กับข้อความจะอ่านยาก | test contrast ≥ 4.5:1 (ยกเว้น disabled) | S (4) |
| OBS Source ไม่ปิด Monitoring เอง | OBS | สูง | ได้ยินเสียงซ้ำในหูฟัง (ช้ากว่า) | ตั้ง Monitor Off ตอนสร้าง Source | S (6) |
| ลิงก์แชร์เปลี่ยนทุกครั้ง | แชร์ | สูง | ต้องส่งลิงก์ใหม่ทุกไลฟ์ | ลิงก์ถาวรผ่าน Vercel (หัวข้อ 8) | M (13) |
| WebSocket ไม่ตรวจ Origin, token 60 บิตจาก PRNG | แชร์ | กลาง (ความปลอดภัย) | เว็บอื่นต่อเข้ามาได้, เดา token ง่ายกว่าที่ควร | ตรวจ Origin + token 128 บิตจาก CSPRNG | M (13) |
| ไม่มีที่ดูว่าระบบพร้อมไหม | Hub | สูง | แก้ "คนดูไม่ได้ยิน/ได้ยินซ้ำ" เองไม่ได้ | Setup Check | M (9) |
| คนดูเงียบโดยไม่รู้ตัว | Hub | สูง | ไลฟ์เงียบทั้งที่คิดว่ามีเสียง | ตรวจ Stream Mix เงียบขณะ DAW เล่น + OBS เชื่อม | M (10) |

### 1.4 User journey (นับจากโค้ดและข้อความปัจจุบัน)

| Journey | คลิก/ขั้นตอน | การตัดสินใจ | ศัพท์เทคนิคที่เจอ | จุดติด |
|---|---|---|---|---|
| ติดตั้ง → ไลฟ์ Recipe A (ร้อง cover) | ~14 (ติดตั้ง, Track ×2 แทร็ก, Hub บน Master, OBS เพิ่ม Source, ตั้ง Monitor Off, ปิด Desktop Audio, ปิด "คุณได้ยิน" แทร็กร้อง, ฟังแบบคนดูเช็ค) | 5 | Insert, FX chain, Master bus, Source, Monitoring, Desktop Audio | ไม่มีอะไรบอกว่าต้องตั้ง Monitoring/Desktop Audio ใน OBS (อยู่ใน README เท่านั้น) |
| เพิ่ม YouTube ด้วย App Audio + อัด | ~12 (Studio One: Song Setup › Inputs › Add, Console, ใส่ปลั๊กอิน, เฉพาะเสียงโปรแกรม, ตั้ง input แทร็ก, กดอัด) | 4 | Input channel, Song Setup, Audio I/O | ขั้นตอนอยู่ในย่อหน้าเดียวรวม 3 DAW |
| "คนดูได้ยินซ้ำ" | ไม่มีเครื่องมือ | — | Desktop Audio, Monitoring | ต้องรู้เองว่าดูที่ OBS |
| "คนดูไม่ได้ยินอะไร" | ไม่มีเครื่องมือ (มีแค่ chip OBS) | — | — | Hub ไม่เตือนเมื่อ Stream Mix เงียบ |
| "เสียงร้องไม่ตรงจังหวะ" | 1 คลิก (ซิงค์อัตโนมัติ) | 1 | latency, buffer | ดี: บันทึก autosync.log ให้เอง |
| แชร์ลิงก์ → ปิด/เปิด DAW → เปิดลิงก์เดิม | เปิดแชร์ 2 คลิก, คัดลอก 1 | 1 | cloudflared | **ลิงก์เดิมใช้ไม่ได้** ต้องส่งใหม่ทุกครั้ง |

---

## 2. แผนงานและสถานะ

สถานะ: ✅ ทำแล้วในรอบนี้ · ◐ ทำบางส่วน · ⏳ ยังไม่ทำ (มีแผน)

### สรุปผล

| # | งาน | ขนาด · ระดับ | สถานะ |
|---|---|---|---|
| 1 | ข้อความยาวแตกเป็นขั้นตอน + เฉพาะ DAW ที่ตรวจพบ | S · A | ✅ |
| 2 | Empty state เช็คลิสต์ที่ติ๊กเอง | S | ✅ |
| 3 | ข้อความ error/warning 3 ส่วน + ปุ่มแก้ | S | ◐ (ตรวจระบบ + banner ใหม่ทั้งหมด; ข้อความเดิมยังไม่ได้ไล่ครบ) |
| 4 | Contrast test + accessibility titles | S | ✅ (focus ring มีอยู่แล้ว ไม่ได้แก้) |
| 5 | จำขนาดหน้าต่าง + UI scale พอดีจอครั้งแรก | S · A | ✅ (จำใน Settings ต่อปลั๊กอิน ยังไม่เก็บใน state ของโปรเจกต์) |
| 6 | OBS Source ปิด Monitoring เอง | S · B | ✅ |
| 7 | Hub Compact / Regular / Wide + ลด min size | M | ✅ |
| 8 | Track / App Audio resize ได้ + แผงย่อยตามเนื้อหา | M | ✅ |
| 9 | Setup Check (ตรวจระบบ) | M | ◐ (9 รายการจากสถานะจริง; ข้อที่ต้องให้ OBS รายงานกลับ เช่น Desktop Audio ซ้ำ ยังไม่ทำ) |
| 10 | ตรวจคนดูไม่ได้ยินอะไร / พีค / latency เปลี่ยน | M · B | ◐ (คนดูไม่ได้ยินอะไร ✅, พีคและ latency เปลี่ยน ⏳) |
| 11 | คัดลอกรายงานปัญหา + รายงานระบบ | M | ◐ (คัดลอกรายงาน ✅, log เหตุการณ์ 50 รายการ ⏳) |
| 12 | ค้นหา/กรองแทร็ก, Live mini view | M | ⏳ (Compact ที่ 420×460 ใช้แทน mini view ได้ส่วนหนึ่ง) |
| 13 | ลิงก์แชร์ถาวรผ่าน Vercel | M · A | ✅ |
| + | REST API ควบคุม Hub (ผู้ใช้ขอเพิ่ม) | M · C (ผู้ใช้สั่งเอง) | ✅ |
| 14–18 | Recipe, แนะนำค่าจากชื่อ, เป้าความดัง/ducking, feedback/เสียงซ้ำใน OBS, installer + QR | L | ⏳ |

ผลวัดได้: ตัวตรวจ layout จาก 1,265 จุด → 0 (81 ภาพ); Hub ขั้นต่ำจาก 880×750 → 420×460 (ปุ่ม "ตัดเสียงคนดู" อยู่ใน header เสมอ); Track 380×664 ย่อไม่ได้ → ย่อแนวตั้งถึง 460; test 5 → 10 ชุด (`.\build.ps1 -Test`)

---

### [1] ข้อความยาวเป็นขั้นตอน เฉพาะ DAW ที่ใช้อยู่ (S · A) ✅
ปัญหา: `AppHint`, `SyncHow`, `LatencyTip` เป็นย่อหน้ายาวรวมทุก DAW
แนวทาง: `plugins/common/Host.h` ตรวจ DAW ด้วย `juce::PluginHostType` (และ `$HEARASIDE_DAW` สำหรับ snapshot/test); App Audio แสดงขั้นตอนมีหมายเลขของ DAW นั้น + ปุ่ม "ดูวิธีของ DAW อื่น"; SyncHow / LatencyTip เป็นบรรทัดละขั้น; เช็คลิสต์เริ่มใช้งานบอกวิธีใส่ Track ของ DAW นั้น
ไฟล์ที่แตะ: `Host.h`, `Strings.*`, `AppAudioProcessor.cpp`, `HubEditor.cpp`
ข้อความใหม่: `AppStepsStudioOne/Cubase/Reaper/Other`, `AppHintNormal`, `StepsOtherDaws`, `StepsThisDaw`, `StartStep1StudioOne/Cubase/Reaper/Fl/Ableton`, `SyncTip`
Real-time: ไม่แตะ · ความเสี่ยง: ขั้นตอนของ Cubase/Reaper เขียนจากความรู้ทั่วไป ควรให้ผู้ใช้ DAW นั้นตรวจ
ทดสอบ: snapshot `app-*` · เกณฑ์ผ่าน: ไม่มีย่อหน้าที่รวมหลาย DAW ในหน้าหลัก

### [2] Empty state เช็คลิสต์ (S) ✅
ปัญหา: "ยังไม่มีแทร็ก…" ประโยคเดียว · แนวทาง: การ์ด "เริ่มใช้งาน 3 ขั้น" (ใส่ Track · เพิ่ม Source ใน OBS · กดฟังแบบคนดู) ติ๊กเองจากสถานะจริง (มีแทร็ก, `obsConnected()`, เคยกด preview จำใน Settings) ซ่อนได้เมื่อมีแทร็กแล้ว และหายเองเมื่อครบ
ไฟล์: `HubEditor.*`, `Settings.h` (flag) · ข้อความใหม่: `StartTitle`, `StartStep1–3`, `StartHide` · ทดสอบ: snapshot `state-empty-*`

### [4] Contrast test (S) ✅
`design/tokens.json` › `contrast`: ทุก token ข้อความ (`ink`, `ink2`, `graphite`, `offFg`) บนทุกพื้น (paper, glass, inset, offBg, chip ซ้อนตามจริง) ต้อง ≥ 4.5:1 ทั้งสองธีม, `muted` ใช้ได้เฉพาะ disabled; `gen_theme.py --contrast` + CTest `design_contrast`; placeholder เปลี่ยนจาก muted (3.1:1) เป็น graphite (6.1:1)

### [5] ขนาดหน้าต่างและ UI scale (S · A) ✅
`EditorShell`: ครั้งแรกที่ยังไม่เคยเลือกขนาด UI เลือก scale ที่ใหญ่ที่สุดที่หน้าต่างค่าเริ่มไม่เกิน 85 % ของจอ (`screenFit`); จำขนาดหน้าต่างต่อปลั๊กอินใน Settings (`window.hub/track/app`, debounce 400 ms กัน host ที่ resize ถี่); เปิดใหม่/เปลี่ยน scale แล้วหน้าต่างไม่เกินจอเสมอ; แผงย่อยทุกแผงแสดงตาม UI scale และไม่สูงกว่าจอ (เลื่อนได้) ผ่าน `PanelFrame`
ความเสี่ยง: Settings เดิมของผู้ใช้ที่มี `uiScale` ถือว่า "เลือกแล้ว" จึงไม่ auto-fit (แต่หน้าต่างยังถูกจำกัดไม่ให้เกินจอ) · ยังไม่ทดสอบสลับจอ DPI ต่างกันกับ DAW จริง

### [6] OBS Source ปิด Monitoring เอง (S · B) ✅
Source ใหม่ตั้ง `OBS_MONITORING_TYPE_NONE` ครั้งเดียว (`monitoring_checked` ใน settings ของ source จึงไม่ทับค่าที่ผู้ใช้เลือกภายหลัง); properties เตือนเมื่อ Monitoring เปิดอยู่พร้อมปุ่ม "ปิด Audio Monitoring ให้" · ทดสอบมือ: เพิ่ม Source ใหม่ใน OBS 32 แล้วดู Advanced Audio Properties (ยังไม่ได้ทดสอบกับ OBS จริงในรอบนี้)

### [7] Hub Compact / Regular / Wide (M) ✅
- Compact (< 760): คอลัมน์เดียว header (ปุ่มตัดเสียงคนดูอยู่เสมอ ที่เหลือซ่อนตามความสำคัญ, สถานะ OBS ย้ายไปใต้หัวการ์ด) → banner ตัดเสียง/ฟังแบบคนดู/คนดูไม่ได้ยิน → การ์ดคนดู (LUFS, meter, ใครได้ยินอะไร 2–3 บรรทัด, ฟังแบบคนดู, ปุ่มระดับเสียงรวม) → รายการแทร็ก
- Regular (760–1180) / Wide (> 1180): 2 คอลัมน์; หน้าต่างเตี้ยกว่าที่การ์ดเต็มต้องการ → คอลัมน์ขวาแบบย่อ (แถบคนดู + สรุปแบบป้าย/ค่า)
- hysteresis 24 px ทั้งความกว้างและความสูง; ค่าทั้งหมดใน `tokens.json › layout`
- แถวแทร็ก 3 ระดับ: Full (pill 118 + สไลเดอร์ 190), Mid (104 + 126), Narrow (2 บรรทัด, pill เต็มแถวเขียน "คุณได้ยิน"/"คนดูไม่ได้ยิน" ครบ, ระดับเสียงย้ายไปแผง ⋯) ชื่อมีที่อย่างน้อย 110–150 px, badge ที่ไม่มีที่จะหลบให้ชื่อ
- การ์ดสรุปเพิ่ม "คนดูได้ยิน แต่คุณไม่ได้ยิน" (หัวข้อ 6.5)
- min size 880×750 → 420×460
ทดสอบ: `ui-snapshot --audit` (ตัวตรวจ layout, exit 1 ถ้ามีปัญหา) · ความเสี่ยง: host ที่ส่ง resize ซ้ำ ๆ (Studio One/FL) ยังไม่ได้ลองจริง

### [8] Track / App Audio resize + แผงย่อย (M) ✅
Track: กว้าง 380–640, สูง 460–1400 ส่วนบน (สวิตช์ + ประโยคสรุป) อยู่กับที่ สไลเดอร์/ดีเลย์/footer เลื่อนใน Viewport · App Audio: 380–800 × 400–1400 แหล่งเสียง/สถานะอยู่บน ระดับ/ดีเลย์/อัด/ขั้นตอนเลื่อนได้ · แผงย่อย: `PanelFrame` (ข้อ 5), แผงซิงค์สูงตามขั้นตอน

### [9] ตรวจระบบ (M) ◐
ปุ่ม ✓ ที่ header (และ banner "คนดูไม่ได้ยินอะไรเลย" กดแล้วเปิด) 9 บรรทัดคงที่: Hub ตัวเดียว · มีแทร็ก · OBS เชื่อม · เสียงถึงคนดู · sample rate · bypass · ประมวลผลล่วงหน้า · App Audio capture · แชร์/ลิงก์ถาวร; ทุกบรรทัดที่ไม่ผ่านบอก ผลกับคุณ / ผลกับคนดู / → วิธีแก้; ปุ่ม "คัดลอกรายงานปัญหา"
ยังไม่ทำ: Hub อยู่บน Master หรือไม่ (ไม่มีข้อมูลจาก host), Track อยู่ท้าย chain, OBS Monitoring/Desktop Audio ซ้ำ (ต้องเพิ่ม field ใน bus protocol ให้ OBS รายงานกลับ), เสียงใน Master ที่ไม่มี Track

### [10] คนดูไม่ได้ยินอะไร (M · B แจ้งเตือนเท่านั้น) ◐
`HubProcessor::serviceSilence()` (message thread, อ่าน meter ที่มีอยู่): Stream Mix < −60 dBFS นาน 5 วินาที ขณะที่แทร็กที่เปิด "คนดูได้ยิน" มีเสียง > −50 dBFS และไม่ได้ตั้งใจตัดเสียง → banner + ตรวจระบบ + `viewers_silent` ใน REST API · ไม่เปลี่ยนเสียงใด ๆ · ยังไม่ทำ: test สัญญาณสังเคราะห์สำหรับ detector, ตรวจพีค/limiter, latency เปลี่ยน

### [11] รายงานปัญหา (M) ◐
`HubProcessor::diagnostics()`: DAW + path, OS, sample rate, buffer, บทบาท Hub, OBS + latency แยกส่วน, ทุกแทร็ก (เลข slot ไม่มีชื่อ), App Audio, ซิงค์, แชร์, REST API — ไม่มีชื่อแทร็ก ลิงก์ token หรือ key · ยังไม่ทำ: log เหตุการณ์ 50 รายการ

### [13] ลิงก์แชร์ถาวรผ่าน Vercel (M · A) ✅
- `web/share-vercel/`: `api/register.js`, `api/unregister.js`, `api/resolve/[token].js` (Web handler `export default { fetch }`), `lib/directory.js` (logic + memory store), `lib/redis.js` (Upstash REST ไม่มี dependency), `vercel.json` (rewrites `/l/:token` `/s/:token`, CSP ให้ต่อ WebSocket ได้เฉพาะ `wss://*.trycloudflare.com`), `scripts/sync-web.mjs` (ต้นฉบับเดียวจาก `plugins/hub/web`), `scripts/dev-server.mjs` (Vercel จำลองในเครื่อง), README วิธี deploy — ใช้ `.js` แทน `.ts` เพื่อให้ `node --test` และ Vercel รันไฟล์เดียวกันได้โดยไม่ต้อง build
- Hub: `ShareDirectory` (thread ของตัวเอง, register ทันทีที่ tunnel พร้อม, heartbeat 30 วิ, backoff 2→60 วิ, unregister เมื่อปิดแชร์, ปิด DAW ไม่รอเน็ต — ยกเลิก request ที่ค้างได้ใน 2 ms), token 26 ตัว (130 บิต, `BCryptGenRandom`/`arc4random_buf`/`getrandom`), secret 256 บิตเก็บใน state ของโปรเจกต์, ตรวจ `Origin` ของ WebSocket (403), ที่อยู่เว็บแชร์ + สวิตช์ใน Settings (ค่าเริ่มจาก CMake `HEARASIDE_SHARE_BASE` ว่างไว้: ไม่ส่งอะไรไปเว็บที่ผู้ใช้ไม่ได้เลือก)
- หน้าเว็บ: resolve ก่อนต่อทุกครั้ง, หน้า "รอสัญญาณ", หลุดแล้วต่อเองภายใน 5 วิ; App Audio รับลิงก์ถาวรของคนอื่นได้ (resolve ผ่าน WinHTTP)
- แผงแชร์: ลิงก์ถาวร (คัดลอกได้ก่อนเปิดแชร์) + สถานะ 3 ส่วน + ลิงก์สำรองของวันนี้
ทดสอบ: `share_api` (node:test 10 ข้อ), `share_web_in_sync`, `share_directory` (mock server: register/heartbeat/retry/unregister/ปิดเร็ว) และ E2E กับ cloudflared จริง (ดู `web/share-vercel/README.md`): ลงทะเบียนใน 4.6 วิ, ปิดเปิด Hub แล้วผู้ฟังในเบราว์เซอร์กลับมาเองใน ~7 วิด้วยลิงก์เดิม, Origin แปลกปลอม 403
ยังไม่ทำ: deploy จริงบน Vercel (ต้องใช้บัญชีของผู้ใช้), ลองไมค์จากมือถือผ่านลิงก์ส่งเสียงถาวร, QR code

### [+] REST API ควบคุม Hub ✅
`docs/rest-api.md` · `ControlServer` (127.0.0.1 เท่านั้น, Bearer key, ตรวจ Host กัน DNS rebinding, ปฏิเสธคำขอจากเว็บเพจที่มี Origin/CORS) · `HubRestApi.cpp` (state, mute, preview, limiter, ระดับรวม, หูฟังรวม, แทร็กตาม slot/ชื่อ, App Audio, ซิงค์, แชร์) ทุกคำสั่งผ่านพารามิเตอร์ของ host / mailbox ของ Track → undo ได้ · ปิดไว้เป็นค่าเริ่ม เปิดใน Settings · test `rest_api` 12 ข้อ

### ⏳ ที่เหลือ (ลำดับแนะนำ)
1. [10] ตรวจพีค (นับ gain reduction ของ limiter) + latency เปลี่ยน → banner "ปรับซิงค์ให้แล้ว · ย้อนกลับ" (ต้องเพิ่มค่า GR เป็น atomic ใน HubEngine)
2. [9] ให้ OBS Source เขียน monitoring type / Desktop Audio active / underrun ลง bus (protocol v3 + migration) แล้วเพิ่มในตรวจระบบ
3. [12] ช่องค้นหา/กรองแทร็ก (ทั้งหมด · คุณได้ยิน · คนดูได้ยิน · มีคำเตือน) + header คอลัมน์ติดบน
4. [4.5] shortcut ในหน้าต่างปลั๊กอิน (M, P, ↑/↓, H/V) — REST API + hotkey OBS ใช้แทนได้แล้วส่วนหนึ่ง
5. [14–18] Large: ทบทวนเหตุผลที่ถอด Scene ก่อน (ไม่มีบันทึกใน git log — ถูกถอดใน working tree ที่ยังไม่ commit)
