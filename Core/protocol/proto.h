/**
 * @file    proto.h
 * @brief   UART packet protocol (STM32 Logic <-> Hub).
 *
 * C port of tools/protocol.py from mad-python-uart-simulation. This is the Hub
 * (VGA board) side: it receives JOIN/STATE/ATTACK/GAMEOVER/SKIP_WAIT from the
 * Logic board and sends START/ATTACK/RESULT back.
 *
 * Wire format of one packet:
 *     [0xAA] [TYPE] [LEN] [PAYLOAD ... LEN bytes] [CRC8]
 * CRC-8/SMBUS (poly 0x07, init 0x00, no reflection) over TYPE+LEN+PAYLOAD.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

/* --- Framing ------------------------------------------------------------- */
#define PROTO_SYNC         0xAA
#define PROTO_MAX_PAYLOAD  112   /* largest payload we expect (STATE) */
#define PROTO_MAX_PACKET   (3 + PROTO_MAX_PAYLOAD + 1)

/* --- Packet types -------------------------------------------------------- */
/* Received from the Logic board. */
#define PROTO_TYPE_JOIN       0x01
#define PROTO_TYPE_STATE      0x02
#define PROTO_TYPE_ATTACK     0x03
#define PROTO_TYPE_GAMEOVER   0x04
#define PROTO_TYPE_SKIP_WAIT  0x05
#define PROTO_TYPE_CREDIT     0x06   /* RFID/pay status relayed from ESP32 */
#define PROTO_TYPE_LEADERBOARD 0x07  /* one leaderboard row relayed from ESP32 */
#define PROTO_TYPE_NEWRECORD  0x08   /* member beat their high score */
/* Sent to the Logic board. */
#define PROTO_TYPE_START      0x10
#define PROTO_TYPE_ATTACK_TX  0x11
#define PROTO_TYPE_RESULT     0x12

/* --- Board ---------------------------------------------------------------- */
#define PROTO_BOARD_W  10
#define PROTO_BOARD_H  20

/* --- Payload sizes ------------------------------------------------------- */
#define PROTO_LEN_JOIN    22
#define PROTO_LEN_STATE   112
#define PROTO_LEN_ATTACK  1
#define PROTO_LEN_START   5
#define PROTO_LEN_RESULT  1
#define PROTO_LEN_CREDIT  9   /* status(1) + balance int32 LE(4) + spent int32 LE(4) */
#define PROTO_LEN_LEADERBOARD 22   /* rank(1)+count(1)+points u32(4)+name[16] */
#define PROTO_LEN_NEWRECORD 4      /* points u32 LE */

/* CREDIT status codes (payload byte 0). */
#define PROTO_CREDIT_DENIED  0   /* tap rejected: not enough balance */
#define PROTO_CREDIT_OK      1   /* paid: balance was sufficient, game unlocked */
#define PROTO_CREDIT_NO_CARD 2   /* card not found in the database (/check_role) */

/* --- Parsed payload containers ------------------------------------------- */
typedef struct {
  uint8_t  player;
  uint8_t  card_type;
  char     name[17];   /* NUL-terminated copy of name[16] */
  uint32_t points;
} proto_join_t;

typedef struct {
  uint8_t  status;    /* PROTO_CREDIT_OK / PROTO_CREDIT_DENIED */
  int32_t  balance;   /* account balance after the tap */
  int32_t  spent;     /* amount deducted for this game */
} proto_credit_t;

typedef struct {
  uint8_t  rank;      /* 0-based row position */
  uint8_t  count;     /* total rows in this leaderboard */
  uint32_t points;
  char     name[17];  /* NUL-terminated copy of name[16] */
} proto_lb_t;

typedef struct {
  uint8_t  player;
  uint8_t  seq;
  uint8_t  board[100];   /* 10x20 cells, 4 bits each */
  uint8_t  next;
  uint8_t  hold;
  uint32_t score;
  uint16_t lines;
  uint8_t  level;
  uint8_t  garbage_wait;
} proto_state_t;

/* --- CRC8 ---------------------------------------------------------------- */
uint8_t proto_crc8(const uint8_t *data, uint32_t len);

/* --- Building packets (write to caller buffer, return total length) ------ */
int proto_build_packet(uint8_t *out, uint8_t type,
                       const uint8_t *payload, uint8_t len);
int proto_build_start(uint8_t *out, uint8_t mode, uint32_t seed);
int proto_build_attack(uint8_t *out, uint8_t rows);
int proto_build_result(uint8_t *out, uint8_t result);
int proto_make_join_payload(uint8_t *out, uint8_t player, uint8_t card_type,
                            const char *name, uint32_t points);

/* --- Parsing payloads ---------------------------------------------------- */
void    proto_parse_join(const uint8_t *payload, proto_join_t *out);
void    proto_parse_state(const uint8_t *payload, proto_state_t *out);
void    proto_parse_credit(const uint8_t *payload, proto_credit_t *out);
void    proto_parse_leaderboard(const uint8_t *payload, proto_lb_t *out);
uint32_t proto_parse_newrecord(const uint8_t *payload);
uint8_t proto_parse_attack(const uint8_t *payload);
uint8_t proto_cell_at(const uint8_t *board, int x, int y);

/* Expected payload length for a received type, or -1 if no check. */
int         proto_expected_len(uint8_t type);
const char *proto_type_name(uint8_t type);

/* --- Incoming parser (byte-level state machine) -------------------------- */
typedef enum {
  PROTO_WAIT_SYNC = 0,
  PROTO_GET_TYPE,
  PROTO_GET_LEN,
  PROTO_GET_PAYLOAD,
  PROTO_GET_CRC
} proto_rx_state_t;

typedef struct {
  proto_rx_state_t state;
  uint8_t  type;
  uint8_t  len;
  uint8_t  idx;
  uint8_t  payload[PROTO_MAX_PAYLOAD];
  uint32_t crc_errors;
  uint32_t length_errors;
} proto_parser_t;

/* Called for each complete, CRC-valid packet. */
typedef void (*proto_packet_cb)(uint8_t type, const uint8_t *payload,
                                uint8_t len, void *ctx);
/* Called for each byte seen while hunting for SYNC (board printf noise). */
typedef void (*proto_text_cb)(uint8_t byte, void *ctx);

void proto_parser_init(proto_parser_t *p);
void proto_parser_feed_byte(proto_parser_t *p, uint8_t b,
                            proto_packet_cb on_packet,
                            proto_text_cb on_text, void *ctx);

#endif /* PROTO_H */
