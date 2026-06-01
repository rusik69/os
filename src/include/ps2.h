#ifndef PS2_H
#define PS2_H

#include "types.h"

/* PS/2 controller ports */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

/* PS/2 controller commands */
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_DISABLE_P2   0xA7
#define PS2_CMD_ENABLE_P2    0xA8
#define PS2_CMD_TEST_P1      0xAB
#define PS2_CMD_TEST_P2      0xA9
#define PS2_CMD_TEST_CTRL    0xAA
#define PS2_CMD_SEND_TO_P2   0xD4

/* PS/2 controller config byte bits */
#define PS2_CFG_P1_IRQ      (1 << 0)
#define PS2_CFG_P2_IRQ      (1 << 1)
#define PS2_CFG_SYSTEM_FLAG (1 << 2)
#define PS2_CFG_P1_CLOCK    (1 << 4)
#define PS2_CFG_P2_CLOCK    (1 << 5)
#define PS2_CFG_P1_TRANS    (1 << 6)

/* Controller self-test result */
#define PS2_TEST_OK 0x55

/* Wait/status helpers */
void ps2_wait_write(void);
void ps2_wait_read(void);

/* Low-level I/O */
void    ps2_write_command(uint8_t cmd);
void    ps2_write_data(uint8_t data);
uint8_t ps2_read_data(void);

/* Controller initialisation */
int ps2_controller_init(void);

/* Port enable/disable */
void ps2_enable_port1(void);
void ps2_enable_port2(void);
void ps2_disable_port2(void);

/* Port self-test */
int ps2_test_port1(void);
int ps2_test_port2(void);

#endif /* PS2_H */
