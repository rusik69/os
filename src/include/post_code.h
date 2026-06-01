#ifndef POST_CODE_H
#define POST_CODE_H

#include "types.h"

/* Port 0x80 POST code driver */

/* POST codes (diagnostic checkpoints) */
#define POST_CODE_START       0x01
#define POST_CODE_DONE        0xFF
#define POST_CODE_DRIVERS_INIT 0x10
#define POST_CODE_FILESYSTEM  0x20
#define POST_CODE_NETWORK     0x30
#define POST_CODE_SHELL       0x40
#define POST_CODE_TEST        0x50
#define POST_CODE_PANIC       0xE0
#define POST_CODE_FATAL       0xEE

/* API */
void post_code_write(uint8_t code);
uint8_t post_code_read(void);
void post_code_init(void);

/* Aliases for common POST steps */
#define post_code_driver_init()      post_code_write(POST_CODE_DRIVERS_INIT)
#define post_code_filesystem_start() post_code_write(POST_CODE_FILESYSTEM)
#define post_code_network_start()    post_code_write(POST_CODE_NETWORK)
#define post_code_shell_start()      post_code_write(POST_CODE_SHELL)
#define post_code_test_start()       post_code_write(POST_CODE_TEST)
#define post_code_panic()            post_code_write(POST_CODE_PANIC)

#endif /* POST_CODE_H */
