#ifndef IPMI_H
#define IPMI_H

#include "types.h"

/* IPMI interface types */
#define IPMI_IF_KCS    0  /* Keyboard Controller Style */
#define IPMI_IF_SMIC   1  /* System Management Interrupt Controller */
#define IPMI_IF_BT     2  /* Block Transfer */
#define IPMI_IF_SSIF   3  /* SMBus System Interface */

/* IPMI KCS I/O ports (typical BMC base = 0xCA2) */
#define IPMI_KCS_BASE      0xCA2
#define IPMI_KCS_DATA      (IPMI_KCS_BASE + 0)  /* Data register */
#define IPMI_KCS_STATUS    (IPMI_KCS_BASE + 1)  /* Status register */
#define IPMI_KCS_CMD       (IPMI_KCS_BASE + 1)  /* Command register (same as status on write) */

/* KCS status bits */
#define IPMI_KCS_STS_OBF   (1 << 0)  /* Output Buffer Full */
#define IPMI_KCS_STS_IBF   (1 << 1)  /* Input Buffer Full */
#define IPMI_KCS_STS_SMS   (1 << 2)  /* State Machine State */
#define IPMI_KCS_STS_CD    (1 << 3)  /* Command/Data */
#define IPMI_KCS_STS_OEM1  (1 << 4)
#define IPMI_KCS_STS_OEM2  (1 << 5)
#define IPMI_KCS_STS_SMS_ATN (1 << 6)  /* SMS Attention */
#define IPMI_KCS_STS_HBUSY (1 << 7)  /* Host Busy */

/* KCS commands */
#define IPMI_KCS_GET_STATUS  0x60
#define IPMI_KCS_WRITE_START 0x61
#define IPMI_KCS_WRITE_END   0x62

/* KCS state machine states */
#define IPMI_KCS_IDLE      0
#define IPMI_KCS_READ      1
#define IPMI_KCS_WRITE     2
#define IPMI_KCS_ERROR     3

/* IPMI message (netfn + cmd + data) */
#define IPMI_MAX_DATA  32

struct ipmi_msg {
    uint8_t  netfn;
    uint8_t  cmd;
    uint8_t  data[IPMI_MAX_DATA];
    uint8_t  data_len;
    uint8_t  rsp[IPMI_MAX_DATA + 2]; /* +NetFn +cmd +code */
    uint8_t  rsp_len;
    uint8_t  completion_code;
};

/* IPMI commands (standard) */
#define IPMI_NETFN_APP         0x06
#define IPMI_NETFN_STORAGE     0x0A
#define IPMI_NETFN_CHASSIS     0x00
#define IPMI_NETFN_BRIDGE      0x02
#define IPMI_NETFN_SENSOR      0x04

#define IPMI_CMD_GET_DEVICE_ID         0x01
#define IPMI_CMD_COLD_RESET            0x02
#define IPMI_CMD_WARM_RESET            0x03
#define IPMI_CMD_GET_SELF_TEST         0x04
#define IPMI_CMD_GET_SYSTEM_GUID       0x37
#define IPMI_CMD_GET_CHASSIS_STATUS    0x01
#define IPMI_CMD_CHASSIS_CONTROL       0x02

/* IPMI driver state */
struct ipmi_device {
    int      present;
    int      if_type;     /* KCS, SMIC, etc. */
    uint16_t base_addr;
    uint8_t  bmc_version;
    uint8_t  fw_rev1;
    uint8_t  fw_rev2;
};

/* API */
int  ipmi_init(void);
int  ipmi_is_present(void);
int  ipmi_is_initialised(void);
int  ipmi_send_cmd(struct ipmi_msg *msg);
int  ipmi_get_device_id(uint8_t *dev_id, uint8_t *rev);
int  ipmi_chassis_status(uint8_t *power_state);
int  ipmi_chassis_control(int power_action);
const struct ipmi_device *ipmi_get_device(void);

/* Power actions */
#define IPMI_POWER_OFF   0
#define IPMI_POWER_ON    1
#define IPMI_POWER_CYCLE 2
#define IPMI_POWER_RESET 3

#endif /* IPMI_H */
