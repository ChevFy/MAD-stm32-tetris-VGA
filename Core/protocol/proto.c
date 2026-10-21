/**
 * @file  proto.c
 * @brief UART packet protocol — C port of tools/protocol.py.
 */
#include "proto.h"
#include <string.h>

/* --- CRC8 (CRC-8/SMBUS) --------------------------------------------------- */
uint8_t proto_crc8(const uint8_t *data, uint32_t len)
{
  uint8_t crc = 0x00;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                         : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

/* --- little-endian helpers ------------------------------------------------ */
static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint16_t get_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- Building packets ----------------------------------------------------- */
int proto_build_packet(uint8_t *out, uint8_t type,
                       const uint8_t *payload, uint8_t len)
{
  out[0] = PROTO_SYNC;
  out[1] = type;
  out[2] = len;
  for (uint8_t i = 0; i < len; i++) out[3 + i] = payload[i];
  out[3 + len] = proto_crc8(&out[1], (uint32_t)len + 2); /* over TYPE+LEN+PAYLOAD */
  return 4 + len;
}

int proto_build_start(uint8_t *out, uint8_t mode, uint32_t seed)
{
  uint8_t pl[PROTO_LEN_START];
  pl[0] = mode;
  put_u32(&pl[1], seed);
  return proto_build_packet(out, PROTO_TYPE_START, pl, PROTO_LEN_START);
}

int proto_build_attack(uint8_t *out, uint8_t rows)
{
  return proto_build_packet(out, PROTO_TYPE_ATTACK_TX, &rows, 1);
}

int proto_build_result(uint8_t *out, uint8_t result)
{
  return proto_build_packet(out, PROTO_TYPE_RESULT, &result, 1);
}

int proto_make_join_payload(uint8_t *out, uint8_t player, uint8_t card_type,
                            const char *name, uint32_t points)
{
  memset(out, 0, PROTO_LEN_JOIN);
  out[0] = player;
  out[1] = card_type;
  if (name) {
    for (int i = 0; i < 16 && name[i]; i++) out[2 + i] = (uint8_t)name[i];
  }
  put_u32(&out[18], points);
  return PROTO_LEN_JOIN;
}

/* --- Parsing payloads ----------------------------------------------------- */
void proto_parse_join(const uint8_t *payload, proto_join_t *out)
{
  out->player    = payload[0];
  out->card_type = payload[1];
  memcpy(out->name, &payload[2], 16);
  out->name[16] = '\0';
  out->points = get_u32(&payload[18]);
}

void proto_parse_state(const uint8_t *payload, proto_state_t *out)
{
  out->player = payload[0];
  out->seq    = payload[1];
  memcpy(out->board, &payload[2], 100);
  out->next         = payload[102];
  out->hold         = payload[103];
  out->score        = get_u32(&payload[104]);
  out->lines        = get_u16(&payload[108]);
  out->level        = payload[110];
  out->garbage_wait = payload[111];
}

void proto_parse_credit(const uint8_t *payload, proto_credit_t *out)
{
  out->status  = payload[0];
  out->balance = (int32_t)get_u32(&payload[1]);
  out->spent   = (int32_t)get_u32(&payload[5]);
}

void proto_parse_leaderboard(const uint8_t *payload, proto_lb_t *out)
{
  out->rank   = payload[0];
  out->count  = payload[1];
  out->points = get_u32(&payload[2]);
  memcpy(out->name, &payload[6], 16);
  out->name[16] = '\0';
}

uint32_t proto_parse_newrecord(const uint8_t *payload)
{
  return get_u32(payload);
}

uint8_t proto_parse_attack(const uint8_t *payload)
{
  return payload[0];
}

uint8_t proto_cell_at(const uint8_t *board, int x, int y)
{
  int i = y * PROTO_BOARD_W + x;
  uint8_t byte = board[i / 2];
  return (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
}

int proto_expected_len(uint8_t type)
{
  switch (type) {
    case PROTO_TYPE_JOIN:      return PROTO_LEN_JOIN;
    case PROTO_TYPE_STATE:     return PROTO_LEN_STATE;
    case PROTO_TYPE_ATTACK:    return PROTO_LEN_ATTACK;
    case PROTO_TYPE_GAMEOVER:  return 0;
    case PROTO_TYPE_SKIP_WAIT: return 0;
    case PROTO_TYPE_CREDIT:    return PROTO_LEN_CREDIT;
    case PROTO_TYPE_LEADERBOARD: return PROTO_LEN_LEADERBOARD;
    case PROTO_TYPE_NEWRECORD: return PROTO_LEN_NEWRECORD;
    default:                   return -1;
  }
}

const char *proto_type_name(uint8_t type)
{
  switch (type) {
    case PROTO_TYPE_JOIN:      return "JOIN";
    case PROTO_TYPE_STATE:     return "STATE";
    case PROTO_TYPE_ATTACK:    return "ATTACK";
    case PROTO_TYPE_GAMEOVER:  return "GAMEOVER";
    case PROTO_TYPE_SKIP_WAIT: return "SKIP_WAIT";
    case PROTO_TYPE_CREDIT:    return "CREDIT";
    case PROTO_TYPE_LEADERBOARD: return "LEADERBOARD";
    case PROTO_TYPE_NEWRECORD: return "NEWRECORD";
    case PROTO_TYPE_START:     return "START";
    case PROTO_TYPE_ATTACK_TX: return "ATTACK_TX";
    case PROTO_TYPE_RESULT:    return "RESULT";
    default:                   return "?";
  }
}

static int is_known_type(uint8_t t)
{
  return proto_type_name(t)[0] != '?';
}

/* --- Incoming parser ------------------------------------------------------ */
void proto_parser_init(proto_parser_t *p)
{
  memset(p, 0, sizeof(*p));
  p->state = PROTO_WAIT_SYNC;
}

void proto_parser_feed_byte(proto_parser_t *p, uint8_t b,
                            proto_packet_cb on_packet,
                            proto_text_cb on_text, void *ctx)
{
  switch (p->state) {
    case PROTO_WAIT_SYNC:
      if (b == PROTO_SYNC) p->state = PROTO_GET_TYPE;
      else if (on_text)    on_text(b, ctx);
      break;

    case PROTO_GET_TYPE:
      if (b == PROTO_SYNC) {
        /* run of 0xAA: treat previous as false sync, keep waiting for TYPE */
      } else if (!is_known_type(b)) {
        /* 0xAA was printf noise, not a frame start: resync cleanly */
        p->state = PROTO_WAIT_SYNC;
      } else {
        p->type  = b;
        p->state = PROTO_GET_LEN;
      }
      break;

    case PROTO_GET_LEN:
      p->len = b;
      p->idx = 0;
      if (b > PROTO_MAX_PAYLOAD) {
        p->length_errors++;
        p->state = PROTO_WAIT_SYNC;
      } else if (b == 0) {
        p->state = PROTO_GET_CRC;
      } else {
        p->state = PROTO_GET_PAYLOAD;
      }
      break;

    case PROTO_GET_PAYLOAD:
      p->payload[p->idx++] = b;
      if (p->idx >= p->len) p->state = PROTO_GET_CRC;
      break;

    case PROTO_GET_CRC: {
      uint8_t body[2 + PROTO_MAX_PAYLOAD];
      body[0] = p->type;
      body[1] = p->len;
      memcpy(&body[2], p->payload, p->len);
      if (proto_crc8(body, (uint32_t)p->len + 2) == b) {
        if (on_packet) on_packet(p->type, p->payload, p->len, ctx);
      } else {
        p->crc_errors++;
      }
      p->state = PROTO_WAIT_SYNC;
      break;
    }
  }
}
