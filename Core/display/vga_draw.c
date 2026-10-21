/**
 * @file  vga_draw.c
 * @brief VGA framebuffer rendering — C port of tools/render.py.
 */
#include "vga_draw.h"
#include "font5x7.h"
#include "proto.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Cell colour code (0 empty, 1-7 = I O T S Z J L, 8 = garbage) -> palette.
 * Only 8 colours exist, so L reuses yellow and garbage reuses white. */
static const uint8_t CELL_COLOR[9] = {
  VGA_BLACK,    /* 0 empty   */
  VGA_CYAN,     /* 1 I       */
  VGA_YELLOW,   /* 2 O       */
  VGA_MAGENTA,  /* 3 T       */
  VGA_GREEN,    /* 4 S       */
  VGA_RED,      /* 5 Z       */
  VGA_BLUE,     /* 6 J       */
  VGA_YELLOW,   /* 7 L (no orange) */
  VGA_WHITE,    /* 8 garbage */
};

/* 4x4 spawn-orientation cells for the Next/Hold preview (piece 1-7). */
static const int8_t PIECE_SHAPES[8][4][2] = {
  {{0,0},{0,0},{0,0},{0,0}},           /* 0 unused */
  {{0,1},{1,1},{2,1},{3,1}},           /* I */
  {{1,0},{2,0},{1,1},{2,1}},           /* O */
  {{1,0},{0,1},{1,1},{2,1}},           /* T */
  {{1,0},{2,0},{0,1},{1,1}},           /* S */
  {{0,0},{1,0},{1,1},{2,1}},           /* Z */
  {{0,0},{0,1},{1,1},{2,1}},           /* J */
  {{2,0},{0,1},{1,1},{2,1}},           /* L */
};

/* --- primitives ----------------------------------------------------------- */

void vga_clear(uint8_t color)
{
  memset(framebuffer, color, sizeof(framebuffer));
}

void vga_fill_rect(int x, int y, int w, int h, uint8_t color)
{
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > VGA_W) w = VGA_W - x;
  if (y + h > VGA_H) h = VGA_H - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++)
    memset(&framebuffer[yy * VGA_W + x], color, w);
}

void vga_hline(int x, int y, int w, uint8_t color) { vga_fill_rect(x, y, w, 1, color); }
void vga_vline(int x, int y, int h, uint8_t color) { vga_fill_rect(x, y, 1, h, color); }

void vga_rect(int x, int y, int w, int h, uint8_t color)
{
  vga_hline(x, y, w, color);
  vga_hline(x, y + h - 1, w, color);
  vga_vline(x, y, h, color);
  vga_vline(x + w - 1, y, h, color);
}

static void draw_char(char c, int x, int y, uint8_t color, int scale)
{
  unsigned ch = (unsigned char)toupper((unsigned char)c);
  if (ch < FONT_FIRST || ch > FONT_LAST) return;
  const uint8_t *g = FONT5X7[ch - FONT_FIRST];
  for (int r = 0; r < FONT_H; r++) {
    uint8_t row = g[r];
    for (int col = 0; col < FONT_W; col++) {
      if (row & (1 << (FONT_W - 1 - col)))
        vga_fill_rect(x + col * scale, y + r * scale, scale, scale, color);
    }
  }
}

int vga_text_width(const char *s, int scale)
{
  return (int)strlen(s) * FONT_ADV * scale;
}

void vga_text(const char *s, int x, int y, uint8_t color, int scale)
{
  for (; *s; s++) {
    draw_char(*s, x, y, color, scale);
    x += FONT_ADV * scale;
  }
}

void vga_text_center(const char *s, int cx, int y, uint8_t color, int scale)
{
  vga_text(s, cx - vga_text_width(s, scale) / 2, y, color, scale);
}

/* seconds, rounded up, from a millisecond duration. */
static int ceil_secs(uint32_t ms) { return (int)((ms + 999) / 1000); }

/* --- board ---------------------------------------------------------------- */

static void draw_cell(int x, int y, uint8_t color, int cell)
{
  vga_fill_rect(x, y, cell - 1, cell - 1, color);
}

static void draw_board(const proto_state_t *st, int ox, int oy, int cell)
{
  for (int y = 0; y < PROTO_BOARD_H; y++) {
    for (int x = 0; x < PROTO_BOARD_W; x++) {
      uint8_t c = proto_cell_at(st->board, x, y);
      uint8_t color = (c <= 8) ? CELL_COLOR[c] : VGA_RED;
      draw_cell(ox + x * cell, oy + y * cell, color, cell);
    }
  }
  vga_rect(ox - 2, oy - 2, PROTO_BOARD_W * cell + 2, PROTO_BOARD_H * cell + 2,
           VGA_GREY);
}

static void draw_piece_box(const char *label, uint8_t piece, int x, int y)
{
  const int cell = 18;
  vga_text(label, x, y, VGA_GREY, VGA_FONT_SMALL);
  int box_y = y + 16;
  vga_rect(x, box_y, 4 * cell, 2 * cell, VGA_GREY);
  if (piece >= 1 && piece <= 7) {
    for (int i = 0; i < 4; i++) {
      int px = PIECE_SHAPES[piece][i][0];
      int py = PIECE_SHAPES[piece][i][1];
      draw_cell(x + px * cell, box_y + py * cell, CELL_COLOR[piece], cell);
    }
  }
}

static void draw_side_board(const hub_t *h, int p, int ox, int oy, int cell,
                            int compact)
{
  const hub_side_t *side = &h->sides[p];
  char buf[24];
  snprintf(buf, sizeof(buf), "P%d", p);
  vga_text(buf, ox, oy - 22, VGA_WHITE, VGA_FONT_SMALL);

  if (!side->has_state) {
    vga_rect(ox - 2, oy - 2, PROTO_BOARD_W * cell + 2,
             PROTO_BOARD_H * cell + 2, VGA_GREY);
    vga_text("WAITING...", ox + 4, oy + 6, VGA_GREY, VGA_FONT_SMALL);
    return;
  }
  const proto_state_t *st = &side->last_state;
  draw_board(st, ox, oy, cell);
  int px = ox + PROTO_BOARD_W * cell + 12;
  if (compact) {
    snprintf(buf, sizeof(buf), "S %lu", (unsigned long)st->score);
    vga_text(buf, px, oy, VGA_WHITE, VGA_FONT_SMALL);
    snprintf(buf, sizeof(buf), "L %u", st->lines);
    vga_text(buf, px, oy + 18, VGA_WHITE, VGA_FONT_SMALL);
    snprintf(buf, sizeof(buf), "LV %u", st->level);
    vga_text(buf, px, oy + 36, VGA_WHITE, VGA_FONT_SMALL);
  } else {
    draw_piece_box("NEXT", st->next, px, oy);
    draw_piece_box("HOLD", st->hold, px, oy + 80);
    snprintf(buf, sizeof(buf), "SCORE %lu", (unsigned long)st->score);
    vga_text(buf, px, oy + 180, VGA_WHITE, VGA_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "LINES %u", st->lines);
    vga_text(buf, px, oy + 202, VGA_WHITE, VGA_FONT_NORMAL);
    snprintf(buf, sizeof(buf), "LEVEL %u", st->level);
    vga_text(buf, px, oy + 224, VGA_WHITE, VGA_FONT_NORMAL);
    if (st->garbage_wait > 0) {
      snprintf(buf, sizeof(buf), "GARBAGE %u", st->garbage_wait);
      vga_text(buf, px, oy + 252, VGA_RED, VGA_FONT_NORMAL);
      int gw = st->garbage_wait < 20 ? st->garbage_wait : 20;
      vga_fill_rect(px, oy + 272, gw * 8, 10, VGA_RED);
    }
  }
}

static void draw_attack_msg(const hub_t *h, uint32_t now)
{
  if (h->attack_msg[0] && now - h->attack_msg_time < 2000)
    vga_text_center(h->attack_msg, VGA_W / 2, 12, VGA_RED, VGA_FONT_NORMAL);
}

static void draw_playing(const hub_t *h, uint32_t now)
{
  int players[2], n = 0;
  for (int p = 1; p <= 2; p++)
    if (h->sides[p].status == SIDE_IN_GAME) players[n++] = p;

  if (h->mode == 2 && n == 2) {
    int cell = 16;
    for (int i = 0; i < n; i++) {
      int ox = 20 + i * (PROTO_BOARD_W * cell + 100);
      draw_side_board(h, players[i], ox, 40, cell, 1);
    }
  } else {
    int p = n ? players[0] : 1;
    draw_side_board(h, p, 30, 40, 20, 0);
  }
  draw_attack_msg(h, now);
}

/* --- lobby screens -------------------------------------------------------- */

static void column_header(void)
{
  vga_text_center("PLAYER 1", VGA_W / 4, 40, VGA_WHITE, VGA_FONT_NORMAL);
  vga_text_center("PLAYER 2", 3 * VGA_W / 4, 40, VGA_WHITE, VGA_FONT_NORMAL);
  vga_vline(VGA_W / 2, 30, VGA_H - 90, VGA_GREY);
}

static void draw_lobby_idle(const hub_t *h)
{
  vga_text_center("TETRIS", VGA_W / 2, 70, VGA_WHITE, VGA_FONT_BIG);

  if (h->credit_ok) {
    vga_text_center("READY TO PLAY", VGA_W / 2, 175, VGA_GREEN, VGA_FONT_BIG);
    vga_text_center("PRESS START TO BEGIN", VGA_W / 2, 235, VGA_WHITE,
                    VGA_FONT_NORMAL);
  } else {
    vga_text_center("TAP YOUR CARD TO PLAY", VGA_W / 2, 185, VGA_YELLOW,
                    VGA_FONT_NORMAL);
    vga_text_center("PLACE CARD ON THE READER", VGA_W / 2, 225, VGA_GREY,
                    VGA_FONT_SMALL);
  }
}

static void draw_wait_partner(const hub_t *h, uint32_t now)
{
  column_header();
  const int cx[2] = { VGA_W / 4, 3 * VGA_W / 4 };
  for (int i = 0; i < 2; i++) {
    int p = i + 1;
    const hub_side_t *side = &h->sides[p];
    if (side->status == SIDE_READY) {
      vga_text_center("READY", cx[i], 110, VGA_GREEN, VGA_FONT_NORMAL);
      if (side->has_join && side->join.name[0])
        vga_text_center(side->join.name, cx[i], 135, VGA_WHITE, VGA_FONT_NORMAL);
      vga_text_center("PRESS AGAIN", cx[i], 175, VGA_GREY, VGA_FONT_SMALL);
      vga_text_center("TO START SOLO", cx[i], 192, VGA_GREY, VGA_FONT_SMALL);
    } else {
      vga_text_center("PRESS TO", cx[i], 120, VGA_GREY, VGA_FONT_NORMAL);
      vga_text_center("JOIN 2P", cx[i], 142, VGA_GREY, VGA_FONT_NORMAL);
    }
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "STARTING 1P IN  %d",
           ceil_secs(hub_countdown_remaining(h, now)));
  vga_text_center(buf, VGA_W / 2, VGA_H - 90, VGA_YELLOW, VGA_FONT_NORMAL);
}

static void draw_countdown(const hub_t *h, uint32_t now)
{
  vga_text_center(h->mode == 2 ? "VS" : "1 PLAYER", VGA_W / 2, 90,
                  VGA_WHITE, VGA_FONT_BIG);
  int n = ceil_secs(hub_countdown_remaining(h, now));
  if (n < 1) n = 1;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", n);
  vga_text_center(buf, VGA_W / 2, 180, VGA_YELLOW, VGA_FONT_HUGE);
}

static void draw_leaderboard(const hub_t *h, int y)
{
  vga_text_center("LEADERBOARD", VGA_W / 2, y, VGA_YELLOW, VGA_FONT_NORMAL);
  if (h->lb_count <= 0) {
    vga_text_center("(no scores yet)", VGA_W / 2, y + 28, VGA_GREY,
                    VGA_FONT_SMALL);
    return;
  }
  for (int i = 0; i < h->lb_count && i < HUB_LB_MAX; i++) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%d. %-10s %lu",
             i + 1, h->lb[i].name, (unsigned long)h->lb[i].points);
    vga_text_center(buf, VGA_W / 2, y + 26 + i * 20,
                    i == 0 ? VGA_GREEN : VGA_WHITE, VGA_FONT_SMALL);
  }
}

static void draw_result_show(const hub_t *h)
{
  /* count sides that actually played, to center a single player */
  int shown = 0;
  for (int p = 1; p <= 2; p++)
    if (!(h->sides[p].result < 0 && !h->sides[p].has_state)) shown++;

  const int cx2[2] = { VGA_W / 4, 3 * VGA_W / 4 };
  int idx = 0;
  for (int p = 1; p <= 2; p++) {
    const hub_side_t *side = &h->sides[p];
    if (side->result < 0 && !side->has_state) continue;
    int cx = (shown == 1) ? VGA_W / 2 : cx2[idx++];
    if (side->result == RESULT_WIN)
      vga_text_center("WIN!", cx, 60, VGA_GREEN, VGA_FONT_BIG);
    else if (side->result == RESULT_LOSE)
      vga_text_center("LOSE", cx, 60, VGA_RED, VGA_FONT_BIG);
    else
      vga_text_center("GAME OVER", cx, 60, VGA_YELLOW, VGA_FONT_NORMAL);
    char buf[24];
    uint32_t score = side->has_state ? side->last_state.score : 0;
    snprintf(buf, sizeof(buf), "SCORE %lu", (unsigned long)score);
    vga_text_center(buf, cx, 110, VGA_WHITE, VGA_FONT_NORMAL);
  }

  int lb_y = 170;
  if (h->new_record) {
    char buf[32];
    vga_text_center("NEW HIGH SCORE!", VGA_W / 2, 150, VGA_GREEN, VGA_FONT_BIG);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)h->record_points);
    vga_text_center(buf, VGA_W / 2, 200, VGA_YELLOW, VGA_FONT_NORMAL);
    lb_y = 240;
  }
  draw_leaderboard(h, lb_y);
}

static const char *lobby_name(lobby_state_t s)
{
  switch (s) {
    case LOBBY_IDLE:   return "LOBBY_IDLE";
    case WAIT_PARTNER: return "WAIT_PARTNER";
    case COUNTDOWN:    return "COUNTDOWN";
    case PLAYING:      return "PLAYING";
    case RESULT_SHOW:  return "RESULT_SHOW";
    default:           return "?";
  }
}

/* --- debug panel ---------------------------------------------------------- */

static void draw_debug(const hub_t *h, uint32_t now)
{
  const hub_debug_t *s = &h->stats;
  const int panel_x = VGA_W - 220;
  int bx = panel_x + 8;
  int line = 0;
  char buf[HUB_WARN_LEN];

  vga_fill_rect(panel_x, 0, 220, VGA_H, VGA_BLACK);
  vga_vline(panel_x, 0, VGA_H, VGA_GREY);

  vga_text("-- DEBUG --", bx, 8, VGA_WHITE, VGA_FONT_SMALL);
  line = 1;
  for (int i = 0; i < 8; i++) {
    if (s->pkt_per_sec[i] == 0) continue;
    snprintf(buf, sizeof(buf), "%s: %lu/S",
             proto_type_name(hub_debug_slot_type(i)),
             (unsigned long)s->pkt_per_sec[i]);
    vga_text(buf, bx, 8 + line++ * 18, VGA_WHITE, VGA_FONT_SMALL);
  }
  snprintf(buf, sizeof(buf), "CRC_ERR: %lu", (unsigned long)s->crc_errors);
  vga_text(buf, bx, 8 + line++ * 18, VGA_WHITE, VGA_FONT_SMALL);
  snprintf(buf, sizeof(buf), "LEN_ERR: %lu", (unsigned long)s->length_errors);
  vga_text(buf, bx, 8 + line++ * 18, VGA_WHITE, VGA_FONT_SMALL);
  snprintf(buf, sizeof(buf), "LOST: %lu", (unsigned long)s->lost_packets);
  vga_text(buf, bx, 8 + line++ * 18, VGA_WHITE, VGA_FONT_SMALL);
  snprintf(buf, sizeof(buf), "UNKNOWN: %lu", (unsigned long)s->unknown_types);
  vga_text(buf, bx, 8 + line++ * 18, VGA_WHITE, VGA_FONT_SMALL);
  snprintf(buf, sizeof(buf), "SKIP_WAIT: %d", h->skip_count);
  vga_text(buf, bx, 8 + line++ * 18, VGA_WHITE, VGA_FONT_SMALL);

  uint32_t age = s->last_pkt_time ? (now - s->last_pkt_time) : 0;
  snprintf(buf, sizeof(buf), "LAST PKT: %lu.%02luS",
           (unsigned long)(age / 1000), (unsigned long)((age % 1000) / 10));
  vga_text(buf, bx, 8 + line++ * 18, (age > 500) ? VGA_RED : VGA_WHITE,
           VGA_FONT_SMALL);

  /* most recent warnings (ring buffer), up to 6 newest */
  line++;
  int show = s->warn_count < 6 ? s->warn_count : 6;
  for (int k = 0; k < show; k++) {
    int idx = (s->warn_head - 1 - k + 2 * HUB_WARN_SLOTS) % HUB_WARN_SLOTS;
    vga_text(s->warnings[idx], bx, 8 + line++ * 18, VGA_RED, VGA_FONT_SMALL);
  }
}

/* --- credit / pay overlay ------------------------------------------------- */

#define CREDIT_DENY_MS 3000   /* how long the red "not enough" banner stays */
#define CREDIT_SHOW_MS 4000   /* how long the BALANCE banner stays after a tap */

static void draw_credit(const hub_t *h, uint32_t now)
{
  /* balance banner: show briefly after a paid tap, then fade out */
  if (h->credit_time && now - h->credit_time < CREDIT_SHOW_MS) {
    char buf[40];
    if (h->credit_balance > 0)
      snprintf(buf, sizeof(buf), "BALANCE: %ld   -%ld",
               (long)h->credit_balance, (long)h->credit_spent);
    else
      snprintf(buf, sizeof(buf), "PAID   -%ld", (long)h->credit_spent);
    vga_text_center(buf, VGA_W / 2, 22, VGA_GREEN, VGA_FONT_NORMAL);
  }

  /* transient red overlay after a denied tap */
  if (h->deny_time && now - h->deny_time < CREDIT_DENY_MS) {
    const int bw = 380, bh = 96;
    const int bx = (VGA_W - bw) / 2, by = (VGA_H - bh) / 2;
    const char *title, *hint;
    if (h->deny_reason == PROTO_CREDIT_NO_CARD) {
      title = "UNKNOWN CARD";
      hint  = "CARD NOT REGISTERED";
    } else {
      title = "NOT ENOUGH CREDIT";
      hint  = "TAP A CARD WITH BALANCE";
    }
    vga_fill_rect(bx, by, bw, bh, VGA_RED);
    vga_rect(bx, by, bw, bh, VGA_WHITE);
    vga_text_center(title, VGA_W / 2, by + 22, VGA_WHITE, VGA_FONT_BIG);
    vga_text_center(hint, VGA_W / 2, by + 62, VGA_WHITE, VGA_FONT_SMALL);
  }
}

/* --- main ----------------------------------------------------------------- */

void vga_render(const hub_t *h, int show_debug, uint32_t now)
{
  vga_clear(VGA_DARK);

  switch (h->lobby) {
    case LOBBY_IDLE:   draw_lobby_idle(h);           break;
    case WAIT_PARTNER: draw_wait_partner(h, now);    break;
    case COUNTDOWN:    draw_countdown(h, now);       break;
    case PLAYING:      draw_playing(h, now);         break;
    case RESULT_SHOW:  draw_result_show(h);          break;
    default: break;
  }

  /* "PLEASE WAIT" banner for a side that pressed during a game (item 9). */
  if (h->lobby == PLAYING || h->lobby == RESULT_SHOW) {
    char buf[40];
    int n = 0;
    buf[0] = '\0';
    for (int p = 1; p <= 2; p++) {
      if (h->sides[p].pending_join) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%sP%d", n ? "," : "", p);
        strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        n++;
      }
    }
    if (n) {
      char line[72];
      snprintf(line, sizeof(line), "PLEASE WAIT (%s QUEUED)", buf);
      vga_text_center(line, VGA_W / 2, VGA_H - 60, VGA_YELLOW, VGA_FONT_SMALL);
    }
  }

  char status[40];
  snprintf(status, sizeof(status), "HUB: %s  MODE:%d",
           lobby_name(h->lobby), h->mode);
  vga_text(status, 20, VGA_H - 24, VGA_GREY, VGA_FONT_SMALL);

  draw_credit(h, now);

  if (show_debug) draw_debug(h, now);
}
