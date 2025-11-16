#ifndef CORE_SYSCALL_H
#define CORE_SYSCALL_H

#include <stdint.h>

// Syscall numbers
#define SYS_EXIT     0
#define SYS_PRINT    1
#define SYS_GETPID   2
#define SYS_GETTIME  3
#define SYS_SLEEP    4
#define SYS_YIELD    5

// Memory
#define SYS_MMAP     20
#define SYS_MUNMAP   21
#define SYS_BRK      22

// Input
#define SYS_GETCHAR  30
#define SYS_GETCHAR_NONBLOCKING  32
#define SYS_POLL     31  // Check if input available

// Graphics
#define SYS_FB_INFO  40  // Get framebuffer info
#define SYS_FB_MAP   41  // Map framebuffer into process memory
#define SYS_FB_FLIP  42  // Swap buffers (for double buffering)

// Timing
#define SYS_GETTICKS 50  // Get system ticks since boot
#define SYS_SLEEP_MS 51  // Sleep for specified milliseconds
#define SYS_SLEEP_TICKS 52 // Sleep for specified ticks
#define SYS_GETTICKS_DELTA 53  // Get ticks since last call

// Utility
#define SYS_RAND     60  // Get random number
#define SYS_REBOOT    61 // Reboot the system
#define SYS_SHUTDOWN  62 // Shutdown the system

// File info structure
typedef struct {
    uint64_t size;
    uint32_t mode;
    uint32_t type;
} file_stat_t;

// Framebuffer info structure
typedef struct {
    uint64_t address;     // Physical address (kernel maps it for you)
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
} fb_info_t;

void syscall_init(void);
void syscall_on_process_exit(uint32_t pid);

#endif // CORE_SYSCALL_H
