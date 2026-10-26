/**
 * @file  hub.c
 * @brief Lobby + game Hub state machine — C port of hub_sim.py::Hub.
 */
#include "hub.h"
#include <string.h>
#include <stdio.h>

/* Default timings (ms). */
static const uint32_t DEFAULT_TIMINGS[T_COUNT] = {
  10000,  /* partner_wait */
  3000,   /* countdown */
  5000,   /* result_show */
  2000,   /* alive_timeout */
};

/* --- debug stats ---------------------------------------------------------- */

static int type_index(uint8_t type)
{
  switch (type) {
    case PROTO_TYPE_JOIN:      return 0;
    case PROTO_TYPE_STATE:     return 1;
    case PROTO_TYPE_ATTACK:    return 2;
    case PROTO_TYPE_GAMEOVER:  return 3;
    case PROTO_TYPE_SKIP_WAIT: return 4;
    case PROTO_TYPE_START:     return 5;
    case PROTO_TYPE_ATTACK_TX: return 6;
    case PROTO_TYPE_RESULT:    return 7;
    default:                   return -1;
  }
}

/* type for a pkt_per_sec slot index (inverse of type_index), for display. */
uint8_t hub_debug_slot_type(int slot)
{
  static const uint8_t t[8] = {
    PROTO_TYPE_JOIN, PROTO_TYPE_STATE, PROTO_TYPE_ATTACK, PROTO_TYPE_GAMEOVER,
    PROTO_TYPE_SKIP_WAIT, PROTO_TYPE_START, PROTO_TYPE_ATTACK_TX,
    PROTO_TYPE_RESULT };
  return (slot >= 0 && slot < 8) ? t[slot] : 0;
}

void hub_warn(hub_debug_t *s, const char *msg)
{
  strncpy(s->warnings[s->warn_head], msg, HUB_WARN_LEN - 1);
  s->warnings[s->warn_head][HUB_WARN_LEN - 1] = '\0';
  s->warn_head = (s->warn_head + 1) % HUB_WARN_SLOTS;
  if (s->warn_count < HUB_WARN_SLOTS) s->warn_count++;
}

void hub_debug_note(hub_debug_t *s, uint8_t type, uint32_t now)
{
  int i = type_index(type);
  if (i >= 0) s->pkt_window[i]++;
  s->last_pkt_time = now;
}

void hub_debug_tick_window(hub_debug_t *s, uint32_t now)
{
  if (now - s->window_start >= 1000) {
    memcpy(s->pkt_per_sec, s->pkt_window, sizeof(s->pkt_per_sec));
    memset(s->pkt_window, 0, sizeof(s->pkt_window));
    s->window_start = now;
  }
}

static void check_seq(hub_debug_t *s, uint8_t seq)
{
  if (s->last_seq >= 0) {
    uint8_t expected = (uint8_t)(s->last_seq + 1);
    if (seq != expected) {
      uint8_t gap = (uint8_t)(seq - expected);
      s->lost_packets += gap;
      char buf[HUB_WARN_LEN];
      snprintf(buf, sizeof(buf), "seq jump %d->%d (lost %d)",
               s->last_seq, seq, gap);
      hub_warn(s, buf);
    }
  }
  s->last_seq = seq;
}

static void validate_state(const proto_state_t *st, hub_debug_t *s)
{
  char buf[HUB_WARN_LEN];
  if (!(st->next >= 1 && st->next <= 7)) {
    snprintf(buf, sizeof(buf), "next out of range: %d", st->next);
    hub_warn(s, buf);
  }
  if (st->hold > 7) {
    snprintf(buf, sizeof(buf), "hold out of range: %d", st->hold);
    hub_warn(s, buf);
  }
  for (int i = 0; i < PROTO_BOARD_W * PROTO_BOARD_H; i++) {
    uint8_t c = proto_cell_at(st->board, i % PROTO_BOARD_W, i / PROTO_BOARD_W);
    if (c > 8) {
      snprintf(buf, sizeof(buf), "bad cell color %d at i=%d", c, i);
      hub_warn(s, buf);
      break;
    }
  }
}

/* --- init ----------------------------------------------------------------- */

void hub_init(hub_t *h, hub_send_fn send, void *send_ctx)
{
  memset(h, 0, sizeof(*h));
  h->send = send;
  h->send_ctx = send_ctx;
  h->lobby = LOBBY_IDLE;
  h->mode = 1;
  h->rng = 0x12345678u;
  h->stats.last_seq = -1;
  h->sides[1].result = -1;
  h->sides[2].result = -1;
  memcpy(h->timings, DEFAULT_TIMINGS, sizeof(h->timings));
}

void hub_set_seed(hub_t *h, uint32_t seed)
{
  h->fixed_seed = seed;
  h->has_fixed_seed = 1;
}

void hub_seed_rng(hub_t *h, uint32_t entropy)
{
  h->rng ^= entropy ? entropy : 1u;
}

static uint32_t rng_next(hub_t *h)
{
  h->rng = h->rng * 1664525u + 1013904223u;   /* Numerical Recipes LCG */
  return h->rng;
}

/* --- helpers -------------------------------------------------------------- */

static int ready_count(const hub_t *h)
{
  int n = 0;
  for (int p = 1; p <= 2; p++) if (h->sides[p].status == SIDE_READY) n++;
  return n;
}

static int in_game_count(const hub_t *h)
{
  int n = 0;
  for (int p = 1; p <= 2; p++) if (h->sides[p].status == SIDE_IN_GAME) n++;
  return n;
}

uint32_t hub_countdown_remaining(const hub_t *h, uint32_t now)
{
  return (now < h->deadline) ? (h->deadline - now) : 0;
}

/* --- outgoing ------------------------------------------------------------- */

static void emit(hub_t *h, const uint8_t *pkt, int len)
{
  if (h->send) h->send(pkt, len, h->send_ctx);
}

void hub_send_attack(hub_t *h, uint8_t rows)
{
  uint8_t pkt[PROTO_MAX_PACKET];
  int n = proto_build_attack(pkt, rows);
  emit(h, pkt, n);
}

static void send_result(hub_t *h, int player, int result)
{
  uint8_t pkt[PROTO_MAX_PACKET];
  h->sides[player].result = result;
  int n = proto_build_result(pkt, (uint8_t)result);
  emit(h, pkt, n);
}

static void start_countdown(hub_t *h, uint32_t now)
{
  h->lobby = COUNTDOWN;
  h->deadline = now + h->timings[T_COUNTDOWN];
  h->skip_count = 0;
}

static void send_start_all(hub_t *h)
{
  uint8_t pkt[PROTO_MAX_PACKET];
  h->seed = h->has_fixed_seed ? h->fixed_seed : rng_next(h);
  for (int p = 1; p <= 2; p++) {
    if (h->sides[p].status == SIDE_READY) {
      int n = proto_build_start(pkt, (uint8_t)h->mode, h->seed);
      emit(h, pkt, n);
      h->sides[p].status = SIDE_IN_GAME;
    }
  }
  h->lobby = PLAYING;
}

static void enter_result_show(hub_t *h, uint32_t now)
{
  h->lobby = RESULT_SHOW;
  h->deadline = now + h->timings[T_RESULT_SHOW];
}

static void reset_lobby(hub_t *h)
{
  for (int p = 1; p <= 2; p++) {
    hub_side_t *s = &h->sides[p];
    s->status = SIDE_EMPTY;
    s->has_join = 0;
    s->has_state = 0;
    s->result = -1;
    s->pending_join = 0;
  }
  h->lobby = LOBBY_IDLE;
  h->mode = 1;
}

/* --- incoming ------------------------------------------------------------- */

static void on_join(hub_t *h, const proto_join_t *join, uint32_t now)
{
  int p = (join->player == 1 || join->player == 2) ? join->player : 1;
  hub_side_t *side = &h->sides[p];
  side->last_join_time = now;

  if (h->lobby == PLAYING || h->lobby == RESULT_SHOW) {
    if (side->status != SIDE_IN_GAME) side->pending_join = 1;
    return;
  }

  if (h->lobby == COUNTDOWN) {
    if (side->status == SIDE_EMPTY) side->pending_join = 1;
    else { side->join = *join; side->has_join = 1; }
    return;
  }

  if (side->status == SIDE_EMPTY) {
    side->status = SIDE_READY;
    side->first_join_time = now;
  }
  side->join = *join;
  side->has_join = 1;

  if (h->lobby == LOBBY_IDLE) {
    h->lobby = WAIT_PARTNER;
    h->deadline = now + h->timings[T_PARTNER_WAIT];
    h->credit_ok = 0;            /* game starting: clear the "ready" banner */
    h->new_record = 0;           /* clear last game's NEW RECORD flag */
  } else if (h->lobby == WAIT_PARTNER) {
    if (ready_count(h) == 2) {
      h->mode = 2;
      start_countdown(h, now);
    }
  }
}

static void on_skip_wait(hub_t *h, uint32_t now)
{
  h->skip_count++;
  if (h->lobby == WAIT_PARTNER && ready_count(h) == 1) {
    int ready = (h->sides[1].status == SIDE_READY) ? 1 : 2;
    if (now - h->sides[ready].first_join_time < 500)
      hub_warn(&h->stats, "SKIP_WAIT arrived < 0.5s after first JOIN");
    h->mode = 1;
    start_countdown(h, now);
  }
  if (h->skip_count > 1) {
    char buf[HUB_WARN_LEN];
    snprintf(buf, sizeof(buf), "SKIP_WAIT sent %dx this round", h->skip_count);
    hub_warn(&h->stats, buf);
  }
}

static void on_state(hub_t *h, const proto_state_t *st, uint32_t now)
{
  check_seq(&h->stats, st->seq);
  validate_state(st, &h->stats);
  int p = (st->player == 1 || st->player == 2) ? st->player : 1;
  hub_side_t *side = &h->sides[p];
  side->last_state = *st;
  side->has_state = 1;
  side->last_state_time = now;
  side->status = SIDE_IN_GAME;      /* any STATE -> that side is in-game */
  if (h->lobby == LOBBY_IDLE || h->lobby == WAIT_PARTNER
      || h->lobby == COUNTDOWN) {
    h->lobby = PLAYING;
  }
}

static void on_gameover(hub_t *h, uint32_t now)
{
  if (in_game_count(h) == 0) return;
  /* loser = in-game side with the most recent STATE. */
  int loser = 0;
  uint32_t best = 0;
  for (int p = 1; p <= 2; p++) {
    if (h->sides[p].status == SIDE_IN_GAME
        && (loser == 0 || h->sides[p].last_state_time >= best)) {
      loser = p;
      best = h->sides[p].last_state_time;
    }
  }
  if (h->mode == 1 || in_game_count(h) == 1) {
    send_result(h, loser, RESULT_SINGLE_DONE);
  } else {
    int winner = (loser == 1) ? 2 : 1;
    send_result(h, loser, RESULT_LOSE);
    send_result(h, winner, RESULT_WIN);
  }
  enter_result_show(h, now);
}

void hub_handle(hub_t *h, uint8_t type, const uint8_t *payload, uint8_t len,
                uint32_t now)
{
  /* CREDIT is display-only and not one of the 8 debug slots: handle it up
   * front so it never trips the unknown-type gate or the slot stats. */
  if (type == PROTO_TYPE_CREDIT) {
    if (len != PROTO_LEN_CREDIT) {
      char buf[HUB_WARN_LEN];
      snprintf(buf, sizeof(buf), "CREDIT LEN %d != %d", len, PROTO_LEN_CREDIT);
      hub_warn(&h->stats, buf);
      return;
    }
    proto_credit_t c;
    proto_parse_credit(payload, &c);
    h->credit_known   = 1;
    h->credit_balance = c.balance;
    h->credit_spent   = c.spent;
    h->stats.last_pkt_time = now;
    if (c.status == PROTO_CREDIT_OK) {
      h->credit_ok = 1;          /* paid: gate open, ready to start */
      h->credit_time = now;      /* start the timed BALANCE banner */
    } else {
      h->credit_ok   = 0;
      h->deny_time   = now;
      h->deny_reason = c.status;
    }
    return;
  }

  /* LEADERBOARD: one row per packet; store by rank for the result screen. */
  if (type == PROTO_TYPE_LEADERBOARD) {
    if (len == PROTO_LEN_LEADERBOARD) {
      proto_lb_t row;
      proto_parse_leaderboard(payload, &row);
      if (row.rank < HUB_LB_MAX) {
        memcpy(h->lb[row.rank].name, row.name, sizeof(h->lb[row.rank].name));
        h->lb[row.rank].points = row.points;
      }
      h->lb_count = (row.count < HUB_LB_MAX) ? row.count : HUB_LB_MAX;
      h->stats.last_pkt_time = now;
    }
    return;
  }

  /* NEWRECORD: member beat their high score this game. */
  if (type == PROTO_TYPE_NEWRECORD) {
    if (len == PROTO_LEN_NEWRECORD) {
      h->new_record    = 1;
      h->record_points = proto_parse_newrecord(payload);
      h->stats.last_pkt_time = now;
    }
    return;
  }

  if (type_index(type) < 0) {
    h->stats.unknown_types++;
    char buf[HUB_WARN_LEN];
    snprintf(buf, sizeof(buf), "unknown TYPE 0x%02X", type);
    hub_warn(&h->stats, buf);
    return;
  }
  hub_debug_note(&h->stats, type, now);

  int expected = proto_expected_len(type);
  if (expected >= 0 && len != expected) {
    char buf[HUB_WARN_LEN];
    snprintf(buf, sizeof(buf), "%s LEN %d != %d",
             proto_type_name(type), len, expected);
    hub_warn(&h->stats, buf);
    return;
  }

  switch (type) {
    case PROTO_TYPE_JOIN: {
      proto_join_t j;
      proto_parse_join(payload, &j);
      on_join(h, &j, now);
      break;
    }
    case PROTO_TYPE_SKIP_WAIT:
      on_skip_wait(h, now);
      break;
    case PROTO_TYPE_STATE: {
      proto_state_t st;
      proto_parse_state(payload, &st);
      on_state(h, &st, now);
      break;
    }
    case PROTO_TYPE_ATTACK: {
      uint8_t rows = proto_parse_attack(payload);
      snprintf(h->attack_msg, sizeof(h->attack_msg),
               "PLAYER ATTACKED %d ROWS", rows);
      h->attack_msg_time = now;
      break;
    }
    case PROTO_TYPE_GAMEOVER:
      on_gameover(h, now);
      break;
    default:
      break;
  }
}

/* --- time-based transitions ---------------------------------------------- */

static void drop_silent_ready(hub_t *h, uint32_t now)
{
  for (int p = 1; p <= 2; p++) {
    hub_side_t *s = &h->sides[p];
    if (s->status == SIDE_READY
        && now - s->last_join_time > h->timings[T_ALIVE_TIMEOUT]) {
      s->status = SIDE_EMPTY;
      char buf[HUB_WARN_LEN];
      snprintf(buf, sizeof(buf), "P%d dropped (no JOIN > %lums)",
               p, (unsigned long)h->timings[T_ALIVE_TIMEOUT]);
      hub_warn(&h->stats, buf);
    }
  }
}

static void check_playing_timeout(hub_t *h, uint32_t now)
{
  for (int p = 1; p <= 2; p++) {
    hub_side_t *s = &h->sides[p];
    if (s->status != SIDE_IN_GAME) continue;
    if (s->last_state_time
        && now - s->last_state_time > h->timings[T_ALIVE_TIMEOUT]) {
      char buf[HUB_WARN_LEN];
      snprintf(buf, sizeof(buf), "P%d lost (no STATE > %lums)",
               p, (unsigned long)h->timings[T_ALIVE_TIMEOUT]);
      hub_warn(&h->stats, buf);
      if (h->mode == 1 || in_game_count(h) == 1) {
        send_result(h, p, RESULT_SINGLE_DONE);
      } else {
        int winner = (p == 1) ? 2 : 1;
        send_result(h, p, RESULT_LOSE);
        send_result(h, winner, RESULT_WIN);
      }
      enter_result_show(h, now);
      return;
    }
  }
}

void hub_update(hub_t *h, uint32_t now)
{
  switch (h->lobby) {
    case WAIT_PARTNER:
      drop_silent_ready(h, now);
      if (ready_count(h) == 0) {
        reset_lobby(h);
      } else if (now >= h->deadline) {
        h->mode = 1;
        start_countdown(h, now);
      }
      break;
    case COUNTDOWN:
      if (ready_count(h) == 0) {
        reset_lobby(h);
      } else if (now >= h->deadline) {
        if (ready_count(h) == 1) h->mode = 1;
        send_start_all(h);
      }
      break;
    case PLAYING:
      check_playing_timeout(h, now);
      break;
    case RESULT_SHOW:
      if (now >= h->deadline) reset_lobby(h);
      break;
    default:
      break;
  }
}

/* --- test hooks ----------------------------------------------------------- */

void hub_sim_join(hub_t *h, int player, uint32_t now)
{
  proto_join_t j;
  memset(&j, 0, sizeof(j));
  j.player = (uint8_t)player;
  snprintf(j.name, sizeof(j.name), "P%d", player);
  on_join(h, &j, now);
}

void hub_sim_skip_wait(hub_t *h, uint32_t now)
{
  on_skip_wait(h, now);
}

void hub_force_result(hub_t *h, int result, uint32_t now)
{
  int any = 0;
  for (int p = 1; p <= 2; p++) {
    if (h->sides[p].status == SIDE_IN_GAME) {
      send_result(h, p, result);
      any = 1;
    }
  }
  if (any) enter_result_show(h, now);
}

void hub_force_reset(hub_t *h)
{
  reset_lobby(h);
}
