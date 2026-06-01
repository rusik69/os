#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "io.h"
#include "scheduler.h"
#include "printf.h"
#include "ps2.h"

#define KB_DATA_PORT 0x60
#define KB_CMD_PORT  0x64
#define KB_BUF_SIZE  256

static volatile char kb_buffer[KB_BUF_SIZE];
static volatile int kb_head = 0;
static volatile int kb_tail = 0;
static volatile uint8_t kb_shift = 0;
static volatile uint8_t kb_ctrl = 0;
static volatile uint8_t kb_capslock = 0;
static volatile uint8_t key_down[256];
static volatile uint8_t kb_extend = 0;

/* Volume of the keyboard click (not applicable to PS/2, but kept for compat) */
static uint8_t kb_layout = KB_LAYOUT_US;
static volatile uint8_t kb_led_state = 0;

/* ── Scancode tables ──────────────────────────────────────────────── */
/* US Layout */
static const char us_scancode[128] = {
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

static const char us_shift[128] = {
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

/* UK Layout */
static const char uk_scancode[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '#','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,  ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char uk_shift[128] = {
    0,  27, '!','"','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','@','~',
    0,  '~','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,  ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ── Internal helpers ─────────────────────────────────────────────── */

static void kb_push(char c) {
    int next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) { kb_buffer[kb_head] = c; kb_head = next; }
}

static void key_set_down(uint8_t sc, int down) {
    if (sc < 128)
        key_down[sc] = down ? 1 : 0;
}

/* Send a command byte to the keyboard controller */
static void kb_send_cmd(uint8_t cmd) {
    /* Wait for KB controller to be ready */
    uint32_t timeout = 100000;
    while (timeout-- && (inb(KB_CMD_PORT) & 0x02))
        __asm__ volatile("pause");
    outb(KB_CMD_PORT, cmd);
}

/* Send data to the keyboard */
static void kb_write_data(uint8_t data) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(KB_CMD_PORT) & 0x02))
        __asm__ volatile("pause");
    outb(KB_DATA_PORT, data);
}

/* Wait for ACK from keyboard */
static int kb_wait_ack(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(KB_CMD_PORT) & 0x01) {
            uint8_t ack = inb(KB_DATA_PORT);
            if (ack == 0xFA) return 0;  /* ACK */
            if (ack == 0xFE) return -2; /* Resend */
            return -1;                   /* Unexpected */
        }
        __asm__ volatile("pause");
    }
    return -3; /* Timeout */
}

/* ── Keyboard LED control ─────────────────────────────────────────── */

int keyboard_set_leds(uint8_t leds) {
    /* Cap/Nums/Scroll lock LEDs are controlled via keyboard command 0xED */
    /* Only send if keyboard is active (data port available) */
    kb_write_data(0xED);
    if (kb_wait_ack() < 0) return -1;

    kb_write_data(leds & 0x07);
    if (kb_wait_ack() < 0) return -1;

    kb_led_state = leds & 0x07;
    return 0;
}

uint8_t keyboard_get_leds(void) {
    return kb_led_state;
}

/* ── Layout support ───────────────────────────────────────────────── */

int keyboard_set_layout(int layout) {
    int old = kb_layout;
    if (layout == KB_LAYOUT_US || layout == KB_LAYOUT_UK) {
        kb_layout = (uint8_t)layout;
    }
    return old;
}

int keyboard_get_layout(void) {
    return kb_layout;
}

/* ── IRQ handler ──────────────────────────────────────────────────── */

static void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = ps2_read_data();

    irq_ack(1);

    if (scancode == 0xE0) {
        kb_extend = 1;
        return;
    }

    if (kb_extend) {
        kb_extend = 0;
        if (scancode & 0x80)
            key_set_down(scancode & 0x7F, 0);
        else
            key_set_down(scancode, 1);
        return;
    }

    switch (scancode) {
    case 0x2A: case 0x36: kb_shift = 1; return;
    case 0xAA: case 0xB6: kb_shift = 0; return;
    case 0x1D: kb_ctrl = 1; return;
    case 0x9D: kb_ctrl = 0; return;
    case 0x3A: kb_capslock = !kb_capslock; return;
    }

    if (scancode & 0x80) {
        key_set_down(scancode & 0x7F, 0);
        return;
    }

    key_set_down(scancode, 1);

    if (scancode == 0x48) { kb_push(KEY_UP); return; }
    if (scancode == 0x50) { kb_push(KEY_DOWN); return; }
    if (scancode == 0x4B) { kb_push(KEY_LEFT); return; }
    if (scancode == 0x4D) { kb_push(KEY_RIGHT); return; }

    if (scancode >= 128) return;

    /* Use the selected layout */
    const char *base_table = (kb_layout == KB_LAYOUT_UK) ? uk_scancode : us_scancode;
    const char *shift_table = (kb_layout == KB_LAYOUT_UK) ? uk_shift : us_shift;

    char base = base_table[scancode];
    int use_shift = kb_shift;
    if (base >= 'a' && base <= 'z') use_shift = kb_shift ^ kb_capslock;
    char c = use_shift ? shift_table[scancode] : base;

    if (kb_ctrl && c >= 'a' && c <= 'z') c = c - 'a' + 1;
    if (c) kb_push(c);
}

/* ── Initialisation ───────────────────────────────────────────────── */

void keyboard_init(void) {
    /* Initialise PS/2 controller first */
    ps2_controller_init();
    
    idt_register_handler(33, keyboard_handler);
    if (apic_is_init_complete()) {
        ioapic_redirect_extint(1);
        ioapic_unmask_irq(1);
    }
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

int keyboard_is_down(char c) {
    uint8_t sc = 0;
    switch (c) {
    case 'w': case 'W': sc = 0x11; break;
    case 'a': case 'A': sc = 0x1E; break;
    case 's': case 'S': sc = 0x1F; break;
    case 'd': case 'D': sc = 0x20; break;
    case 'q': case 'Q': sc = 0x10; break;
    default: return 0;
    }
    return key_down[sc] != 0;
}

int keyboard_escape_down(void) {
    return key_down[0x01] != 0;
}

void keyboard_reset_state(void) {
    for (int i = 0; i < 256; i++)
        key_down[i] = 0;
    kb_head = 0;
    kb_tail = 0;
    kb_extend = 0;
}

char keyboard_getchar(void) {
    while (1) {
        __asm__ volatile("cli");
        if (keyboard_has_input()) {
            char c = kb_buffer[kb_tail];
            kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
            __asm__ volatile("sti");
            return c;
        }
        __asm__ volatile("sti");
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
        __asm__ volatile("pause");
        scheduler_yield();
    }
}
