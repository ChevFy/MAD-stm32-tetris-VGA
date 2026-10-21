/**
 * @file    uart.h
 * @brief   USART3 driver for the Hub link (ST-LINK VCP on PD8/PD9, AF7).
 *
 * Register-level 8N1 UART with an interrupt-filled RX ring buffer. HAL's UART
 * driver sources are not part of this project, so the peripheral is driven
 * directly. `uart_send` matches hub_send_fn so it can be handed to hub_init().
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Configure USART3 at `baud` (8N1, no flow control) and enable RX IRQ. */
void uart_init(uint32_t baud);

/* Pop one received byte. Returns 1 and writes *out, or 0 if the ring is empty. */
int uart_rx_read(uint8_t *out);

/* Blocking transmit of a framed packet. Signature matches hub_send_fn. */
void uart_send(const uint8_t *pkt, int len, void *ctx);

#endif /* UART_H */
