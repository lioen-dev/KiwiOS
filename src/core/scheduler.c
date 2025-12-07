#include "core/scheduler.h"
#include "drivers/timer.h"
#include "memory/heap.h"
#include "lib/string.h"

// Print helpers from the console renderer in main.c
struct limine_framebuffer;
extern void print(struct limine_framebuffer *fb, const char *s);
extern struct limine_framebuffer *fb0(void);
extern void print_u64(struct limine_framebuffer *fb, uint64_t v);

typedef struct task {
    const char *name;
    uint64_t *stack_top; // Points to saved rax in the interrupt-style frame
    uint8_t *stack_base;
    size_t stack_size;
    void (*entry)(void *);
    void *arg;
    int runnable;
} task_t;

#define MAX_TASKS 8
#define DEFAULT_STACK_SIZE (16 * 1024)

// Layout that matches irq0_handler's push order.
enum context_slots {
    CTX_RAX = 0,
    CTX_RBX,
    CTX_RCX,
    CTX_RDX,
    CTX_RSI,
    CTX_RDI,
    CTX_RBP,
    CTX_R8,
    CTX_R9,
    CTX_R10,
    CTX_R11,
    CTX_R12,
    CTX_R13,
    CTX_R14,
    CTX_R15,
    CTX_RIP,
    CTX_CS,
    CTX_RFLAGS,
    CTX_SLOTS
};

static task_t g_tasks[MAX_TASKS];
static size_t g_task_count = 0;
static size_t g_current = 0;
static int g_scheduler_ready = 0;

static void task_trampoline(task_t *task) {
    if (!task || !task->entry) return;

    asm volatile ("sti");
    task->entry(task->arg);

    // If the task function returns, park the CPU in a halt loop.
    for (;;) asm volatile ("hlt");
}

static uint64_t *build_initial_frame(task_t *task) {
    size_t slots = CTX_SLOTS;
    uint64_t *frame = (uint64_t *)(task->stack_base + task->stack_size);
    frame -= slots;
    memset(frame, 0, slots * sizeof(uint64_t));

    frame[CTX_RIP] = (uint64_t)task_trampoline;
    frame[CTX_CS] = 0x08; // Kernel code segment from GDT
    frame[CTX_RFLAGS] = 0x202; // IF=1
    frame[CTX_RBP] = (uint64_t)(task->stack_base + task->stack_size);
    frame[CTX_RDI] = (uint64_t)task;

    return frame;
}

static void add_boot_task(void) {
    if (g_task_count >= MAX_TASKS) return;

    g_tasks[0].name = "boot";
    g_tasks[0].stack_top = NULL; // filled in on first tick
    g_tasks[0].stack_base = NULL;
    g_tasks[0].stack_size = 0;
    g_tasks[0].entry = NULL;
    g_tasks[0].arg = NULL;
    g_tasks[0].runnable = 1;
    g_task_count = 1;
}

void scheduler_init(void) {
    add_boot_task();
    timer_register_tick_handler(scheduler_tick);
    g_scheduler_ready = 1;
}

int scheduler_create_kernel_task(const char *name, void (*entry)(void *), void *arg, size_t stack_size) {
    if (!entry) return -1;
    if (g_task_count >= MAX_TASKS) return -1;

    task_t *task = &g_tasks[g_task_count];
    task->name = name ? name : "anon";
    task->stack_size = (stack_size == 0) ? DEFAULT_STACK_SIZE : stack_size;
    task->stack_base = (uint8_t *)kmalloc(task->stack_size);
    if (!task->stack_base) return -1;

    task->entry = entry;
    task->arg = arg;
    task->runnable = 1;
    task->stack_top = build_initial_frame(task);

    g_task_count++;
    return 0;
}

uint64_t *scheduler_tick(uint64_t *interrupt_rsp) {
    if (!g_scheduler_ready || g_task_count == 0) return interrupt_rsp;

    task_t *current = &g_tasks[g_current];
    current->stack_top = interrupt_rsp;

    if (g_task_count == 1) return interrupt_rsp;

    size_t next = g_current;
    for (size_t i = 0; i < g_task_count; i++) {
        next = (next + 1) % g_task_count;
        if (g_tasks[next].runnable) break;
    }

    if (next == g_current || !g_tasks[next].runnable) {
        return interrupt_rsp;
    }

    g_current = next;
    return g_tasks[g_current].stack_top;
}

size_t scheduler_task_count(void) {
    return g_task_count;
}

// ---------------- Diagnostics helpers ----------------
static void print_task_line(task_t *task, size_t idx) {
    print(NULL, "  [");
    print_u64(NULL, (uint64_t)idx);
    print(NULL, "] ");
    print(NULL, task->name ? task->name : "(unnamed)");
    print(NULL, task->runnable ? " runnable" : " stopped");
    print(NULL, "\n");
}

void scheduler_dump_tasks(void) {
    print(NULL, "[sched] tasks: ");
    print_u64(NULL, (uint64_t)g_task_count);
    print(NULL, " total\n");
    for (size_t i = 0; i < g_task_count; i++) {
        print_task_line(&g_tasks[i], i);
    }
}
