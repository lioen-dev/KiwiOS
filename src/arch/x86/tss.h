#ifndef ARCH_X86_TSS_H
#define ARCH_X86_TSS_H

#include <stdint.h>

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;      // Kernel stack pointer (ring 0)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

extern tss_t tss;

void tss_init(void);
void tss_set_kernel_stack(uint64_t stack);

#endif // ARCH_X86_TSS_H
