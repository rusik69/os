#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "io.h"

#define KB_DATA_PORT 0x60
#define KB_BUF_SIZE  256

static char kb_buffer[KB_BUF_SIZE];
static volatile int kb_head = 0;
static volatile int kb_tail = 0;
static volatile uint8_t kb_shift = 0;
static volatile uint8_t kb_ctrl = 0;
static volatile uint8_t kb_capslock = 0;

static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,  ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char scancode_to_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,  ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void kb_push(char c) {
    int next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) { kb_buffer[kb_head] = c; kb_head = next; }
}

static void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = inb(KB_DATA_PORT);

    switch (scancode) {
    case 0x2A: case 0x36: kb_shift = 1; pic_eoi(1); return;
    case 0xAA: case 0xB6: kb_shift = 0; pic_eoi(1); return;
    case 0x1D: kb_ctrl = 1; pic_eoi(1); return;
    case 0x9D: kb_ctrl = 0; pic_eoi(1); return;
    case 0x3A: kb_capslock = !kb_capslock; pic_eoi(1); return;
    }

    if (scancode & 0x80) { pic_eoi(1); return; }

    if (scancode == 0x48) { kb_push(KEY_UP); pic_eoi(1); return; }
    if (scancode == 0x50) { kb_push(KEY_DOWN); pic_eoi(1); return; }
    if (scancode == 0x4B) { kb_push(KEY_LEFT); pic_eoi(1); return; }
    if (scancode == 0x4D) { kb_push(KEY_RIGHT); pic_eoi(1); return; }

    char base = scancode_to_ascii[scancode];
    int use_shift = kb_shift;
    if (base >= 'a' && base <= 'z') use_shift = kb_shift ^ kb_capslock;
    char c = use_shift ? scancode_to_ascii_shift[scancode] : base;

    if (kb_ctrl && c >= 'a' && c <= 'z') c = c - 'a' + 1;
    if (c) kb_push(c);
    pic_eoi(1);
}

void keyboard_init(void) {
    idt_register_handler(33, keyboard_handler);
    pic_unmask(1);
}

int keyboard_has_input(void) {
    return kb_head != kb_tail;
}

#define SERIAL_DATA  0x3F8
#define SERIAL_LSR   (0x3F8 + 5)

static int serial_has_input(void) {
    return inb(SERIAL_LSR) & 1;
}

static uint8_t serial_esc_state = 0;

char keyboard_getchar(void) {
    while (1) {
        if (keyboard_has_input()) {
            char c = kb_buffer[kb_tail];
            kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
            return c;
        }
        if (serial_has_input()) {
            char c = inb(SERIAL_DATA);
            if (serial_esc_state == 0 && c == 27) { serial_esc_state = 1; continue; }
            if (serial_esc_state == 1) {
                if (c == '[') { serial_esc_state = 2; continue; }
                serial_esc_state = 0;
            }
            if (serial_esc_state == 2) {
                serial_esc_state = 0;
                if (c == 'A') return KEY_UP;
                if (c == 'B') return KEY_DOWN;
                if (c == 'D') return KEY_LEFT;
                if (c == 'C') return KEY_RIGHT;
                continue;
            }
            if (c == '\r') c = '\n';
            if (c == 127) c = '\b';
            return c;
        }
        __asm__ volatile("hlt");
    }
}
