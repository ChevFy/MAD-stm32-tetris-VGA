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

## ขาเริ่มต้น (ที่เฟิร์มแวร์ตั้งไว้) — USART3

| Hub (บอร์ดนี้) | ทิศ | Logic board |
|----------------|-----|-------------|
| **PD8** = USART3_TX | →→→ | RX ของ Logic |
| **PD9** = USART3_RX | ←←← | TX ของ Logic |
| **GND** | ─── | **GND** (ต้องต่อร่วมเสมอ) |

> ต่อแบบ **ไขว้**: TX ข้างหนึ่ง เข้า RX อีกข้าง. GND ร่วมบังคับ.

```
   Hub (VGA)                 Logic (STM32)
 PD8 TX ─────────────────────► RX
 PD9 RX ◄───────────────────── TX
 GND ───────────────────────── GND
```

## ⚠️ ข้อควรระวัง: PD8/PD9 ชนกับ ST-LINK VCP

บน Nucleo-144 ขา **PD8/PD9 ต่อเข้า ST-LINK (USB Virtual COM Port) อยู่แล้ว**
ผ่าน solder bridge. ผลคือ:

- ถ้าจะต่อบอร์ด Logic ภายนอกที่ PD8/PD9 → อาจชนกับ ST-LINK. ทางแก้:
  ถอด solder bridge ของ VCP (ดูคู่มือ Nucleo UM1974 — SB ของ VCP บน ST-LINK)
  **หรือ** ย้าย UART ไป USART อื่น (ดูข้างล่าง).
- ข้อดี: ถ้า **ไม่** ต่อบอร์ด Logic คุณเสียบ USB แล้วคุยกับบอร์ดนี้จาก PC ได้เลย
  ผ่าน VCP (เทสต์ส่ง packet จาก PC ได้ทันที baud 921600).

## ทางเลือก: ย้ายไป USART6 (เลี่ยงชน VCP, เลี่ยงขา LTDC)

ขาที่ว่าง + ไม่ชน LTDC: **PG14 (TX), PG9 (RX), AF8**.

แก้ใน `Core/Src/main.c` ฟังก์ชัน `USART3_Init()` — เปลี่ยน instance + ขา + AF:

```c
/* USART6 แทน USART3: PG14=TX, PG9=RX, AF8 */
RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN;
RCC->APB2ENR |= RCC_APB2ENR_USART6EN;           /* USART6 อยู่ APB2 */
(void)RCC->APB2ENR;

GPIOG->MODER   &= ~((3u<<(14*2)) | (3u<<(9*2)));
GPIOG->MODER   |=  ((2u<<(14*2)) | (2u<<(9*2)));  /* AF */
GPIOG->OSPEEDR |=  ((3u<<(14*2)) | (3u<<(9*2)));
GPIOG->AFR[1]  &= ~((0xFu<<((14-8)*4)) | (0xFu<<((9-8)*4)));
GPIOG->AFR[1]  |=  ((8u  <<((14-8)*4)) | (8u  <<((9-8)*4)));  /* AF8 */

USART6->CR1 = 0;
USART6->BRR = (HAL_RCC_GetPCLK2Freq() + baud/2) / baud;   /* APB2 clock! */
USART6->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
USART6->CR1 |= USART_CR1_UE;
HAL_NVIC_SetPriority(USART6_IRQn, 6, 0);
HAL_NVIC_EnableIRQ(USART6_IRQn);
```

แล้วเปลี่ยนชื่อ ISR `USART3_IRQHandler` → `USART6_IRQHandler` และ register
`USART3->` ทั้งหมดใน `uart_send()`/ISR ให้เป็น `USART6->`.
(หมายเหตุ: USART6 ใช้ **PCLK2** ไม่ใช่ PCLK1 — ใช้ `HAL_RCC_GetPCLK2Freq()`).

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
- ถ้าไม่ต่อบอร์ด Logic เลย → หลัง 2 วิ Hub รัน fake-board demo เอง (เกมหล่น
  ให้ดูบนจอ) พอมี packet จริงเข้ามา demo หยุดเอง.
