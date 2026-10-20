/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ltdc.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "proto.h"
#include "hub.h"
#include "vga_draw.h"
#include "hub_test.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
#define FB_W 640
#define FB_H 480

uint8_t  framebuffer[FB_W * FB_H] __attribute__((aligned(32)));
uint32_t clut[256];

/* Hub + protocol state (ported from mad-python-uart-simulation). */
hub_t          g_hub;
proto_parser_t g_parser;
fake_board_t   g_fake;

/* UART3 RX ring buffer, filled by USART3_IRQHandler. */
#define RX_RING_SIZE 1024
volatile uint8_t  rx_ring[RX_RING_SIZE];
volatile uint16_t rx_head, rx_tail;

uint32_t g_last_real_pkt;   /* tick of last packet from a real board */
int      g_show_debug;      /* toggled by the user button (B1/PC13) */
int      g_test_passed, g_test_total, g_selftest_rc;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
void VGA_Pixel(int x, int y, uint8_t c);
void VGA_TestPattern(void);
void VGA_Start(void);
/* USER CODE BEGIN PFP */
static void USART3_Init(uint32_t baud);
static void uart_send(const uint8_t *pkt, int len, void *ctx);
static void on_packet(uint8_t type, const uint8_t *payload, uint8_t len,
                      void *ctx);
static void on_text(uint8_t b, void *ctx);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LTDC_Init();
  /* USER CODE BEGIN 2 */
  VGA_Start();
  LTDC->BCCR = 0x00000000;        /* พื้นหลังดำ */
  LTDC->SRCR = LTDC_SRCR_IMR;

  /* --- on-boot tests (ported from the Python tools) --- */
  g_selftest_rc = hub_selftest();
  hub_lobby_tests(&g_test_passed, &g_test_total);

  vga_clear(VGA_BLACK);
  vga_text_center("VGA HUB  SELF TEST", VGA_W / 2, 110, VGA_WHITE,
                  VGA_FONT_NORMAL);
  vga_text_center(g_selftest_rc == 0 ? "PARSER: PASS" : "PARSER: FAIL",
                  VGA_W / 2, 170,
                  g_selftest_rc == 0 ? VGA_GREEN : VGA_RED, VGA_FONT_NORMAL);
  {
    char b[32];
    snprintf(b, sizeof(b), "LOBBY: %d/%d", g_test_passed, g_test_total);
    vga_text_center(b, VGA_W / 2, 210,
                    (g_test_passed == g_test_total) ? VGA_GREEN : VGA_RED,
                    VGA_FONT_NORMAL);
  }
  HAL_Delay(2500);

  /* --- protocol + hub + UART --- */
  USART3_Init(921600);
  proto_parser_init(&g_parser);
  hub_init(&g_hub, uart_send, NULL);
  hub_seed_rng(&g_hub, HAL_GetTick() ^ 0xA5A5u);
  fake_board_init(&g_fake, 0xC0FFEEu);
  g_last_real_pkt = 0;
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef led = {0};
  led.Pin  = GPIO_PIN_7;              /* LD2 สีน้ำเงิน = liveness */
  led.Mode = GPIO_MODE_OUTPUT_PP;
  HAL_GPIO_Init(GPIOB, &led);

  /* B1 user button (PC13) toggles the debug panel. */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef btn = {0};
  btn.Pin  = GPIO_PIN_13;
  btn.Mode = GPIO_MODE_INPUT;
  btn.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &btn);

  uint32_t last_render = 0, last_blink = 0, cpsr_prev = 0;
  int btn_prev = 0;

  while (1)
  {
    uint32_t now = HAL_GetTick();

    /* 1) drain UART RX -> parser -> hub (real board path) */
    while (rx_tail != rx_head) {
      uint8_t b = rx_ring[rx_tail];
      rx_tail = (uint16_t)((rx_tail + 1) % RX_RING_SIZE);
      proto_parser_feed_byte(&g_parser, b, on_packet, on_text, &now);
    }

    /* 2) no real traffic -> run the fake board so the screen shows a demo */
    if (now - g_last_real_pkt > 2000)
      fake_board_step(&g_fake, &g_hub, now);

    /* 3) time-based lobby + debug window */
    hub_debug_tick_window(&g_hub.stats, now);
    hub_update(&g_hub, now);

    /* 4) debug-panel toggle on button press edge */
    int btn_now = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET;
    if (btn_now && !btn_prev) g_show_debug = !g_show_debug;
    btn_prev = btn_now;

    /* 5) render ~30 fps */
    if (now - last_render >= 33) {
      last_render = now;
      g_hub.stats.crc_errors    = g_parser.crc_errors;
      g_hub.stats.length_errors = g_parser.length_errors;
      vga_render(&g_hub, g_show_debug, now);
    }

    /* 6) LED liveness tied to LTDC scanning */
    if (now - last_blink >= 200) {
      last_blink = now;
      uint32_t c = LTDC->CPSR;
      if (c != cpsr_prev) HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_7);
      else                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
      cpsr_prev = c;
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void VGA_Start(void)
{
  /* CubeMX ตั้ง timing กับ GPIO ให้แล้ว เหลือชี้เฟรมบัฟเฟอร์ */
  LTDC_LayerCfgTypeDef L = {0};
  L.WindowX0 = 0;   L.WindowX1 = FB_W;
  L.WindowY0 = 0;   L.WindowY1 = FB_H;
  L.PixelFormat = LTDC_PIXEL_FORMAT_L8;
  L.Alpha  = 255;   L.Alpha0 = 0;
  L.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
  L.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
  L.FBStartAdress = (uint32_t)framebuffer;
  L.ImageWidth  = FB_W;
  L.ImageHeight = FB_H;
  HAL_LTDC_ConfigLayer(&hltdc, &L, 0);

  /* 8 สี เพราะต่อแค่บิตบนสุดของแต่ละช่อง */
  for (int i = 0; i < 256; i++) {
    clut[i] = ((i & 4) ? 0xFF0000 : 0)
            | ((i & 2) ? 0x00FF00 : 0)
            | ((i & 1) ? 0x0000FF : 0);
  }
  HAL_LTDC_ConfigCLUT(&hltdc, clut, 256, 0);
  HAL_LTDC_EnableCLUT(&hltdc, 0);
}

void VGA_TestPattern(void)
{
  for (int y = 0; y < FB_H; y++)
    for (int x = 0; x < FB_W; x++)
      framebuffer[y * FB_W + x] = x / 80;   /* 640/8 = 80 แถบ */
}

void VGA_Pixel(int x, int y, uint8_t c)
{
  if ((unsigned)x < FB_W && (unsigned)y < FB_H)
    framebuffer[y * FB_W + x] = c;
}

/* --- USART3 (ST-LINK VCP on PD8/PD9, AF7) at register level --------------- */
/* HAL UART driver sources are not in this project, so configure directly. */
static void USART3_Init(uint32_t baud)
{
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
  RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
  (void)RCC->APB1ENR;   /* ensure the clock is up before touching the regs */

  /* PD8 = TX, PD9 = RX -> alternate function 7 */
  GPIOD->MODER   &= ~((3u << (8 * 2)) | (3u << (9 * 2)));
  GPIOD->MODER   |=  ((2u << (8 * 2)) | (2u << (9 * 2)));
  GPIOD->OSPEEDR |=  ((3u << (8 * 2)) | (3u << (9 * 2)));
  GPIOD->AFR[1]  &= ~((0xFu << (0 * 4)) | (0xFu << (1 * 4)));  /* pins 8,9 */
  GPIOD->AFR[1]  |=  ((7u   << (0 * 4)) | (7u   << (1 * 4)));

  USART3->CR1 = 0;
  USART3->BRR = (HAL_RCC_GetPCLK1Freq() + baud / 2) / baud;
  USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
  USART3->CR1 |= USART_CR1_UE;

  HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART3_IRQn);
}

/* Hub send callback: blocking transmit of a framed packet. */
static void uart_send(const uint8_t *pkt, int len, void *ctx)
{
  (void)ctx;
  for (int i = 0; i < len; i++) {
    while (!(USART3->ISR & USART_ISR_TXE)) { }
    USART3->TDR = pkt[i];
  }
  while (!(USART3->ISR & USART_ISR_TC)) { }
}

/* Parser callbacks (real board path). */
static void on_packet(uint8_t type, const uint8_t *payload, uint8_t len,
                      void *ctx)
{
  uint32_t now = *(uint32_t *)ctx;
  g_last_real_pkt = now;
  hub_handle(&g_hub, type, payload, len, now);
}

static void on_text(uint8_t b, void *ctx)
{
  (void)b; (void)ctx;   /* board printf noise: ignored on the display */
}

/* USART3 RX interrupt: push bytes into the ring, clear overrun. */
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
/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
