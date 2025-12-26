#include "core/boot.h"

// ================= Limine boilerplate =================
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

bool boot_limine_supported(void) {
    return LIMINE_BASE_REVISION_SUPPORTED;
}

struct limine_framebuffer_response *boot_framebuffer_response(void) {
    return framebuffer_request.response;
}

struct limine_memmap_response *boot_memmap_response(void) {
    return memmap_request.response;
}

struct limine_hhdm_response *boot_hhdm_response(void) {
    return hhdm_request.response;
}

struct limine_module_response *boot_module_response(void) {
    return module_request.response;
}

void boot_hcf(void) {
    for (;;) asm volatile ("hlt");
}
