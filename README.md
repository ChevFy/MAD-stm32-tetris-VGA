# mad-stm32-tetris-VGA

Bare-metal STM32F767ZI firmware that acts as the **Tetris "Hub"**: it generates a
640×480@60 VGA signal with the LTDC peripheral, receives game packets from a
separate STM32 **Logic** board over UART, runs the lobby/match state machine, and
draws the whole game on the screen. It is a C port of the
`mad-python-uart-simulation` PC Hub simulator.

## Hardware

- **Board:** STM32F767ZI Nucleo-144, Cortex-M7 @ 216 MHz.
- **VGA out:** LTDC drives 24 digital RGB lines + HSYNC/VSYNC. A monitor expects
  analog 0–0.7 V per color, so an external **resistor DAC** sits between the MCU
  and the connector. Only the **top bit of each channel** is wired, giving an
  **8-color** palette (see `docs/vga-wiring.md`).
- **UART link:** **USART6 — PG14 (TX) / PG9 (RX)**, 921600 8N1, 3.3 V TTL, no flow
  control. These are morpho-header pins (not the ST-LINK USB), so you jumper them
  straight to the Logic board (see `docs/uart-wiring.md`).
- **LEDs/buttons:** PB7 LED lights on UART RX activity; B1 (PC13) toggles the
  on-screen debug panel.

## How it works

```
Logic board ──UART(USART6)──► USART6 RX IRQ ──► rx_ring[] ──► proto parser
                                                                   │
                                                    packet (type, payload)
                                                                   │
                                                                   ▼
                                              hub state machine (lobby/match)
                                                      │              │
                                               UART reply         render
                                            (START/RESULT/...)       │
                                                                     ▼
                                                   framebuffer ──LTDC──► VGA screen
```

- **Framebuffer:** `uint8_t framebuffer[640*480]`, L8 (8bpp indexed). A CLUT maps
  each index to one of 8 colors matching the resistor-ladder wiring.
- **Main loop (~30 fps):** drain the UART ring buffer → feed the protocol parser →
  update the hub state machine → render the frame.
- **No hardware needed to see it run:** if no real packets arrive, a built-in
  `fake_board` demo drives the hub with a synthetic game so the screen animates.

## Code layout

CubeMX-generated files live in `Core/Src` + `Core/Inc`. The hand-written Hub code
is split by feature, each a C port of a Python module:

| Folder | File | Ported from | Role |
|--------|------|-------------|------|
| `Core/Src` | `main.c` | — | entry point, clocks, LTDC setup, register-level USART6 driver + ISR, main loop |
| `Core/protocol` | `proto.c` | `protocol.py` | CRC-8/SMBUS, packet build/parse, byte-level parser |
| `Core/hub` | `hub.c` | `hub_sim.py` | lobby/match state machine + debug stats (time base = ms via `HAL_GetTick`) |
| `Core/display` | `vga_draw.c`, `font5x7.h` | `render.py` | `vga_render` — draws the game to the framebuffer |
| `Core/tests` | `hub_test.c` | self-tests + `fake_board.py` | `hub_selftest`, `hub_lobby_tests`, `fake_board_*` demo |

`mad-stm32-tetris-VGA.ioc` is the CubeMX source of truth for pinout/clocks.
UART is **not** in the `.ioc` — USART6 is configured at register level in `main.c`
(the project has no HAL UART driver).

## Build

Toolchain: GNU Tools for STM32 (arm-none-eabi-gcc 14.3.rel1), normally via
STM32CubeIDE. It is not on `PATH`, so for a command-line build:

```sh
export PATH="/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.macosaarch64_1.0.0.202602081740/tools/bin:$PATH"
make -C Debug all      # -> Debug/mad-stm32-tetris-VGA.elf
make -C Debug clean
```

Flash/debug with `mad-stm32-tetris-VGA.launch` (ST-LINK / OpenOCD) in CubeIDE.

The portable logic (no HAL deps) can be tested natively on a host:

```sh
cc -std=gnu11 -I Core/Inc runtests.c Core/protocol/proto.c Core/hub/hub.c Core/tests/hub_test.c
```

## Docs

Wiring and architecture notes (in Thai) are under `docs/`:
`system-architecture.md` (the full RFID→ESP32→Logic→Hub→screen chain),
`uart-wiring.md`, `uart-firmware-integration.md`, `vga-wiring.md`.
