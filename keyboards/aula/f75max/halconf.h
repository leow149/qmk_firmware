// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// RGB matrix: hardware PWM across the SN32 CT16B0/B1/B2 timers (see drivers/led/sn32f2xx.c, hardware_pwm.diff).
#define HAL_USE_PWM TRUE
// LCD (GC9107) on SPI0
#define HAL_USE_SPI TRUE
// Wireless-module link on UART2
#define HAL_USE_SERIAL TRUE
#include_next <halconf.h>
