#ifndef CORE_KEYBOARD_H
#define CORE_KEYBOARD_H

char keyboard_getchar(void);
int keyboard_getchar_nonblocking(void);
void wait_for_key(void);

enum {
    KEY_ARROW_UP   = -16,
    KEY_ARROW_DOWN = -17,
};

#endif // CORE_KEYBOARD_H
