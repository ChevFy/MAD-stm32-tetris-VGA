/**
 * @file  uart.c
 * @brief USART3 driver (register level) + RX ring buffer. See uart.h.
 */
#include "uart.h"
#include "main.h"   /* CMSIS device regs + HAL (NVIC, RCC clock query) */

#define RX_RING_SIZE 1024

static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head, rx_tail;

/* --- USART3 on PD8 (TX) / PD9 (RX), AF7 ---------------------------------- */
void uart_init(uint32_t baud)
{
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
  RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
  (void)RCC->APB1ENR;   /* ensure the clock is up before touching the regs */

  GPIOD->MODER   &= ~((3u << (8 * 2)) | (3u << (9 * 2)));
  GPIOD->MODER   |=  ((2u << (8 * 2)) | (2u << (9 * 2)));   /* AF mode */
  GPIOD->OSPEEDR |=  ((3u << (8 * 2)) | (3u << (9 * 2)));   /* very high */
  GPIOD->AFR[1]  &= ~((0xFu << (0 * 4)) | (0xFu << (1 * 4)));
  GPIOD->AFR[1]  |=  ((7u   << (0 * 4)) | (7u   << (1 * 4)));  /* AF7 */

  USART3->CR1 = 0;
  USART3->BRR = (HAL_RCC_GetPCLK1Freq() + baud / 2) / baud;
  USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
  USART3->CR1 |= USART_CR1_UE;

  HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART3_IRQn);
}

int uart_rx_read(uint8_t *out)
{
  if (rx_tail == rx_head) return 0;
  *out = rx_ring[rx_tail];
  rx_tail = (uint16_t)((rx_tail + 1) % RX_RING_SIZE);
  return 1;
}

void uart_send(const uint8_t *pkt, int len, void *ctx)
{
  (void)ctx;
  for (int i = 0; i < len; i++) {
    while (!(USART3->ISR & USART_ISR_TXE)) { }
    USART3->TDR = pkt[i];
  }
  while (!(USART3->ISR & USART_ISR_TC)) { }
}

/* RX interrupt: push bytes into the ring, clear overrun. */
void USART3_IRQHandler(void)
{
  uint32_t isr = USART3->ISR;
  if (isr & USART_ISR_ORE) USART3->ICR = USART_ICR_ORECF;
  if (isr & USART_ISR_RXNE) {
    uint8_t b = (uint8_t)USART3->RDR;
    uint16_t next = (uint16_t)((rx_head + 1) % RX_RING_SIZE);
    if (next != rx_tail) {
      rx_ring[rx_head] = b;
      rx_head = next;
    }
  }
}
