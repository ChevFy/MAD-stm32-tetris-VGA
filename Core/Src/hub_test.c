/**
 * @file  hub_test.c
 * @brief On-device parser self-test, lobby tests, and a fake board.
 *        Ports hub_sim.py::run_selftest, test_lobby.py, and fake_board.py.
 */
#include "hub_test.h"
#include "proto.h"
#include <string.h>

/* ========================================================================= */
/* Packet collector (used by both the self-test and the lobby tests)         */
/* ========================================================================= */

#define COLLECT_MAX 24

typedef struct {
  uint8_t  types[COLLECT_MAX];
  uint8_t  payloads[COLLECT_MAX][PROTO_MAX_PAYLOAD];
  uint8_t  lens[COLLECT_MAX];
  int      count;
  uint8_t  text[64];
  int      text_len;
} collector_t;

static void collect_pkt(uint8_t type, const uint8_t *pl, uint8_t len, void *ctx)
{
  collector_t *c = ctx;
  if (c->count >= COLLECT_MAX) return;
  c->types[c->count] = type;
  memcpy(c->payloads[c->count], pl, len);
  c->lens[c->count] = len;
  c->count++;
}

static void collect_text(uint8_t b, void *ctx)
{
  collector_t *c = ctx;
  if (c->text_len < (int)sizeof(c->text) - 1) c->text[c->text_len++] = b;
}

/* ========================================================================= */
/* Parser self-test                                                          */
/* ========================================================================= */

static int little_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

int hub_selftest(void)
{
  if (proto_crc8((const uint8_t *)"123456789", 9) != 0xF4) return 1;

  uint8_t good0[8], good1[8], good2[16], good3[8], good4[PROTO_MAX_PACKET];
  int n0 = proto_build_attack(good0, 3);              /* ATTACK_TX */
  int n1 = proto_build_result(good1, 1);              /* RESULT */
  int n2 = proto_build_start(good2, 1, 0x12345678);   /* START */
  int n3 = proto_build_packet(good3, PROTO_TYPE_GAMEOVER, NULL, 0);

  uint8_t state_pl[PROTO_LEN_STATE];
  memset(state_pl, 0, sizeof(state_pl));
  state_pl[0] = 1; state_pl[1] = 7;          /* player, seq */
  state_pl[102] = 3;                          /* next */
  state_pl[104] = 100;                        /* score LE */
  state_pl[108] = 2;                          /* lines LE */
  state_pl[110] = 1;                          /* level */
  int n4 = proto_build_packet(good4, PROTO_TYPE_STATE, state_pl, PROTO_LEN_STATE);

  uint8_t bad_crc[8];
  int nb = proto_build_attack(bad_crc, 9);
  bad_crc[nb - 1] ^= 0xFF;                    /* corrupt CRC */

  uint8_t oversize[13] = { PROTO_SYNC, PROTO_TYPE_STATE, 200,
                           0,0,0,0,0,0,0,0,0,0 };

  /* Build the messy stream. */
  uint8_t stream[256];
  int len = 0;
  const char *junk = "garbage printf here\n";
  while (*junk) stream[len++] = (uint8_t)*junk++;
  memcpy(&stream[len], good0, n0); len += n0;
  memcpy(&stream[len], bad_crc, nb); len += nb;
  stream[len++] = 0xAA; stream[len++] = 0xAA;   /* stray syncs */
  memcpy(&stream[len], good1, n1); len += n1;
  memcpy(&stream[len], oversize, sizeof(oversize)); len += sizeof(oversize);
  memcpy(&stream[len], good2, n2); len += n2;
  memcpy(&stream[len], good3, n3); len += n3;
  memcpy(&stream[len], good4, n4); len += n4;

  /* Feed in 3-byte chunks to exercise state saving. */
  proto_parser_t parser;
  proto_parser_init(&parser);
  collector_t c;
  memset(&c, 0, sizeof(c));
  for (int i = 0; i < len; i++)
    proto_parser_feed_byte(&parser, stream[i], collect_pkt, collect_text, &c);

  static const uint8_t expected[] = {
    PROTO_TYPE_ATTACK_TX, PROTO_TYPE_RESULT, PROTO_TYPE_START,
    PROTO_TYPE_GAMEOVER, PROTO_TYPE_STATE };
  if (c.count != 5) return 1;
  for (int i = 0; i < 5; i++) if (c.types[i] != expected[i]) return 1;
  if (parser.crc_errors != 1 || parser.length_errors != 1) return 1;
  c.text[c.text_len] = '\0';
  if (strstr((char *)c.text, "garbage printf here") == NULL) return 1;

  /* also sanity-check a START round-trips to the right seed */
  if (little_u32(&c.payloads[2][1]) != 0x12345678) return 1;

  return 0;
}

/* ========================================================================= */
/* Lobby tests (test_lobby.py)                                               */
/* ========================================================================= */

typedef struct { uint8_t buf[512]; int len; } txcap_t;

static void tx_send(const uint8_t *pkt, int len, void *ctx)
{
  txcap_t *t = ctx;
  if (t->len + len <= (int)sizeof(t->buf)) {
    memcpy(&t->buf[t->len], pkt, len);
    t->len += len;
  }
}

static void decode_sent(const txcap_t *t, collector_t *c)
{
  proto_parser_t p;
  proto_parser_init(&p);
  memset(c, 0, sizeof(*c));
  for (int i = 0; i < t->len; i++)
    proto_parser_feed_byte(&p, t->buf[i], collect_pkt, NULL, c);
}

static int count_starts(const txcap_t *t, int *mode, uint32_t *seed)
{
  collector_t c;
  decode_sent(t, &c);
  int n = 0;
  for (int i = 0; i < c.count; i++) {
    if (c.types[i] == PROTO_TYPE_START) {
      if (n == 0) {
        if (mode) *mode = c.payloads[i][0];
        if (seed) *seed = (uint32_t)little_u32(&c.payloads[i][1]);
      }
      n++;
    }
  }
  return n;
}

static int count_results(const txcap_t *t, int *last_val)
{
  collector_t c;
  decode_sent(t, &c);
  int n = 0;
  for (int i = 0; i < c.count; i++) {
    if (c.types[i] == PROTO_TYPE_RESULT) {
      if (last_val) *last_val = c.payloads[i][0];
      n++;
    }
  }
  return n;
}

static void feed_join(hub_t *h, int player, uint32_t t)
{
  uint8_t pl[PROTO_LEN_JOIN];
  char name[4];
  name[0] = 'P'; name[1] = '0' + player; name[2] = '\0';
  proto_make_join_payload(pl, (uint8_t)player, 0, name, 0);
  hub_handle(h, PROTO_TYPE_JOIN, pl, PROTO_LEN_JOIN, t);
}

static void feed_state(hub_t *h, int player, uint8_t seq, uint32_t score,
                       uint32_t t)
{
  uint8_t pl[PROTO_LEN_STATE];
  memset(pl, 0, sizeof(pl));
  pl[0] = (uint8_t)player;
  pl[1] = seq;
  pl[102] = 1;               /* next = 1 (valid) */
  pl[104] = score & 0xFF;
  pl[105] = (score >> 8) & 0xFF;
  pl[106] = (score >> 16) & 0xFF;
  pl[107] = (score >> 24) & 0xFF;
  pl[110] = 1;               /* level */
  hub_handle(h, PROTO_TYPE_STATE, pl, PROTO_LEN_STATE, t);
}

#define CHECK(cond) do { total++; if (cond) passed++; } while (0)

int hub_lobby_tests(int *passed_out, int *total_out)
{
  int passed = 0, total = 0;
  hub_t h;
  txcap_t cap;
  int mode; uint32_t seed; int rc;

  /* --- A: solo, partner-wait timeout --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  for (uint32_t t = 0; t <= 10000; t += 500) { feed_join(&h, 1, t); hub_update(&h, t); }
  CHECK(h.sides[1].status == SIDE_READY);
  hub_update(&h, 10100);
  CHECK(h.lobby == COUNTDOWN && h.mode == 1);
  hub_update(&h, 13200);
  CHECK(h.lobby == PLAYING && h.sides[1].status == SIDE_IN_GAME);
  rc = count_starts(&cap, &mode, &seed);
  CHECK(rc == 1 && mode == 1 && seed == 0xABCD);

  /* --- B: solo via SKIP_WAIT --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  feed_join(&h, 1, 0);
  hub_handle(&h, PROTO_TYPE_SKIP_WAIT, NULL, 0, 1000);
  CHECK(h.lobby == COUNTDOWN && h.mode == 1);
  hub_update(&h, 4100);
  CHECK(h.lobby == PLAYING);
  CHECK(count_starts(&cap, &mode, &seed) == 1 && mode == 1 && seed == 0xABCD);

  /* --- C: two players --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  feed_join(&h, 1, 0);
  feed_join(&h, 2, 1000);
  CHECK(h.lobby == COUNTDOWN && h.mode == 2);
  hub_update(&h, 4100);
  CHECK(h.lobby == PLAYING);
  rc = count_starts(&cap, &mode, &seed);
  CHECK(rc == 2 && mode == 2 && seed == 0xABCD);
  CHECK(h.sides[1].status == SIDE_IN_GAME && h.sides[2].status == SIDE_IN_GAME);

  /* --- D: skip wins the race, late join queued --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  feed_join(&h, 1, 0);
  hub_handle(&h, PROTO_TYPE_SKIP_WAIT, NULL, 0, 1000);
  CHECK(h.lobby == COUNTDOWN && h.mode == 1);
  feed_join(&h, 2, 1500);
  CHECK(h.sides[2].status == SIDE_EMPTY && h.sides[2].pending_join == 1);
  hub_update(&h, 4200);
  CHECK(h.lobby == PLAYING && count_starts(&cap, NULL, NULL) == 1);

  /* --- ready side drops when silent --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  feed_join(&h, 1, 0);
  CHECK(h.lobby == WAIT_PARTNER);
  hub_update(&h, 2500);
  CHECK(h.sides[1].status == SIDE_EMPTY && h.lobby == LOBBY_IDLE);

  /* --- STATE without START flips to PLAYING --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  feed_state(&h, 1, 0, 0, 0);
  CHECK(h.lobby == PLAYING && h.sides[1].status == SIDE_IN_GAME);

  /* --- GAMEOVER sends RESULT and shows, then resets --- */
  memset(&cap, 0, sizeof(cap));
  hub_init(&h, tx_send, &cap); hub_set_seed(&h, 0xABCD);
  feed_join(&h, 1, 0);
  hub_update(&h, 10100); hub_update(&h, 13200);     /* -> PLAYING */
  feed_state(&h, 1, 1, 1234, 14000);
  hub_handle(&h, PROTO_TYPE_GAMEOVER, NULL, 0, 15000);
  { int val = -1; rc = count_results(&cap, &val);
    CHECK(rc == 1 && val == RESULT_SINGLE_DONE); }
  CHECK(h.lobby == RESULT_SHOW);
  hub_update(&h, 20100);
  CHECK(h.lobby == LOBBY_IDLE && h.sides[1].status == SIDE_EMPTY);

  if (passed_out) *passed_out = passed;
  if (total_out)  *total_out  = total;
  return (passed == total) ? 0 : 1;
}

/* ========================================================================= */
/* Fake board (fake_board.py) — drives the Hub with a synthetic game         */
/* ========================================================================= */

#define FB_FRAME_MS   110
#define FB_RESTART_MS 4000

/* spawn-cell offsets (same shape table as vga_draw.c, restated here) */
static const int8_t FB_SHAPES[8][4][2] = {
  {{0,0},{0,0},{0,0},{0,0}},
  {{0,1},{1,1},{2,1},{3,1}}, {{1,0},{2,0},{1,1},{2,1}},
  {{1,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{0,1},{1,1}},
  {{0,0},{1,0},{1,1},{2,1}}, {{0,0},{0,1},{1,1},{2,1}},
  {{2,0},{0,1},{1,1},{2,1}},
};
#define PIECE_SHAPES_x(p, i) (FB_SHAPES[p][i][0])
#define PIECE_SHAPES_y(p, i) (FB_SHAPES[p][i][1])

static uint32_t fb_rng(fake_board_t *fb)
{
  fb->rng = fb->rng * 1664525u + 1013904223u;
  return fb->rng;
}

static uint8_t fb_next_piece(fake_board_t *fb)
{
  return (uint8_t)(1 + (fb_rng(fb) >> 16) % 7);
}

static int fb_collides(const fake_board_t *fb, uint8_t piece, int px, int py)
{
  for (int i = 0; i < 4; i++) {
    int cx = px + PIECE_SHAPES_x(piece, i);
    int cy = py + PIECE_SHAPES_y(piece, i);
    if (cx < 0 || cx >= PROTO_BOARD_W || cy >= PROTO_BOARD_H) return 1;
    if (cy >= 0 && fb->grid[cy][cx]) return 1;
  }
  return 0;
}

static void fb_reset(fake_board_t *fb)
{
  memset(fb->grid, 0, sizeof(fb->grid));
  fb->score = 0; fb->lines = 0; fb->level = 1; fb->hold = 0;
  fb->seq = 0; fb->frame = 0;
  fb->cur = fb_next_piece(fb);
  fb->nxt = fb_next_piece(fb);
  fb->px = 3; fb->py = 0;
  fb->restart_at = 0;
}

void fake_board_init(fake_board_t *fb, uint32_t seed)
{
  memset(fb, 0, sizeof(*fb));
  fb->rng = seed ? seed : 1u;
  fb->started = 0;
  fb->next_tick = 0;
  fb_reset(fb);
}

static void fb_encode_board(const fake_board_t *fb, uint8_t view[20][10],
                            uint8_t out[100])
{
  memset(out, 0, 100);
  for (int y = 0; y < PROTO_BOARD_H; y++) {
    for (int x = 0; x < PROTO_BOARD_W; x++) {
      int i = y * PROTO_BOARD_W + x;
      if (i % 2 == 0) out[i / 2] |= (view[y][x] & 0x0F) << 4;
      else            out[i / 2] |= (view[y][x] & 0x0F);
    }
  }
}

static void fb_emit_state(fake_board_t *fb, hub_t *h, uint32_t now,
                          uint8_t garbage_wait)
{
  uint8_t view[20][10];
  memcpy(view, fb->grid, sizeof(view));
  for (int i = 0; i < 4; i++) {
    int cy = fb->py + PIECE_SHAPES_y(fb->cur, i);
    int cx = fb->px + PIECE_SHAPES_x(fb->cur, i);
    if (cy >= 0 && cy < PROTO_BOARD_H) view[cy][cx] = fb->cur;
  }
  uint8_t pl[PROTO_LEN_STATE];
  memset(pl, 0, sizeof(pl));
  pl[0] = 1;
  pl[1] = fb->seq;
  fb_encode_board(fb, view, &pl[2]);
  pl[102] = fb->nxt;
  pl[103] = fb->hold;
  pl[104] = fb->score & 0xFF;
  pl[105] = (fb->score >> 8) & 0xFF;
  pl[106] = (fb->score >> 16) & 0xFF;
  pl[107] = (fb->score >> 24) & 0xFF;
  pl[108] = fb->lines & 0xFF;
  pl[109] = (fb->lines >> 8) & 0xFF;
  pl[110] = fb->level;
  pl[111] = garbage_wait;
  hub_handle(h, PROTO_TYPE_STATE, pl, PROTO_LEN_STATE, now);
}

void fake_board_step(fake_board_t *fb, hub_t *h, uint32_t now)
{
  if (now < fb->next_tick) return;
  fb->next_tick = now + FB_FRAME_MS;

  if (!fb->started) {
    uint8_t pl[PROTO_LEN_JOIN];
    proto_make_join_payload(pl, 1, 0, "", 0);
    hub_handle(h, PROTO_TYPE_JOIN, pl, PROTO_LEN_JOIN, now);
    fb->started = 1;
    return;
  }

  /* waiting to restart after a game over */
  if (fb->restart_at) {
    if (now >= fb->restart_at) { fb->started = 0; fb_reset(fb); }
    return;
  }

  if (!fb_collides(fb, fb->cur, fb->px, fb->py + 1)) {
    fb->py++;
  } else {
    /* lock the piece */
    for (int i = 0; i < 4; i++) {
      int cy = fb->py + PIECE_SHAPES_y(fb->cur, i);
      int cx = fb->px + PIECE_SHAPES_x(fb->cur, i);
      if (cy >= 0 && cy < PROTO_BOARD_H) fb->grid[cy][cx] = fb->cur;
    }
    /* clear full rows */
    int cleared = 0;
    for (int y = PROTO_BOARD_H - 1; y >= 0; y--) {
      int full = 1;
      for (int x = 0; x < PROTO_BOARD_W; x++) if (!fb->grid[y][x]) { full = 0; break; }
      if (full) {
        for (int yy = y; yy > 0; yy--)
          memcpy(fb->grid[yy], fb->grid[yy - 1], PROTO_BOARD_W);
        memset(fb->grid[0], 0, PROTO_BOARD_W);
        cleared++;
        y++;   /* recheck same row */
      }
    }
    if (cleared) {
      static const uint32_t SCORES[5] = {0, 100, 300, 500, 800};
      fb->lines += cleared;
      fb->score += SCORES[cleared <= 4 ? cleared : 4];
      fb->level = 1 + fb->lines / 10;
    }
    /* occasional hold */
    if (fb->hold == 0 && (fb_rng(fb) % 100) < 15) fb->hold = fb->cur;
    fb->cur = fb->nxt;
    fb->nxt = fb_next_piece(fb);
    fb->px = 3; fb->py = 0;
    if (fb_collides(fb, fb->cur, fb->px, fb->py)) {
      hub_handle(h, PROTO_TYPE_GAMEOVER, NULL, 0, now);
      fb->restart_at = now + FB_RESTART_MS;
      return;
    }
  }

  /* occasional incoming attack from the "other player" */
  if (fb->frame && fb->frame % 40 == 0) {
    uint8_t pl = 2;
    hub_handle(h, PROTO_TYPE_ATTACK, &pl, 1, now);
  }

  uint8_t garbage_wait = (fb->frame % 50 < 2) ? (2 - fb->frame % 50) : 0;
  fb->seq++;
  fb->frame++;
  fb_emit_state(fb, h, now, garbage_wait);
}
