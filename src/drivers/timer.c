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
    // Clamp to a safe, non-zero value to avoid divide-by-zero
    if (frequency == 0) frequency = 18;

    uint32_t divisor = 1193182 / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    timer_frequency = frequency;    // store it
    ticks = 0;
}

uint32_t timer_get_frequency(void) {
    return timer_frequency;
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

uint64_t* timer_handler(uint64_t* interrupt_rsp) {
    ticks++;

    if (tick_handler) {
        return tick_handler(interrupt_rsp);
    }

    return interrupt_rsp;
}

void timer_register_tick_handler(timer_tick_handler_t handler) {
    tick_handler = handler;
}

void timer_sleep_ticks(uint64_t delta_ticks) {
    if (timer_frequency == 0 || delta_ticks == 0) return;

    uint64_t target = ticks + delta_ticks;
    while (ticks < target) {
        asm volatile ("hlt");
    }
}

void timer_sleep_ms(uint64_t ms) {
    if (timer_frequency == 0 || ms == 0) return;

    uint64_t desired_ticks = (ms * (uint64_t)timer_frequency + 999) / 1000; // ceil
    timer_sleep_ticks(desired_ticks);
}

uint64_t timer_ms_since_boot(void) {
    if (timer_frequency == 0) return 0;
    return (ticks * 1000ull) / (uint64_t)timer_frequency;
}
