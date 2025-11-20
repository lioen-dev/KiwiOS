#include "drivers/timer.h"
#include "arch/x86/io.h"
#include <stdint.h>
#include <stddef.h>

static volatile uint64_t ticks = 0;
static timer_tick_handler_t tick_handler = NULL;

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

static uint32_t timer_frequency = 0;

void timer_init(uint32_t frequency) {
    uint32_t divisor = 1193182 / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
    timer_frequency = frequency;    // store it
}

uint32_t timer_get_frequency(void) {
    return timer_frequency;
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

void timer_handler(uint64_t* interrupt_rsp) {
    ticks++;

    if (tick_handler && (ticks % 5 == 0)) {
        tick_handler(interrupt_rsp);
    }
}

void timer_register_tick_handler(timer_tick_handler_t handler) {
    tick_handler = handler;
}
