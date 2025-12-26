#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/boot.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/log.h"
#include "lib/string.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

// ================= Command functions =================
static void cmd_help(struct limine_framebuffer *fb) {
    print(fb, "Available commands:\n\n");
    print(fb, "  help       - Show this help message\n");
    print(fb, "  clear      - Clear the console\n");
    print(fb, "  echo [msg] - Print a message\n");
    print(fb, "  about      - Show information about KiwiOS\n");
    print(fb, "  crash [n]  - Trigger exception number n\n");
    print(fb, "  meminfo    - Show memory usage information\n");
    print(fb, "  memtest    - Run a memory test\n");
    print(fb, "  vmtest     - Run a VMM test\n");
    print(fb, "  heaptest   - Run a heap allocation test\n");
    print(fb, "  fbinfo     - Show framebuffer details\n");
    print(fb, "  scale [factor] - Set framebuffer scaling factor\n");
}

static void cmd_clear(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    console_clear();
}

static void cmd_echo(struct limine_framebuffer *fb, const char *args) {
    if (args && *args) {
        print(fb, args);
        print(fb, "\n");
    } else {
        print(fb, "\n");
    }
}

static void cmd_about(struct limine_framebuffer *fb) {
    print(fb, "KiwiOS v0.1\n");
    print(fb, "A simple operating system\n");
}

static void cmd_crash(struct limine_framebuffer *fb, const char *args) {
    int exception_num = 0; // default to divide by zero
    
    // Parse exception number from args
    if (args && *args) {
        exception_num = 0; // Reset to 0 when parsing
        // Simple string to int conversion
        while (*args >= '0' && *args <= '9') {
            exception_num = exception_num * 10 + (*args - '0');
            args++;
        }
    }
    
    print(fb, "Triggering exception ");
    print_hex(fb, exception_num);
    print(fb, "...\n");
    
    // Trigger the appropriate exception
    switch (exception_num) {
        case 0: { // Division by zero
            volatile int x = 1;
            volatile int y = 0;
            volatile int z = x / y;
            (void)z;
            break;
        }
        case 1: // Debug - use int instruction
            asm volatile ("int $1");
            break;
        case 2: // Non Maskable Interrupt
            asm volatile ("int $2");
            break;
        case 3: // Breakpoint
            asm volatile ("int3");
            break;
        case 4: // Overflow
            asm volatile ("int $4");
            break;
        case 5: { // Bound range exceeded
            // Note: BOUND instruction removed in x86-64, use int instead
            asm volatile ("int $5");
            break;
        }
        case 6: // Invalid opcode
            asm volatile ("ud2");
            break;
        case 7: { // Device not available (FPU)
            // Try to use FPU without enabling it first
            asm volatile (
                "clts\n"  // Clear task switched flag
                "fninit\n" // Init FPU
                "mov $0, %%rax\n"
                "mov %%rax, %%cr0\n" // Clear CR0.TS
                "fld1\n"  // This might not fault, so use int
                ::: "rax"
            );
            asm volatile ("int $7");
            break;
        }
        case 8: // Double fault - hard to trigger, use int
            asm volatile ("int $8");
            break;
        case 10: { // Invalid TSS
            asm volatile ("int $10");
            break;
        }
        case 11: { // Segment not present
            asm volatile ("int $11");
            break;
        }
        case 12: { // Stack segment fault
            asm volatile ("int $12");
            break;
        }
        case 13: { // General protection fault
            // Try to load invalid segment selector
            asm volatile ("mov $0xFFFF, %%ax; mov %%ax, %%ds" ::: "rax");
            break;
        }
        case 14: { // Page fault
            // Try to access unmapped memory
            volatile uint64_t *ptr = (uint64_t *)0xFFFFFFFF80000000ULL;
            volatile uint64_t val = *ptr;
            (void)val;
            break;
        }
        case 16: { // x87 FPU error
            asm volatile ("int $16");
            break;
        }
        case 17: { // Alignment check
            asm volatile ("int $17");
            break;
        }
        case 18: { // Machine check
            asm volatile ("int $18");
            break;
        }
        case 19: { // SIMD floating point exception
            asm volatile ("int $19");
            break;
        }
        case 20: { // Virtualization exception
            asm volatile ("int $20");
            break;
        }
        case 21: { // Control protection exception
            asm volatile ("int $21");
            break;
        }
        default:
            print(fb, "Exception number not supported or reserved.\n");
            print(fb, "Supported: 0-8, 10-14, 16-21\n");
            return;
    }
}

static void cmd_meminfo(struct limine_framebuffer *fb) {
    size_t total, used, free;
    pmm_get_stats(&total, &used, &free);
    
    print(fb, "Memory Information:\n");
    print(fb, "  Total pages: ");
    print_hex(fb, total);
    print(fb, " (");
    print_hex(fb, total * 4); // KB
    print(fb, " KB)\n");
    
    print(fb, "  Used pages:  ");
    print_hex(fb, used);
    print(fb, " (");
    print_hex(fb, used * 4);
    print(fb, " KB)\n");
    
    print(fb, "  Free pages:  ");
    print_hex(fb, free);
    print(fb, " (");
    print_hex(fb, free * 4);
    print(fb, " KB)\n");
}

static void cmd_memtest(struct limine_framebuffer *fb) {
    print(fb, "Testing memory allocation...\n");
    
    // Allocate a single page
    void* page1 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page1);
    print(fb, "\n");
    
    // Allocate another page
    void* page2 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page2);
    print(fb, "\n");
    
    // Allocate 10 contiguous pages
    void* pages = pmm_alloc_pages(10);
    if (pages) {
        print(fb, "Allocated 10 pages at: ");
        print_hex(fb, (uint64_t)pages);
        print(fb, "\n");
    } else {
        print(fb, "Failed to allocate 10 pages!\n");
    }
    
    // Free them
    print(fb, "Freeing allocations...\n");
    pmm_free(page1);
    pmm_free(page2);
    if (pages) pmm_free_pages(pages, 10);
    
    print(fb, "Memory test complete!\n");
}

static void cmd_vmtest(struct limine_framebuffer *fb) {
    print(fb, "Testing Virtual Memory Manager...\n");
    
    // Create a new page table
    page_table_t* test_pt = vmm_create_page_table();
    if (!test_pt) {
        print(fb, "Failed to create page table!\n");
        return;
    }
    print(fb, "Created page table at: ");
    print_hex(fb, (uint64_t)test_pt);
    print(fb, "\n");
    
    // Allocate a physical page
    uint64_t phys_page = (uint64_t)pmm_alloc();
    if (!phys_page) {
        print(fb, "Failed to allocate physical page!\n");
        return;
    }
    print(fb, "Allocated physical page: ");
    print_hex(fb, phys_page);
    print(fb, "\n");
    
    // Map it to a virtual address (let's use 0x400000, typical userspace)
    uint64_t virt_addr = 0x400000;
    bool mapped = vmm_map_page(test_pt, virt_addr, phys_page, PAGE_WRITE | PAGE_USER);
    if (!mapped) {
        print(fb, "Failed to map page!\n");
        pmm_free((void*)phys_page);
        return;
    }
    print(fb, "Mapped virtual ");
    print_hex(fb, virt_addr);
    print(fb, " -> physical ");
    print_hex(fb, phys_page);
    print(fb, "\n");
    
    // Verify the mapping
    uint64_t phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == phys_page) {
        print(fb, "Mapping verified successfully!\n");
    } else {
        print(fb, "Mapping verification FAILED!\n");
        print(fb, "Expected: ");
        print_hex(fb, phys_page);
        print(fb, "\nGot: ");
        print_hex(fb, phys_result);
        print(fb, "\n");
    }
    
    // Test unmapping
    vmm_unmap_page(test_pt, virt_addr);
    phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == 0) {
        print(fb, "Unmapping successful!\n");
    } else {
        print(fb, "Unmapping FAILED!\n");
    }
    
    // Clean up
    pmm_free((void*)phys_page);
    
    print(fb, "VMM test complete!\n");
}

static void cmd_heaptest(struct limine_framebuffer *fb) {
    print(fb, "Testing heap allocator...\n");
    
    // Test 1: Simple allocation
    char* str1 = (char*)kmalloc(32);
    if (str1) {
        print(fb, "Allocated 32 bytes at: ");
        print_hex(fb, (uint64_t)str1);
        print(fb, "\n");
    }
    
    // Test 2: Multiple allocations
    int* numbers = (int*)kmalloc(10 * sizeof(int));
    if (numbers) {
        print(fb, "Allocated array at: ");
        print_hex(fb, (uint64_t)numbers);
        print(fb, "\n");
    }
    
    // Test 3: Calloc (zeroed memory)
    uint64_t* zeroed = (uint64_t*)kcalloc(5, sizeof(uint64_t));
    if (zeroed) {
        print(fb, "Allocated zeroed memory at: ");
        print_hex(fb, (uint64_t)zeroed);
        print(fb, "\n");
    }
    
    // Show stats
    size_t allocated, free_mem, allocs;
    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "Heap stats:\n");
    print(fb, "  Allocated: ");
    print_hex(fb, allocated);
    print(fb, " bytes\n");
    print(fb, "  Free: ");
    print_hex(fb, free_mem);
    print(fb, " bytes\n");
    print(fb, "  Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");
    
    // Free everything
    kfree(str1);
    kfree(numbers);
    kfree(zeroed);
    
    print(fb, "Freed all allocations\n");
    
    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "After free - Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");
}

static void cmd_fbinfo(struct limine_framebuffer *fb_unused) {
    (void)fb_unused;

    struct limine_framebuffer_response *response = boot_framebuffer_response();
    if (!response || response->framebuffer_count == 0) {
        print(NULL, "No framebuffers from Limine.\n");
        return;
    }

    uint64_t count = response->framebuffer_count;
    print(NULL, "Framebuffers: ");
    print_u64(NULL, count);
    print(NULL, "\n");

    for (uint64_t i = 0; i < count; i++) {
        struct limine_framebuffer *fb = response->framebuffers[i];
        if (!fb) continue;

        print(NULL, "FB#"); print_u64(NULL, i); print(NULL, ": ");
        // WxH@bpp (pitch in bytes)
        print_u64(NULL, fb->width);  print(NULL, "x");
        print_u64(NULL, fb->height); print(NULL, "@");
        print_u64(NULL, fb->bpp);    print(NULL, "  pitch=");
        print_u64(NULL, fb->pitch);
        print(NULL, " bytes\n");

        // Memory model and RGB masks
        print(NULL, "  mem_model=");
        print_u64(NULL, fb->memory_model);
        print(NULL, "  R(");
        print_u64(NULL, fb->red_mask_size);   print(NULL, ":");
        print_u64(NULL, fb->red_mask_shift);  print(NULL, ")  G(");
        print_u64(NULL, fb->green_mask_size); print(NULL, ":");
        print_u64(NULL, fb->green_mask_shift);print(NULL, ")  B(");
        print_u64(NULL, fb->blue_mask_size);  print(NULL, ":");
        print_u64(NULL, fb->blue_mask_shift); print(NULL, ")\n");

        // EDID
        print(NULL, "  edid=");
        if (fb->edid && fb->edid_size) {
            print_u64(NULL, fb->edid_size); print(NULL, " bytes\n");
        } else {
            print(NULL, "none\n");
        }

        // Modes (if present)
        if (fb->mode_count && fb->modes) {
            uint64_t mcount = fb->mode_count;
            print(NULL, "  modes=");
            print_u64(NULL, mcount);
            print(NULL, " (showing up to 10)\n");

            uint64_t show = mcount > 10 ? 10 : mcount;
            for (uint64_t j = 0; j < show; j++) {
                struct limine_video_mode *m = fb->modes[j];
                if (!m) continue;

                print(NULL, "    [");
                print_u64(NULL, j);
                print(NULL, "] ");
                print_u64(NULL, m->width);  print(NULL, "x");
                print_u64(NULL, m->height); print(NULL, "@");
                print_u64(NULL, m->bpp);
                print(NULL, "  pitch=");
                print_u64(NULL, m->pitch);
                print(NULL, "  mem_model=");
                print_u64(NULL, m->memory_model);
                print(NULL, "\n");
            }
        } else {
            print(NULL, "  modes=none\n");
        }

        print(NULL, "\n");
    }
}

static void cmd_scale(struct limine_framebuffer *fb, const char *args) {
    (void)fb;
    // parse unsigned int from args; default 1 if missing/invalid
    uint32_t s = 0;
    if (args) {
        while (*args == ' ') args++;
        while (*args >= '0' && *args <= '9') {
            s = s * 10 + (uint32_t)(*args - '0');
            args++;
        }
    }
    if (s == 0) s = 1;
    if (s > 16) s = 9;

    console_set_scale(s);

    print(NULL, "scale set to ");
    // tiny itoa
    char buf[12]; int i = 0; uint32_t t = s; do { buf[i++] = '0' + (t % 10); t/=10; } while (t);
    while (i--) putc_fb(NULL, buf[i]);
    print(NULL, "x\n");
}

static void cmd_unknown(struct limine_framebuffer *fb, const char *cmd) {
    print(fb, "Unknown command: ");
    print(fb, cmd);
    print(fb, "\n");
    print(fb, "Type 'help' for available commands\n");
}

// ================= Command dispatch =================
typedef void (*cmd_func_t)(struct limine_framebuffer *fb);

typedef enum {
    COMMAND_NO_ARGS,
    COMMAND_TAKES_ARGS
} command_arity;

struct command {
    const char *name;
    cmd_func_t func;
    command_arity arity;
};

static struct command commands[] = {
    {"help", cmd_help, COMMAND_NO_ARGS},
    {"clear", cmd_clear, COMMAND_NO_ARGS},
    {"about", cmd_about, COMMAND_NO_ARGS},
    {"meminfo", cmd_meminfo, COMMAND_NO_ARGS},
    {"memtest", cmd_memtest, COMMAND_NO_ARGS},
    {"vmtest", cmd_vmtest, COMMAND_NO_ARGS},
    {"heaptest", cmd_heaptest, COMMAND_NO_ARGS},
    {"fbinfo", cmd_fbinfo, COMMAND_NO_ARGS},
    {NULL, NULL, COMMAND_NO_ARGS} // Sentinel
};

static void execute_command(struct limine_framebuffer *fb, char *input) {
    // Skip leading spaces
    while (*input == ' ') input++;
    
    // Empty command
    if (*input == '\0') return;
    
    // Find end of command word
    char *args = input;
    while (*args && *args != ' ') args++;
    
    // Split command and args
    if (*args) {
        *args = '\0'; // null terminate command
        args++; // point to arguments
        while (*args == ' ') args++; // skip spaces
    }
    
    if (strcmp(input, "echo") == 0) {
        cmd_echo(fb, args);
        log_info("shell", "echo command executed");
        return;
    }
    
    if (strcmp(input, "crash") == 0) {
        cmd_crash(fb, args);
        log_info("shell", "forced exception triggered");
        return;
    }

    if (strcmp(input, "scale") == 0) {
        cmd_scale(fb, args);
        log_info("shell", "console scale changed");
        return;
    }

    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(input, commands[i].name) == 0) {
            commands[i].func(fb);
            log_info("shell", "command executed");
            return;
        }
    }
    
    // Command not found
    cmd_unknown(fb, input);
}

// ================= Input handling =================
#define INPUT_BUFFER_SIZE 256

#define HISTORY_SIZE 32
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_count = 0;   // number of stored entries
static int history_cursor = -1; // -1 = live typing, 0 = newest history, ...
static char history_scratch[INPUT_BUFFER_SIZE];
static int history_scratch_len = 0;

static void history_record(const char *line) {
    if (!line || !*line) return;

    // Avoid duplicate consecutive entries
    if (history_count > 0) {
        const char *last = history[(history_count - 1) % HISTORY_SIZE];
        if (strncmp(last, line, INPUT_BUFFER_SIZE) == 0) return;
    }

    size_t len = strlen(line);
    if (len >= INPUT_BUFFER_SIZE) len = INPUT_BUFFER_SIZE - 1;

    int slot = history_count % HISTORY_SIZE;
    memcpy(history[slot], line, len);
    history[slot][len] = '\0';
    history_count++;
}

static void reset_history_navigation(void) {
    history_cursor = -1;
    history_scratch_len = 0;
}

static const char *history_fetch(int cursor_from_newest) {
    if (cursor_from_newest < 0) return NULL;
    if (cursor_from_newest >= history_count) return NULL;
    int logical = history_count - 1 - cursor_from_newest;
    return history[logical % HISTORY_SIZE];
}

static void replace_input_line(struct limine_framebuffer *fb,
                               char *buffer, int *pos,
                               const char *text) {
    while (*pos > 0) {
        putc_fb(fb, '\b');
        (*pos)--;
    }

    const char *p = text;
    while (*p && *pos < INPUT_BUFFER_SIZE - 1) {
        buffer[*pos] = *p;
        putc_fb(fb, *p);
        (*pos)++;
        p++;
    }
}

void shell_loop(struct limine_framebuffer *fb) {
    char input_buffer[INPUT_BUFFER_SIZE];
    int input_pos = 0;

    print(fb, "Welcome to kiwiOS!\n");
    print(fb, "Type 'help' for available commands\n\n");
    print(fb, "> ");
    log_info("shell", "interactive shell started");
    
    while (1) {
        char c = keyboard_getchar();
        if (c == KEY_ARROW_UP) {
            if (history_cursor == -1) {
                history_scratch_len = input_pos;
                if (history_scratch_len > INPUT_BUFFER_SIZE - 1) history_scratch_len = INPUT_BUFFER_SIZE - 1;
                memcpy(history_scratch, input_buffer, (size_t)history_scratch_len);
                history_scratch[history_scratch_len] = '\0';
            }

            if (history_cursor + 1 < history_count) {
                history_cursor++;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(fb, input_buffer, &input_pos, entry);
            }
            continue;
        }

        if (c == KEY_ARROW_DOWN) {
            if (history_cursor > 0) {
                history_cursor--;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(fb, input_buffer, &input_pos, entry);
            } else if (history_cursor == 0) {
                history_cursor = -1;
                replace_input_line(fb, input_buffer, &input_pos, history_scratch);
            }
            continue;
        }

        if (c == '\n') {
            // Execute command
            print(fb, "\n");
            input_buffer[input_pos] = '\0';
            
            if (input_pos > 0) {
                history_record(input_buffer);
                execute_command(fb, input_buffer);
            }
            
            // Reset for next command
            input_pos = 0;
            print(fb, "> ");
            reset_history_navigation();
        } else if (c == '\b') {
            // Backspace
            if (input_pos > 0) {
                input_pos--;
                putc_fb(fb, '\b');
            }
        } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
            // Add character to buffer
            input_buffer[input_pos++] = c;
            putc_fb(fb, c);
        }
    }
}
