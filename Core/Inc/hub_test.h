/**
 * @file    hub_test.h
 * @brief   On-device tests + a fake board, ported from the Python tools.
 *
 *  - hub_selftest():     parser self-test (hub_sim.py --selftest): feeds a
 *                        messy byte stream (junk, bad CRC, oversized LEN,
 *                        split packets) through the parser and checks it.
 *  - hub_lobby_tests():  lobby state-machine tests (test_lobby.py).
 *  - fake_board_*:       drives the Hub with a synthetic game (fake_board.py /
 *                        --replay) so the VGA output can be seen with no board.
 */
#ifndef HUB_TEST_H
#define HUB_TEST_H

#include <stdint.h>
#include "hub.h"

/* Return 0 on PASS, 1 on FAIL. */
int hub_selftest(void);

/* Run lobby tests; passed/total counts filled in. Return 0 if all passed. */
int hub_lobby_tests(int *passed, int *total);

/* --- fake board (no hardware needed) ------------------------------------- */
typedef struct {
  uint8_t  grid[PROTO_BOARD_H][PROTO_BOARD_W];
  uint8_t  cur, nxt, hold;
  int      px, py;
  uint32_t score;
  uint16_t lines;
  uint8_t  level;
  uint8_t  seq;
  uint32_t rng;
  int      frame;
  int      started;
  uint32_t next_tick;     /* ms of next frame */
  uint32_t restart_at;    /* ms to restart after game over (0 = running) */
} fake_board_t;

void fake_board_init(fake_board_t *fb, uint32_t seed);
/* Advance the fake game and feed packets into the Hub when a frame is due. */
void fake_board_step(fake_board_t *fb, hub_t *h, uint32_t now);

#endif /* HUB_TEST_H */
