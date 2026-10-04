// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include_next <mcuconf.h>
// Hardware PWM spreads the 15 shared columns over three CT16 timers (CT16B1 alone has only 12 channels).
// Column C14 can only route to CT16B0.3, so move the ChibiOS OS-tick free-running counter to the unused CT16B5.
#undef SN32_ST_USE_TIMER
#define SN32_ST_USE_TIMER SN32_TIM_CT16B5
#undef SN32_PWM_USE_CT16B0
#define SN32_PWM_USE_CT16B0 TRUE
#undef SN32_PWM_USE_CT16B1
#define SN32_PWM_USE_CT16B1 TRUE
#undef SN32_PWM_USE_CT16B2
#define SN32_PWM_USE_CT16B2 TRUE
#undef SN32_PWM_CT16B0_IRQ_PRIORITY
#define SN32_PWM_CT16B0_IRQ_PRIORITY 2   // more urgent than SPI0 (3): the RGB scan must preempt LCD transfers
#undef SN32_PWM_CT16B1_IRQ_PRIORITY
#define SN32_PWM_CT16B1_IRQ_PRIORITY 2   // more urgent than SPI0 (3): the RGB scan must preempt LCD transfers
#undef SN32_PWM_CT16B2_IRQ_PRIORITY
#define SN32_PWM_CT16B2_IRQ_PRIORITY 2   // more urgent than SPI0 (3): the RGB scan must preempt LCD transfers

// LCD on SPI0 (P3.0 SCK, P3.2 MOSI; CS P1.8 is a plain GPIO)
#undef SN32_SPI_USE_SPI0
#define SN32_SPI_USE_SPI0 TRUE

// Wireless-module link: UART2. Most urgent interrupt (a late byte is a lost byte); RGB scan (2) preempts SPI (3).
#undef SN32_SERIAL_USE_UART2
#define SN32_SERIAL_USE_UART2 TRUE
#undef SN32_SERIAL_UART2_PRIORITY
#define SN32_SERIAL_UART2_PRIORITY 1
