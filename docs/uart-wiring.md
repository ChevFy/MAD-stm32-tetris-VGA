# การต่อ UART (บอร์ด Logic อีกตัว ↔ บอร์ด Hub/VGA)

บอร์ด VGA นี้ = **Hub**. มันรับ packet จากบอร์ด **Logic** (STM32 อีกตัวที่คุมปุ่ม
+ ลอจิกเกม) ผ่าน UART แล้ววาดลงจอ VGA. สเปกโปรโตคอลเต็ม ๆ อยู่ที่
`../mad-python-uart-simulation/docs/protocol.md` (repo Python ข้าง ๆ) — ฝั่งนี้
พอร์ตมาเป็น C แล้วใน `Core/Src/proto.c`.

## ตั้งค่า UART

| ค่า | ค่าที่ใช้ |
|-----|----------|
| Baud | **921600** |
| Frame | 8N1 (8 data, no parity, 1 stop) |
| Flow control | ไม่มี |
| Logic level | 3.3V TTL (ห้ามต่อ RS-232 ±12V ตรง ๆ) |

## ขาเริ่มต้น (ที่เฟิร์มแวร์ตั้งไว้) — USART6

ลิงก์จริงใช้ **USART6 บน PG14/PG9 (AF8)** — เป็นพินที่ออก morpho header
เสียบสายจัมเปอร์ได้ ไม่ใช่ขา USB. ตั้งค่าใน `Core/Src/main.c` → `USART6_Init()`.

| Hub (บอร์ดนี้) | ทิศ | Logic board |
|----------------|-----|-------------|
| **PG14** = USART6_TX | →→→ | **PG9** = USART6_RX |
| **PG9** = USART6_RX | ←←← | **PG14** = USART6_TX |
| **GND** | ─── | **GND** (ต้องต่อร่วมเสมอ) |

> ต่อแบบ **ไขว้**: TX ข้างหนึ่ง เข้า RX อีกข้าง. GND ร่วมบังคับ.
> หาพินที่ silkscreen เขียน `PG9` / `PG14` บน morpho header (CN11/CN12).

```
   Hub (VGA)                      Logic (STM32)
 PG14 TX ─────────────────────► PG9  RX
 PG9  RX ◄───────────────────── PG14 TX
 GND  ───────────────────────── GND
```

ทั้งสองบอร์ดต้องตั้ง USART6 baud = **921600** ให้ตรงกัน. ฝั่ง Hub ใช้ **PCLK2**
(`HAL_RCC_GetPCLK2Freq()`) เพราะ USART6 อยู่บน APB2. บน Hub เช็กแล้วว่า LTDC ใช้
GPIOG แค่ **PG13** → PG9/PG14 ว่าง ไม่ชนจอ.

## ทำไมไม่ใช้ PD8/PD9 (USART3)

PD8/PD9 บน Nucleo-144 ต่อเข้า **ST-LINK USB Virtual COM Port** ผ่าน solder bridge
อยู่แล้ว — ต่อสายนอกตรง ๆ จะชนกับ VCP. จึงย้ายลิงก์มาที่ USART6/PG14/PG9.

> อยากกลับไปใช้ PD8/PD9 เพื่อ **เทสต์ส่ง packet จาก PC ผ่าน USB VCP** (ไม่ต้องมี
> บอร์ด Logic): สลับ `USART6_Init()` กลับเป็น USART3 (PD8/PD9 AF7, PCLK1) แล้ว
> rename ISR + register กลับเป็น `USART3`. git log มีเวอร์ชัน USART3 เดิมให้อ้างอิง.

## ฝั่งบอร์ด Logic ต้องทำอะไร

ส่ง/รับตามโปรโตคอล (มีโค้ด C อ้างอิงพร้อมใน
`../mad-python-uart-simulation/docs/protocol.md`):

1. ลอบบี้: กดปุ่ม → ส่ง `JOIN` ทุก 0.5 วิ จนได้ `START`.
2. ได้ `START` (มี mode, seed) → เริ่มเกม, ส่ง `STATE` รัว ๆ.
3. ชิ้นล้นจอ → ส่ง `GAMEOVER` ครั้งเดียว → รอ `RESULT`.
4. CRC = CRC-8/SMBUS over `TYPE+LEN+PAYLOAD`. เช็ก `crc8("123456789")==0xF4`.

## เช็กว่ารับได้จริง

- กดปุ่ม **B1 (PC13)** บน Hub → เปิด debug panel.
- ดูค่า `STATE: n/S`, `JOIN: n/S` ขึ้น = รับ packet ได้.
- `CRC_ERR` ต้อง **0**. ถ้าขึ้นเรื่อย ๆ → baud ไม่ตรง หรือ GND ไม่ร่วม.
- ถ้าไม่ต่อบอร์ด Logic เลย → Hub ค้างหน้าลอบบี้รอ `JOIN` (ไม่มี demo แล้ว).
