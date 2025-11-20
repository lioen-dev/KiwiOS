#ifndef DRIVERS_TIMER_H
#define DRIVERS_TIMER_H

#include <stdint.h>

typedef void (*timer_tick_handler_t)(uint64_t* interrupt_rsp);

void timer_init(uint32_t frequency);
uint64_t timer_get_ticks(void);
void timer_handler(uint64_t* interrupt_rsp);
uint32_t timer_get_frequency(void);
void timer_register_tick_handler(timer_tick_handler_t handler);

#endif // DRIVERS_TIMER_H
