# ภาพรวมระบบ Tetris Cabinet — อะไรต่อกับอะไร

ระบบมี **4 ส่วน** ต่อกันเป็นลูกโซ่ RFID → ESP32 → STM32 Logic → STM32 VGA Hub →
จอ, โดย ESP32 คุยกับ backend ผ่าน MQTT เพื่อจัดการเงิน/คะแนน.

```
 [บัตร RFID]
     │ แตะ
     ▼
 [MFRC522]── SPI ──[ESP32-WROOM-32]── WiFi/MQTT(TLS) ──[Backend Go]──[Postgres]
                        │   ▲                              (EMQX Cloud broker)
            USART2 115200│   │ USART2
          (pay/LB/newrec)│   │ (score)
                        ▼   │
                 [STM32 Logic]  ── ปุ่ม 6 ตัว
                        │   ▲
            USART6 921600│   │ USART6
             (JOIN/STATE │   │ (START/RESULT/ATTACK_TX)
              /CREDIT…)  ▼   │
                 [STM32 VGA Hub] ── LTDC ──► จอ VGA (640x480)
```

| ส่วน | บอร์ด | หน้าที่ | repo |
|------|-------|--------|------|
| Backend | - (server) | MQTT + Postgres: role/เงิน/คะแนน/leaderboard | `mad-tetris-backend` |
| Pay gateway | ESP32-WROOM-32 | อ่าน RFID, คุย MQTT, ส่งผล UART | `mad-stm32-tetris-logic/arduino/` |
| Game logic | STM32F767 Nucleo-144 | เอนจินเกม + ปุ่ม + gate จ่ายเงิน | `mad-stm32-tetris-logic` |
| Display (Hub) | STM32F767 Nucleo-144 | วาดจอ VGA จาก packet | `mad-stm32-tetris-VGA` |

---

## 1. RFID MFRC522 → ESP32 (SPI, 3.3V)

| MFRC522 | ESP32 |
|---------|-------|
| SDA/SS | GPIO5 |
| SCK | GPIO18 |
| MOSI | GPIO23 |
| MISO | GPIO19 |
| RST | GPIO27 |
| 3.3V | 3V3 |
| GND | GND |

> MFRC522 = **3.3V เท่านั้น** ห้าม 5V.

---

## 2. ESP32 ↔ STM32 Logic (USART2, 115200 8N1, สองทาง)

| ทิศ | จาก | → | ถึง | ใช้ส่งอะไร |
|-----|-----|---|-----|-----------|
| ESP32 → Logic | **GPIO17** (TX2) | → | **PD6** (USART2_RX) | `PAY OK/DENY/NOCARD`, `LB`, `NEWREC` |
| Logic → ESP32 | **PD5** (USART2_TX) | → | **GPIO16** (RX2) | `SCORE` (ตอนจบเกม) |
| GND | GND | ─ | GND | ต้องร่วม |

**ข้อความ (ASCII, จบด้วย `\n`):**

ESP32 → Logic:
- `PAY OK <balance> <spent> <name>` — จ่ายผ่าน (name ว่าง = guest)
- `PAY DENY <balance>` — เงินไม่พอ
- `PAY NOCARD` — บัตรไม่อยู่ใน DB
- `LB <rank> <count> <points> <name>` — leaderboard ทีละแถว
- `NEWREC <points>` — ทำลายสถิติ (member)

Logic → ESP32:
- `SCORE <points>` — คะแนนสุดท้ายตอนเกมจบ (ESP32 เอาไป save ถ้าเป็น member)

---

## 3. STM32 Logic ↔ STM32 VGA Hub (USART6, 921600 8N1, สองทาง-ไขว้)

| Logic | ทิศ | VGA Hub |
|-------|-----|---------|
| **PG14** (USART6_TX) | → | **PG9** (USART6_RX) |
| **PG9** (USART6_RX) | ← | **PG14** (USART6_TX) |
| GND | ─ | GND (ต้องร่วม) |

พินอยู่บน morpho header (CN11/CN12). เลี่ยง PD8/PD9 เพราะชน ST-LINK USB VCP —
ดู [`uart-wiring.md`](uart-wiring.md).

**Packet (binary):** เฟรม `0xAA | TYPE | LEN | PAYLOAD | CRC8` (CRC-8/SMBUS).
รายละเอียดการไหลภายในเฟิร์มแวร์ดู [`uart-firmware-integration.md`](uart-firmware-integration.md).

| TYPE | ค่า | ทิศ | ความหมาย |
|------|-----|-----|---------|
| JOIN | 0x01 | Logic→Hub | เข้าลอบบี้ (+ชื่อ member) |
| STATE | 0x02 | Logic→Hub | บอร์ด 10x20 + score/next/hold/… |
| ATTACK | 0x03 | Logic→Hub | ส่ง garbage |
| GAMEOVER | 0x04 | Logic→Hub | เกมจบ |
| SKIP_WAIT | 0x05 | Logic→Hub | ข้ามรอคู่ (เล่นเดี่ยว) |
| CREDIT | 0x06 | Logic→Hub | สถานะจ่ายเงิน: status+balance+spent |
| LEADERBOARD | 0x07 | Logic→Hub | 1 แถว: rank+count+points+name |
| NEWRECORD | 0x08 | Logic→Hub | member ทำลายสถิติ: points |
| START | 0x10 | Hub→Logic | เริ่มเกม (mode+seed) |
| ATTACK_TX | 0x11 | Hub→Logic | ส่ง garbage ไปเป้าหมาย |
| RESULT | 0x12 | Hub→Logic | ผลแพ้/ชนะ |

---

## 4. ESP32 ↔ Backend (WiFi → MQTT TLS)

- Broker: **EMQX Cloud**, TLS **8883**, user/pass (ตั้งใน `.ino` + backend `.env`)
- ขอ: publish `<topic>`; ตอบ: backend publish `<topic>/response`

| Topic | payload | ตอบ |
|-------|---------|-----|
| `/check_role` | `{card_id}` | `{role:"guest"/"member"}` หรือ `{error}` |
| `/retieve_member` | `{card_id}` | `{username, balance, points}` (member) |
| `/pay` | `{card_id, amount}` | OK หรือ `{error:"...insufficient"}` |
| `/retieve_leaderboard` | `{}` | array `[{username, points}, …]` top 10 |
| `/add_score` | `{card_id, points}` | member + ทุบสถิติ → `{updated:true, best}` |
| `/add_money` | `{card_id, amount}` | เติมเงิน |
| `/create_guest` `/create_member` `/remove_card` | `{card_id,…}` | จัดการบัตร |

**กฎสำคัญ:** `/pay` หักเงินได้ทั้ง guest+member (ถ้าเงินพอ). `/add_score` =
**members เท่านั้น** และ save **เฉพาะเมื่อคะแนนใหม่ > สถิติเดิม** (high-score).

---

## 5. ปุ่ม + ไฟ บนบอร์ด

### STM32 Logic — ปุ่ม (ต่อปุ่มระหว่างพิน ↔ GND, active-low pullup)

| ปุ่ม | Player 1 | Player 2 |
|------|----------|----------|
| LEFT | PF13 | PE10 |
| RIGHT | PE9 | PE12 |
| ROTATE | PE11 | PE13 |
| HOLD | PF14 | PE14 |
| SOFT_DROP | PF12 | PE15 |

- **เริ่มเกม = แตะบัตร** ไม่ใช้ปุ่ม START. แตะ 1 ใบ → รอ 10 วิ; แตะใบ 2 ใน 10 วิ → **2 คน**
- **LD1 (PB0, เขียว)** = ติดเมื่อรับ pay line จาก ESP32 (เช็ก USART2 RX)

### STM32 VGA Hub

- **PB7 (LD2 น้ำเงิน)** = ติดเมื่อรับ byte จาก Logic (ตัวเช็ก USART6 RX)
- **B1 (PC13)** = toggle debug panel
- LTDC → จอ VGA (ขา RGB/HSYNC/VSYNC/CLK) ดู [`vga-wiring.md`](vga-wiring.md)

---

## 6. ลำดับการเล่น (end-to-end)

```
1. แตะบัตร
   ESP32 → /check_role → role
     guest  → /pay
     member → /retieve_member (ชื่อ+เงิน) → /pay
   backend หักเงิน
     เงินพอ  → ESP32 "PAY OK <bal> <spent> <name>" → Logic (credit+1)
     ไม่พอ   → "PAY DENY"  → Hub overlay แดง "NOT ENOUGH CREDIT"
     ไม่มีบัตร→ "PAY NOCARD"→ Hub overlay แดง "UNKNOWN CARD"
   ESP32 ดึง /retieve_leaderboard → "LB …" → Hub (cache)

2. จ่ายผ่าน → Hub โชว์ BALANCE/PAID (4 วิ) + "READY TO PLAY / PRESS START"
   member → ชื่อขึ้นในลอบบี้

3. กด START → JOIN → ลอบบี้ (รอคู่ 10 วิ)
   - P2 แตะบัตร (คนละใบ) ช่วงนี้ = เล่น 2 คน
   - กด HOLD / หมด 10 วิ → เริ่มเกม
   * แตะบัตรใบเดิมซ้ำ = ไม่คิดเงิน (กันหักซ้ำ)

4. เล่น Tetris → ส่ง STATE 10 เฟรม/วิ → Hub วาด

5. ตันจอ (game over)
   Logic → GAMEOVER → Hub เข้าจอ RESULT
   Logic → "SCORE n" → ESP32
     member → /add_score → backend (save ถ้าทุบสถิติ)
       updated → ESP32 "NEWREC n" → Hub โชว์ "NEW HIGH SCORE!"
     ดึง /retieve_leaderboard ใหม่ → Hub อัปเดตตาราง
   จอ RESULT 5 วิ: WIN/LOSE + SCORE + (NEW HIGH SCORE) + LEADERBOARD
   → กลับหน้า "TAP YOUR CARD"
```

---

## 7. GND ร่วม (บังคับ)

ต่อ GND ให้ถึงกันทั้งหมด:
```
MFRC522 GND ── ESP32 GND ── Logic GND ── Hub GND
```
แต่ละบอร์ดจ่ายไฟ USB แยกได้ แต่ **GND ต้องร่วม** ไม่งั้น UART/SPI อ่านมั่ว.

---

## 8. Build / Flash

| ส่วน | build | flash |
|------|-------|-------|
| Backend | `go build ./...` | `go run` / docker-compose (ต้องตั้ง `.env`) |
| VGA Hub | `make -C Debug all` (CLI) หรือ CubeIDE | ST-LINK |
| Logic | STM32CubeIDE | ST-LINK |
| ESP32 | Arduino IDE (ESP32 core) | USB; lib: PubSubClient, MFRC522, ArduinoJson v6 |

**สำคัญ:** proto packet เปลี่ยน length บ่อย (CREDIT = 9 ไบต์ล่าสุด) → flash
**VGA + Logic พร้อมกัน** เสมอ ไม่งั้น length mismatch → Hub ทิ้ง packet.

config ที่ต้องตั้งก่อน flash ESP32 (`tetris_pay.ino` หัวไฟล์): `WIFI_SSID/PASS`,
`MQTT_HOST/USER/PASS` (ให้ตรงกับ backend `.env`), `GAME_PRICE`.
