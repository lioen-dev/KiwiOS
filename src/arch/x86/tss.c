#include "arch/x86/tss.h"
#include <stddef.h>
#include <stdint.h>
#include "lib/string.h"

tss_t tss;

void tss_init(void) {
    memset(&tss, 0, sizeof(tss_t));
    tss.iopb_offset = sizeof(tss_t);
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}