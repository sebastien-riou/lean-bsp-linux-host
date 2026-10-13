/* linux-host internals shared between main.c and the primitives; not part of the contract. */
#pragma once
#include <stdint.h>

void bsp_init(int argc, char *argv[]);
void bsp_start_tick(void);                       /* linux-host's counterpart of __enable_irq() */
void lean_bsp_default_app_tick(uint32_t tick_ms);
