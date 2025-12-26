#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "arch/x86/io.h"
#include "core/console.h"
#include "core/keyboard.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64

// US QWERTY scancode set 1 -> ASCII mapping
static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, // Left ctrl
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, // Left shift
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 
    0, // Right shift
    '*',
    0, // Left alt
    ' '
};

static const char scancode_to_ascii_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, // Left ctrl
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, // Left shift
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 
    0, // Right shift
    '*',
    0, // Left alt
    ' '
};

static bool shift_pressed = false;
static bool ctrl_pressed  = false;
static bool e0_prefix     = false;
static int  pending_special = -1;

// Helpers for scancode press/release
static inline bool is_shift_press(uint8_t s)   { return s == 0x2A || s == 0x36; }
static inline bool is_shift_release(uint8_t s) { return s == 0xAA || s == 0xB6; }
static inline bool is_ctrl_press(uint8_t s)    { return s == 0x1D; }   // Left Ctrl
static inline bool is_ctrl_release(uint8_t s)  { return s == 0x9D; }   // Left Ctrl release

static inline char maybe_ctrlify(char c) {
    if (!ctrl_pressed) return c;
    // Only letters become control chars (A..Z -> 1..26)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return (char)(c & 0x1F);
    }
    return c;
}

static bool handle_extended_scancode(uint8_t scancode) {
    if (scancode & 0x80) return true; // ignore extended releases

    switch (scancode) {
        case 0x49: console_page_up();   return true;
        case 0x51: console_page_down(); return true;
        case 0x48: pending_special = KEY_ARROW_UP;   return true; // Up arrow
        case 0x50: pending_special = KEY_ARROW_DOWN; return true; // Down arrow
        default:   return false;
    }
}

char keyboard_getchar(void) {
    if (pending_special != -1) {
        char k = (char)pending_special;
        pending_special = -1;
        return k;
    }

    for (;;) {
        // Data available?
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & 0x01)) continue;

        uint8_t scancode = inb(PS2_DATA_PORT);

        if (scancode == 0xE0) { e0_prefix = true; continue; }
        if (e0_prefix) {
            bool consumed = handle_extended_scancode(scancode);
            e0_prefix = false;
            if (consumed) {
                if (pending_special != -1) {
                    char k = (char)pending_special;
                    pending_special = -1;
                    return k;
                }
                continue;
            }
        }

        // Track modifiers
        if (is_shift_press(scancode))   { shift_pressed = true;  continue; }
        if (is_shift_release(scancode)) { shift_pressed = false; continue; }
        if (is_ctrl_press(scancode))    { ctrl_pressed  = true;  continue; }
        if (is_ctrl_release(scancode))  { ctrl_pressed  = false; continue; }

        // Ignore generic key releases
        if (scancode & 0x80) continue;

        // Map to ASCII and optionally ctrlify
        if (scancode < sizeof(scancode_to_ascii)) {
            char c = shift_pressed ? scancode_to_ascii_shift[scancode]
                                   : scancode_to_ascii[scancode];
            if (c != 0) return maybe_ctrlify(c);
        }
    }
}

int keyboard_getchar_nonblocking(void) {
    if (pending_special != -1) {
        char k = (char)pending_special;
        pending_special = -1;
        return (int)k;
    }

    // Any data?
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & 0x01)) return -1;

    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == 0xE0) { e0_prefix = true; return -1; }
    if (e0_prefix) {
        bool consumed = handle_extended_scancode(scancode);
        e0_prefix = false;
        if (consumed) {
            if (pending_special != -1) {
                char k = (char)pending_special;
                pending_special = -1;
                return (int)k;
            }
            return -1;
        }
    }

    // Track modifiers
    if (is_shift_press(scancode))   { shift_pressed = true;  return -1; }
    if (is_shift_release(scancode)) { shift_pressed = false; return -1; }
    if (is_ctrl_press(scancode))    { ctrl_pressed  = true;  return -1; }
    if (is_ctrl_release(scancode))  { ctrl_pressed  = false; return -1; }

    // Ignore generic key releases
    if (scancode & 0x80) return -1;

    // Map to ASCII and optionally ctrlify
    if (scancode < sizeof(scancode_to_ascii)) {
        char c = shift_pressed ? scancode_to_ascii_shift[scancode]
                               : scancode_to_ascii[scancode];
        if (c != 0) return (int)maybe_ctrlify(c);
    }
    return -1;
}

void wait_for_key(void) {
    print(NULL, "[Press any key to continue]");
    keyboard_getchar();
    print(NULL, "\n");
}
