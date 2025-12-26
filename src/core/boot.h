#ifndef CORE_BOOT_H
#define CORE_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include "limine.h"

bool boot_limine_supported(void);
void boot_hcf(void) __attribute__((noreturn));

struct limine_framebuffer_response *boot_framebuffer_response(void);
struct limine_memmap_response *boot_memmap_response(void);
struct limine_hhdm_response *boot_hhdm_response(void);
struct limine_module_response *boot_module_response(void);

#endif // CORE_BOOT_H
