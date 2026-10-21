# UART → ลอจิกเกม (การต่อภายในเฟิร์มแวร์)

เอกสารนี้ว่าด้วย **ฝั่งซอฟต์แวร์**: ไบต์ที่เข้ามาทาง UART ไหลเข้าสู่ Hub state
machine และขึ้นจอ VGA ได้อย่างไร. ส่วนการต่อสาย/ขา/baud อยู่ที่
[`uart-wiring.md`](uart-wiring.md).

โค้ดเกี่ยวข้อง:

| ชั้น | ไฟล์ | หน้าที่ |
|------|------|--------|
| ไดรเวอร์ UART (register-level) | `Core/Src/main.c` | `USART6_Init`, ISR, `uart_send` |
| ตัวแยกโปรโตคอล | `Core/protocol/proto.c` / `.h` | state machine + CRC + parse payload |
| ลอจิกเกม/ลอบบี้ | `Core/hub/hub.c` / `.h` | `hub_handle`, `hub_update` |
| วาดจอ | `Core/display/vga_draw.c` | `vga_render` |

## ภาพรวมเส้นทางข้อมูล (RX)

```
USART6 RXNE IRQ ──► rx_ring[] ──► main loop drain ──► proto_parser_feed_byte
                   (ring buffer)        │
                                        ▼
                            on_packet(type, payload, len)
                                        │
                                        ▼
                            hub_handle(&g_hub, ...) ──► เปลี่ยน state ของ Hub
                                        │                (อาจ hub.send ตอบกลับ)
                                        ▼
                            vga_render(&g_hub, ...) ──► framebuffer ──► จอ VGA
```

### 1) ISR เก็บไบต์ลง ring buffer

`USART6_IRQHandler` (`main.c`) ทำงานทุกไบต์ที่เข้ามา: เคลียร์ overrun (ORE) แล้ว
push ไบต์ลง `rx_ring[1024]` แบบ lock-free (head เขียนใน ISR, tail อ่านใน main
loop). ถ้า ring เต็มจะ **ทิ้งไบต์** (ไม่เขียนทับ) เพื่อไม่ให้ tail พัง.

> ISR สั้นมากโดยตั้งใจ — แค่ย้ายไบต์. งานแยก packet ไปทำใน main loop ไม่ใช่ใน
> interrupt context.

### 2) Main loop ดูด ring → ป้อน parser ทีละไบต์

ใน `while(1)` (ขั้นที่ 1 ของลูป):

```c
while (rx_tail != rx_head) {
  uint8_t b = rx_ring[rx_tail];
  rx_tail = (uint16_t)((rx_tail + 1) % RX_RING_SIZE);
  proto_parser_feed_byte(&g_parser, b, on_packet, on_text, &now);
}
```

`now` (`HAL_GetTick()`) ถูกส่งเป็น `ctx` ลงไปให้ callback ใช้เป็น timestamp.

### 3) Parser: state machine ระดับไบต์

`proto_parser_feed_byte` (`proto.c`) เดินสถานะ
`WAIT_SYNC → GET_TYPE → GET_LEN → GET_PAYLOAD → GET_CRC`:

- เฟรม: `[0xAA][TYPE][LEN][PAYLOAD..LEN][CRC8]`.
- CRC = **CRC-8/SMBUS** (poly `0x07`, init `0x00`) คำนวณบน `TYPE+LEN+PAYLOAD`.
- packet ครบ + CRC ถูก → เรียก `on_packet(type, payload, len, ctx)`.
- CRC ผิด → นับ `g_parser.crc_errors++`, LEN เกินช่วง → `length_errors++`.
- ไบต์ที่เจอตอนยังหา SYNC ไม่พบ (เช่น `printf` ขยะจากบอร์ด Logic) → ส่งเข้า
  `on_text` ซึ่งฝั่งนี้ **ทิ้งทั้งหมด**.

ตัวนับ error ถูกก๊อปเข้า `g_hub.stats` ก่อน render ทุกเฟรม เพื่อให้ debug panel
แสดง `CRC_ERR` ได้.

### 4) `on_packet` → ป้อน Hub

```c
static void on_packet(uint8_t type, const uint8_t *payload, uint8_t len, void *ctx)
{
  uint32_t now = *(uint32_t *)ctx;
  hub_handle(&g_hub, type, payload, len, now);
}
```

ตัว callback บาง ๆ: แค่ดึง timestamp จาก `ctx` แล้วส่ง packet เข้า Hub.

### 5) `hub_handle` — ลอจิกเกมตอบสนอง packet

`hub_handle` (`hub.c`) แยกตาม TYPE แล้วอัปเดต state machine ลอบบี้/เกม:

| TYPE (รับจาก Logic) | ค่า | ผลใน Hub |
|---------------------|-----|---------|
| `JOIN` | `0x01` | ลงทะเบียนผู้เล่นเข้าลอบบี้ (`proto_parse_join`) |
| `STATE` | `0x02` | เก็บบอร์ด 10×20 ล่าสุดไว้วาด (`proto_parse_state`) |
| `ATTACK` | `0x03` | ส่งต่อแถว garbage ไปอีกฝั่ง |
| `GAMEOVER` | `0x04` | ตัดสินแพ้/ชนะ → `RESULT_SHOW` |
| `SKIP_WAIT` | `0x05` | ข้ามหน้าจอรอ |

Hub ตอบกลับผ่าน callback `hub.send` (= `uart_send`) ที่ตั้งตอน
`hub_init(&g_hub, uart_send, NULL)`:

| TYPE (ส่งไป Logic) | ค่า | เมื่อ |
|--------------------|-----|------|
| `START` | `0x10` | ครบ 2 ฝั่ง/หมด countdown → แจ้ง mode + seed |
| `ATTACK_TX` | `0x11` | ส่งแถว garbage ไปเป้าหมาย |
| `RESULT` | `0x12` | จบเกม → แจ้งแพ้/ชนะ/จบเดี่ยว |

### 6) `hub_update` — เปลี่ยนสถานะตามเวลา

เรียกทุกลูป. จัดการ transition ที่ขึ้นกับเวลา ไม่ใช่ packet: หมด countdown,
timeout รอคู่, หมดเวลาโชว์ผล, ตัดผู้เล่นที่เงียบ (`T_ALIVE_TIMEOUT`). deadline
เก็บใน `g_hub.deadline` เทียบกับ `now`.

### 7) ไม่มีบอร์ดต่อ → ค้างหน้าลอบบี้

ไม่มี demo/fake board แล้ว. ถ้าไม่มีบอร์ด Logic ต่อ Hub จะ boot เข้าหน้าลอบบี้
รอ `JOIN` จากบอร์ดจริงเฉย ๆ (จอไม่ขยับจนกว่าจะมี packet เข้ามา).

> โค้ด fake board เดิม (`Core/tests/hub_test.c`) ถูกถอดออกจาก build แล้ว — ไฟล์
> ยังอยู่ในทรีไว้อ้างอิง แต่ไม่ถูก compile/link.

## เส้นทาง TX (Hub → Logic)

ทางเดียว: Hub สร้างเฟรมด้วย `proto_build_start/attack/result` แล้วเรียก
`hub.send` → `uart_send` ซึ่ง **blocking**: วน poll `USART_ISR_TXE` ต่อไบต์ แล้ว
รอ `USART_ISR_TC` ปิดท้าย. ที่ baud 921600 การส่งเฟรมสั้น ๆ ใช้เวลา < 100 µs จึง
ยอม block ได้โดยไม่กระทบ render 30 fps.

## จุดเชื่อม (ถ้าจะแก้/ขยาย)

- **เปลี่ยน UART peripheral/ขา** → แก้ `USART6_Init`, ISR, `uart_send` (ดูหัวข้อ
  USART6 ใน `uart-wiring.md`).
- **เพิ่ม packet type ใหม่** → เพิ่ม `PROTO_TYPE_*` + `proto_expected_len` ใน
  `proto.c`, แล้วจัดการ case ใหม่ใน `hub_handle`.
- **เปลี่ยนการวาด** → `vga_render` ใน `Core/display/vga_draw.c` อ่านจาก `g_hub`
  อย่างเดียว ไม่ยุ่งกับ UART.
- **ปรับขนาด ring / กัน overflow** → `RX_RING_SIZE` ใน `main.c` (ค่าปัจจุบัน
  1024). ที่ 921600 baud, STATE payload 112 ไบต์ต่อเฟรม — 1024 รองรับหลายเฟรม
  ซ้อนได้สบายถ้า main loop ดูดทัน.

## ดูว่าทำงานจริง

กด **B1 (PC13)** เปิด debug panel บน Hub:

- `STATE: n/S`, `JOIN: n/S` ขึ้น = parser + hub_handle รับ packet ได้.
- `CRC_ERR` ต้องเป็น **0**. ถ้าไต่ขึ้น → baud ไม่ตรง หรือ GND ไม่ร่วม.
- LED PB7 กะพริบตาม `LTDC->CPSR` = main loop ยังเดิน (liveness).
