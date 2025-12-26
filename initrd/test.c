#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Minimal syscall layer ----------------------------------------------------
#define SYS_EXIT      0
#define SYS_PRINT     1
#define SYS_FB_INFO   40
#define SYS_FB_MAP    41
#define SYS_FB_FLIP   42
#define SYS_SLEEP_MS  51
#define SYS_RAND      60
#define SYS_GETCHAR_NONBLOCKING 32

typedef struct {
    uint64_t address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
} fb_info_t;

static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "rbx", "rcx", "rdx", "memory");
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t arg1) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1)
        : "rcx", "rdx", "memory");
    return ret;
}

static inline uint64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2)
        : "rdx", "memory");
    return ret;
}

static inline void sys_exit(uint64_t code) { syscall1(SYS_EXIT, code); }
static inline uint64_t sys_print(const char* s) { return syscall1(SYS_PRINT, (uint64_t)s); }
static inline uint64_t sys_fb_info(fb_info_t* info) { return syscall1(SYS_FB_INFO, (uint64_t)info); }
static inline uint64_t sys_fb_map(void) { return syscall1(SYS_FB_MAP, 0); }
static inline uint64_t sys_fb_flip(void) { return syscall0(SYS_FB_FLIP); }
static inline void sys_sleep_ms(uint64_t ms) { syscall1(SYS_SLEEP_MS, ms); }
static inline uint32_t sys_rand(void) { return (uint32_t)syscall0(SYS_RAND); }
static inline int sys_getchar_nonblocking(void) { return (int)syscall0(SYS_GETCHAR_NONBLOCKING); }

// Utilities ----------------------------------------------------------------
static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t next_palette_color(uint32_t seed) {
    // Simple cycling palette using random seeds for variety.
    static const uint32_t palette[] = {
        0xFFFF5555u, 0xFF55FF55u, 0xFF5555FFu, 0xFFFFFF55u,
        0xFFFF55FFu, 0xFF55FFFFu, 0xFFFFFFFFu, 0xFF00AAFFu
    };
    return palette[seed % (sizeof(palette) / sizeof(palette[0]))];
}

static void fill_rect(uint32_t* fb, uint64_t pitch_bytes, uint32_t color,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t fb_width, uint32_t fb_height) {
    if (!fb) return;
    uint32_t pitch_pixels = (uint32_t)(pitch_bytes / 4);

    for (uint32_t row = 0; row < h; ++row) {
        uint32_t py = y + row;
        if (py >= fb_height) break;
        uint32_t* p = fb + ((uint64_t)py * pitch_pixels);
        for (uint32_t col = 0; col < w; ++col) {
            uint32_t px = x + col;
            if (px >= fb_width) break;
            p[px] = color;
        }
    }
}

static void clear_screen(uint32_t* fb, uint64_t pitch_bytes, uint32_t color,
                         uint32_t fb_width, uint32_t fb_height) {
    uint32_t pitch_pixels = (uint32_t)(pitch_bytes / 4);
    for (uint32_t y = 0; y < fb_height; ++y) {
        uint32_t* row = fb + ((uint64_t)y * pitch_pixels);
        for (uint32_t x = 0; x < fb_width; ++x) {
            row[x] = color;
        }
    }
}

// Entry point --------------------------------------------------------------
static int main(uint64_t argc, char** argv) {
    (void)argc; (void)argv;

    fb_info_t info;
    if (sys_fb_info(&info) != 0 || info.bpp != 32) {
        sys_print("fb_info failed or unsupported format\n");
        return 1;
    }

    uint64_t fb_map_ret = sys_fb_map();
    if (fb_map_ret == 0 || fb_map_ret == (uint64_t)-1) {
        sys_print("fb_map failed\n");
        return 1;
    }
    uint32_t* fb = (uint32_t*)fb_map_ret;

    uint32_t bg = make_color(0, 0, 0);
    clear_screen(fb, info.pitch, bg, (uint32_t)info.width, (uint32_t)info.height);

    uint32_t square = 48;
    uint32_t x = 10;
    uint32_t y = 10;
    int dx = 4;
    int dy = 3;
    uint32_t color = next_palette_color(sys_rand());

    while (true) {
        fill_rect(fb, info.pitch, color, x, y, square, square,
                  (uint32_t)info.width, (uint32_t)info.height);

        // Bounce and change color when we hit an edge.
        if ((int)(x + square) >= (int)info.width || (int)x <= 0) {
            dx = -dx;
            color = next_palette_color(sys_rand());
        }
        if ((int)(y + square) >= (int)info.height || (int)y <= 0) {
            dy = -dy;
            color = next_palette_color(sys_rand());
        }

        x = (uint32_t)((int)x + dx);
        y = (uint32_t)((int)y + dy);

        sys_fb_flip();
        sys_sleep_ms(8); // ~60 FPS

        // Check for Shift+Q to exit
        int ch = sys_getchar_nonblocking();
        if (ch == 'Q') {
            break;
        }
    }

    return 0;
}

__attribute__((noreturn)) void _start(uint64_t argc, char** argv) {
    int rc = main(argc, argv);
    sys_exit((uint64_t)rc);
    for (;;) { }
}