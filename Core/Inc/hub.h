/**
 * @file    hub.h
 * @brief   Lobby + game Hub state machine (C port of hub_sim.py::Hub).
 *
 * Reacts to incoming packets and ticks on time. Time is in milliseconds
 * (HAL_GetTick); the Python version used float seconds. Only one game runs at
 * a time; a side that presses during a game shows PLEASE WAIT and joins the
 * lobby once the game ends (item 9 = WAIT).
 */
#ifndef HUB_H
#define HUB_H

#include <stdint.h>
#include "proto.h"

/* Overall lobby state. */
typedef enum {
  LOBBY_IDLE = 0,
  WAIT_PARTNER,
  COUNTDOWN,
  PLAYING,
  RESULT_SHOW
} lobby_state_t;

/* Per-side state. */
typedef enum { SIDE_EMPTY = 0, SIDE_READY, SIDE_IN_GAME } side_status_t;

/* RESULT codes. */
#define RESULT_LOSE         0
#define RESULT_WIN          1
#define RESULT_SINGLE_DONE  2

/* Timing slots (ms). */
enum { T_PARTNER_WAIT = 0, T_COUNTDOWN, T_RESULT_SHOW, T_ALIVE_TIMEOUT, T_COUNT };

#define HUB_WARN_SLOTS  8
#define HUB_WARN_LEN    56

#define HUB_LB_MAX      8   /* leaderboard rows kept for display */

typedef struct {
  char     name[17];
  uint32_t points;
} hub_lb_entry_t;

/* Debug statistics for the Debug panel. */
typedef struct {
  uint32_t pkt_window[8];   /* per-type count this second */
  uint32_t pkt_per_sec[8];  /* per-type pps, latched each second */
  uint32_t crc_errors;
  uint32_t length_errors;
  uint32_t lost_packets;
  uint32_t unknown_types;
  uint32_t last_pkt_time;
  uint32_t window_start;
  int      last_seq;        /* -1 = none yet */
  char     warnings[HUB_WARN_SLOTS][HUB_WARN_LEN];
  uint8_t  warn_count;      /* how many slots filled (<= HUB_WARN_SLOTS) */
  uint8_t  warn_head;       /* next write slot (ring) */
} hub_debug_t;

typedef struct {
  side_status_t  status;
  proto_join_t   join;
  int            has_join;
  proto_state_t  last_state;
  int            has_state;
  uint32_t       last_join_time;
  uint32_t       first_join_time;
  uint32_t       last_state_time;
  int            result;         /* -1 = none */
  int            pending_join;
} hub_side_t;

/* send callback: frame already built, len bytes. */
typedef void (*hub_send_fn)(const uint8_t *pkt, int len, void *ctx);

typedef struct {
  lobby_state_t lobby;
  hub_side_t    sides[3];        /* [1], [2] used */
  int           mode;            /* 1 or 2 */
  uint32_t      seed;
  uint32_t      fixed_seed;
  int           has_fixed_seed;
  uint32_t      rng;             /* LCG state for random seeds */
  uint32_t      deadline;        /* ms deadline for current timed state */
  int           skip_count;
  char          attack_msg[32];
  uint32_t      attack_msg_time;
  int32_t       credit_balance;  /* last known account balance (from CREDIT) */
  int32_t       credit_spent;    /* amount deducted on the last paid tap */
  uint32_t      credit_time;     /* tick of last CREDIT_OK (for timed banner) */
  int           credit_known;    /* 1 once any CREDIT packet has been seen */
  uint32_t      deny_time;       /* tick of last denied tap; 0 = none */
  int           deny_reason;     /* PROTO_CREDIT_DENIED / _NO_CARD */
  int           credit_ok;       /* 1 = paid & ready to start (gate open) */
  hub_lb_entry_t lb[HUB_LB_MAX]; /* leaderboard rows (from LEADERBOARD pkts) */
  int           lb_count;        /* how many rows valid */
  int           new_record;      /* 1 = this game beat the member's best */
  uint32_t      record_points;   /* the new high score */
  uint32_t      timings[T_COUNT];
  hub_debug_t   stats;
  hub_send_fn   send;
  void         *send_ctx;
} hub_t;

void hub_init(hub_t *h, hub_send_fn send, void *send_ctx);
void hub_set_seed(hub_t *h, uint32_t seed);       /* fixed START seed */
void hub_seed_rng(hub_t *h, uint32_t entropy);    /* for random seeds */

/* Feed one received packet (known type, payload/len from the parser). */
void hub_handle(hub_t *h, uint8_t type, const uint8_t *payload, uint8_t len,
                uint32_t now);
/* Time-based lobby transitions; call every loop. */
void hub_update(hub_t *h, uint32_t now);

uint32_t hub_countdown_remaining(const hub_t *h, uint32_t now); /* ms */

/* Debug-panel helpers. */
uint8_t hub_debug_slot_type(int slot);
void hub_debug_note(hub_debug_t *s, uint8_t type, uint32_t now);
void hub_debug_tick_window(hub_debug_t *s, uint32_t now);
void hub_warn(hub_debug_t *s, const char *msg);

/* Keyboard-style test hooks (ported from hub_sim.py). */
void hub_sim_join(hub_t *h, int player, uint32_t now);
void hub_sim_skip_wait(hub_t *h, uint32_t now);
void hub_send_attack(hub_t *h, uint8_t rows);
void hub_force_result(hub_t *h, int result, uint32_t now);
void hub_force_reset(hub_t *h);

#endif /* HUB_H */
