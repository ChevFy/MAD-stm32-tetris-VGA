/**
 * @file    vga_draw.h
 * @brief   VGA framebuffer rendering — C port of tools/render.py.
 *
 * Draws the lobby screens, in-game board(s) and debug panel into the L8
 * framebuffer that the LTDC scans out. Only 8 colours exist (resistor DAC on
 * the top bit of each channel), so the Python RGB palette is approximated.
 */
#ifndef VGA_DRAW_H
#define VGA_DRAW_H

#include <stdint.h>
#include "hub.h"

#define VGA_W  640
#define VGA_H  480

/* 8-colour palette = CLUT index (bit2=R, bit1=G, bit0=B). */
#define VGA_BLACK    0
#define VGA_BLUE     1
#define VGA_GREEN    2
#define VGA_CYAN     3
#define VGA_RED      4
#define VGA_MAGENTA  5
#define VGA_YELLOW   6
#define VGA_WHITE    7

/* render.py named colours, approximated to the 8-colour palette. */
#define VGA_GREY  VGA_BLUE   /* dim lines/labels (no true grey available) */
#define VGA_DARK  VGA_BLACK  /* background */

/* Text scales (pygame used 13/16/40/90 px fonts). */
#define VGA_FONT_SMALL   2
#define VGA_FONT_NORMAL  2
#define VGA_FONT_BIG     5
#define VGA_FONT_HUGE    11

extern uint8_t framebuffer[VGA_W * VGA_H];

/* primitives */
void vga_clear(uint8_t color);
void vga_fill_rect(int x, int y, int w, int h, uint8_t color);
void vga_rect(int x, int y, int w, int h, uint8_t color);   /* 1px outline */
void vga_hline(int x, int y, int w, uint8_t color);
void vga_vline(int x, int y, int h, uint8_t color);
int  vga_text_width(const char *s, int scale);
void vga_text(const char *s, int x, int y, uint8_t color, int scale);
void vga_text_center(const char *s, int cx, int y, uint8_t color, int scale);

/* top-level: draw the whole screen for the current hub state */
void vga_render(const hub_t *h, int show_debug, uint32_t now);

#endif /* VGA_DRAW_H */
