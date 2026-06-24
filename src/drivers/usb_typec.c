/*
 * usb_typec.c — USB Type-C / Power Delivery subsystem
 *
 * Implements CC (Configuration Channel) pin detection, role
 * determination (source/sink/DRP), and basic PD (Power Delivery)
 * contract negotiation.
 *
 * Type-C connector state machine:
 *   Unattached → AttachWait → Attached (Source or Sink) → Try.SRC/SNK
 *
 * Item S46 — USB Type-C/PD driver
 */

#include "usb.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "spinlock.h"
#include "errno.h"

/* ── Type-C CC pin voltage levels ────────────────────────────────── */
#define TYPEC_CC_VSINK_MIN      200    /* mV — minimum for sink detect */
#define TYPEC_CC_VSINK_MAX      600    /* mV — maximum for sink detect */
#define TYPEC_CC_VSRC_DEFAULT   550    /* mV — source default */
#define TYPEC_CC_VSRC_1A5       1800   /* mV — source 1.5A */
#define TYPEC_CC_VSRC_3A0       3300   /* mV — source 3.0A */

/* ── Type-C state machine ────────────────────────────────────────── */
enum typec_state {
    TYPEC_STATE_UNATTACHED = 0,
    TYPEC_STATE_ATTACH_WAIT_SRC,
    TYPEC_STATE_ATTACH_WAIT_SNK,
    TYPEC_STATE_ATTACHED_SRC,
    TYPEC_STATE_ATTACHED_SNK,
    TYPEC_STATE_TRY_SRC,
    TYPEC_STATE_TRY_SNK,
    TYPEC_STATE_ACCESSORY,
};

/* ── Power Delivery roles ────────────────────────────────────────── */
enum pd_role {
    PD_ROLE_SOURCE = 0,
    PD_ROLE_SINK   = 1,
    PD_ROLE_DRP    = 2,   /* Dual-Role Power */
};

/* ── PD contract states ──────────────────────────────────────────── */
enum pd_contract_state {
    PD_CONTRACT_NONE = 0,
    PD_CONTRACT_NEGOTIATING,
    PD_CONTRACT_ESTABLISHED,
    PD_CONTRACT_GIVE_SRC_CAP,
    PD_CONTRACT_GIVE_SNK_CAP,
    PD_CONTRACT_ACCEPT,
    PD_CONTRACT_REJECT,
    PD_CONTRACT_PS_RDY,
};

/* ── Type-C connector instance ───────────────────────────────────── */
struct typec_port {
    int      id;                  /* Port index (0-based) */
    enum typec_state     state;
    enum pd_role         supported_roles;   /* Source, Sink, or DRP */
    enum pd_role         current_role;

    /* CC pin measurements (mV) */
    int      cc1_voltage_mv;
    int      cc2_voltage_mv;
    int      cc1_present;
    int      cc2_present;

    /* Orientation: 0 = CC1 is primary, 1 = CC2 is primary */
    int      orientation;

    /* PD contract */
    enum pd_contract_state contract_state;
    uint32_t src_voltage_mv;      /* Source advertised voltage */
    uint32_t src_current_ma;      /* Source advertised current */
    uint32_t snk_request_voltage; /* Sink requested voltage */
    uint32_t snk_request_current; /* Sink requested current */
    uint32_t negotiated_voltage;  /* Final negotiated voltage */
    uint32_t negotiated_current;  /* Final negotiated current */
    uint32_t max_power_mw;        /* Maximum allowed power */
    int      pd_revision;         /* PD revision (2.0 or 3.0) */

    spinlock_t lock;
};

/* ── Global Type-C ports ─────────────────────────────────────────── */
#define MAX_TYPEC_PORTS 4
static struct typec_port g_typec_ports[MAX_TYPEC_PORTS];
static int g_typec_port_count = 0;
static int g_typec_initialized = 0;

/* ── Internal helpers ────────────────────────────────────────────── */

static const char *typec_state_name(enum typec_state s)
{
    switch (s) {
    case TYPEC_STATE_UNATTACHED:    return "Unattached";
    case TYPEC_STATE_ATTACH_WAIT_SRC: return "AttachWait.SRC";
    case TYPEC_STATE_ATTACH_WAIT_SNK: return "AttachWait.SNK";
    case TYPEC_STATE_ATTACHED_SRC:  return "Attached.SRC";
    case TYPEC_STATE_ATTACHED_SNK:  return "Attached.SNK";
    case TYPEC_STATE_TRY_SRC:       return "Try.SRC";
    case TYPEC_STATE_TRY_SNK:       return "Try.SNK";
    case TYPEC_STATE_ACCESSORY:     return "Accessory";
    default: return "Unknown";
    }
}

/* ── CC pin detection ────────────────────────────────────────────── */

/*
 * typec_detect_cc — Simulate CC pin voltage measurement.
 *
 * On real hardware, this reads the CC1 and CC2 pins through an
 * ADC or comparator connected to the Type-C port controller.
 * We simulate connection detection using a simple periodic pattern
 * so the state machine can exercise DRP transitions.
 */
static void typec_detect_cc(struct typec_port *port)
{
    /* In a real driver, this would:
     *   1. Enable measurement on CC1/CC2
     *   2. Read ADC values via the Type-C PHY
     *   3. Compare against threshold voltages
     *
     * Simulation: if the port is DRP, alternate between unattached
     * and attached-on-CC1 to exercise the state machine.
     */
    if (port->supported_roles == PD_ROLE_DRP) {
        /* Simulate a device connected on CC1 every other poll.
         * First poll: unattached, second: attached, etc.
         * Use a simple toggle based on a static poll counter. */
        static int poll_count = 0;
        poll_count++;

        if (poll_count % 3 == 0) {
            /* Simulate a device attached on CC1 */
            port->cc1_voltage_mv = TYPEC_CC_VSRC_DEFAULT;
            port->cc2_voltage_mv = 0;
            port->cc1_present = 1;
            port->cc2_present = 0;
        } else if (poll_count % 7 == 0) {
            /* Simulate an e-marked cable on CC2 */
            port->cc1_voltage_mv = 0;
            port->cc2_voltage_mv = TYPEC_CC_VSRC_DEFAULT;
            port->cc1_present = 0;
            port->cc2_present = 1;
        } else {
            port->cc1_voltage_mv = 0;
            port->cc2_voltage_mv = 0;
            port->cc1_present = 0;
            port->cc2_present = 0;
        }
    } else {
        /* For source-only or sink-only, always report connected */
        if (port->current_role == PD_ROLE_SOURCE) {
            port->cc1_voltage_mv = TYPEC_CC_VSRC_DEFAULT;
            port->cc2_voltage_mv = 0;
            port->cc1_present = 1;
            port->cc2_present = 0;
        } else {
            port->cc1_voltage_mv = TYPEC_CC_VSINK_MIN + 50;
            port->cc2_voltage_mv = 0;
            port->cc1_present = 1;
            port->cc2_present = 0;
        }
    }
}

/*
 * typec_detect_orientation — Determine cable orientation.
 * The CC pin with a valid voltage indicates the primary CC line.
 * The other CC pin is VCONN (for e-marked cables).
 */
static int typec_detect_orientation(struct typec_port *port)
{
    if (port->cc1_present && !port->cc2_present) {
        port->orientation = 0;
        return 0;
    } else if (port->cc2_present && !port->cc1_present) {
        port->orientation = 1;
        return 0;
    }
    /* Both present or both absent — orientation unknown */
    port->orientation = 0;
    return -1;
}

/* ── Role determination ──────────────────────────────────────────── */

/*
 * typec_determine_role — Resolve Source vs Sink role based on
 * CC line state and supported roles.
 */
static enum pd_role typec_determine_role(struct typec_port *port)
{
    if (port->supported_roles == PD_ROLE_SOURCE)
        return PD_ROLE_SOURCE;
    if (port->supported_roles == PD_ROLE_SINK)
        return PD_ROLE_SINK;

    /* DRP: default to source, can be overridden */
    return PD_ROLE_SOURCE;
}

/* ── PD message simulation ───────────────────────────────────────── */

/*
 * Send a PD message over the CC line (BMC-coded).
 * In a real driver, this would use the PD PHY to send BMC packets.
 */
static int pd_send_message(struct typec_port *port, const uint8_t *msg, int len)
{
    (void)port;
    (void)msg;
    (void)len;
    /* Stub — real implementation would use SOP* messaging over CC */
    return 0;
}

/*\n * pd_negotiate_contract — Run PD contract negotiation as Source.
 *
 * Sequence:
 *   1. Send Source_Capabilities
 *   2. Receive Sink_Request
 *   3. Send Accept
 *   4. Wait for PS_RDY
 *   5. Contract established
 */
static int pd_negotiate_as_source(struct typec_port *port)
{
    port->contract_state = PD_CONTRACT_GIVE_SRC_CAP;

    /* Build Source_Capabilities message */
    uint8_t src_caps[] = {
        0x00, 0x00, 0x00, 0x00,  /* Header */
        0x2C, 0x91, 0x00, 0x00,  /* PDO 1: 5V @ 1.5A (Fixed Supply) */
        0x2C, 0xD1, 0x00, 0x00,  /* PDO 2: 9V @ 1.5A */
        0x2C, 0xF1, 0x00, 0x00,  /* PDO 3: 15V @ 1.0A */
    };

    /* Example PDO format (Fixed Supply):
     *   bits [31:30] = 00 (Fixed Supply)
     *   bits [29:20] = Dual-Role, USB Suspend, etc.
     *   bits [19:10] = Voltage in 50mV units
     *   bits [9:0]   = Current in 10mA units
     *
     *   5V  @ 1.5A: voltage=100(0x064) current=150(0x096)
     *   9V  @ 1.5A: voltage=180(0x0B4) current=150(0x096)
     *   15V @ 1.0A: voltage=300(0x12C) current=100(0x064)
     */

    if (pd_send_message(port, src_caps, sizeof(src_caps)) < 0)
        return -1;

    port->src_voltage_mv = 5000;
    port->src_current_ma = 1500;
    port->contract_state = PD_CONTRACT_NEGOTIATING;

    /* In a real driver, we'd now wait for Sink_Request and reply Accept.
     * For simulation, establish the contract with default settings. */
    port->contract_state = PD_CONTRACT_ESTABLISHED;
    port->negotiated_voltage = port->src_voltage_mv;
    port->negotiated_current = port->src_current_ma;
    port->max_power_mw = port->negotiated_voltage * port->negotiated_current / 1000;

    kprintf("[TYPEC] Port %d: PD contract negotiated: %u mV %u mA (%u mW)\n",
            port->id, port->negotiated_voltage, port->negotiated_current,
            port->max_power_mw);

    return 0;
}

/*
 * pd_negotiate_as_sink — Run PD contract negotiation as Sink.
 */
static int pd_negotiate_as_sink(struct typec_port *port)
{
    port->contract_state = PD_CONTRACT_GIVE_SNK_CAP;

    /* Build Sink_Capabilities message */
    uint8_t snk_caps[] = {
        0x00, 0x00, 0x00, 0x00,  /* Header */
        0x2C, 0x91, 0x00, 0x00,  /* RDO: 5V @ 1.0A requested */
    };

    if (pd_send_message(port, snk_caps, sizeof(snk_caps)) < 0)
        return -1;

    port->contract_state = PD_CONTRACT_NEGOTIATING;

    /* Simulate establishing contract with default settings */
    port->contract_state = PD_CONTRACT_ESTABLISHED;
    port->negotiated_voltage = 5000;
    port->negotiated_current = 1000;
    port->max_power_mw = 5000;

    kprintf("[TYPEC] Port %d: PD contract as sink: %u mV %u mA\n",
            port->id, port->negotiated_voltage, port->negotiated_current);

    return 0;
}

/* ── State machine processing ────────────────────────────────────── */

static void typec_run_state_machine(struct typec_port *port)
{
    spinlock_acquire(&port->lock);

    enum typec_state old_state = port->state;

    switch (port->state) {
    case TYPEC_STATE_UNATTACHED:
        /* Poll for CC voltage.  If detected, transition. */
        typec_detect_cc(port);

        if (port->cc1_present || port->cc2_present) {
            typec_detect_orientation(port);
            port->current_role = typec_determine_role(port);

            if (port->current_role == PD_ROLE_SOURCE)
                port->state = TYPEC_STATE_ATTACHED_SRC;
            else
                port->state = TYPEC_STATE_ATTACHED_SNK;
        }
        break;

    case TYPEC_STATE_ATTACHED_SRC:
        /* Negotiate PD contract as source */
        if (port->contract_state != PD_CONTRACT_ESTABLISHED)
            pd_negotiate_as_source(port);
        break;

    case TYPEC_STATE_ATTACHED_SNK:
        /* Negotiate PD contract as sink */
        if (port->contract_state != PD_CONTRACT_ESTABLISHED)
            pd_negotiate_as_sink(port);
        break;

    case TYPEC_STATE_ATTACH_WAIT_SRC:
        /* Waiting to become source — poll CC lines for connection */
        typec_detect_cc(port);
        if (port->cc1_present || port->cc2_present) {
            typec_detect_orientation(port);
            port->current_role = PD_ROLE_SOURCE;
            port->state = TYPEC_STATE_ATTACHED_SRC;
            kprintf("[TYPEC] Port %d: attach wait SRC -> attached source\n",
                    port->id);
        }
        break;

    case TYPEC_STATE_ATTACH_WAIT_SNK:
        /* Waiting to become sink — poll CC lines for connection */
        typec_detect_cc(port);
        if (port->cc1_present || port->cc2_present) {
            typec_detect_orientation(port);
            port->current_role = PD_ROLE_SINK;
            port->state = TYPEC_STATE_ATTACHED_SNK;
            kprintf("[TYPEC] Port %d: attach wait SNK -> attached sink\n",
                    port->id);
        }
        break;

    case TYPEC_STATE_TRY_SRC:
        /* DRP trying to become source: monitor CC for source-capable partner */
        typec_detect_cc(port);
        if (port->cc1_voltage_mv > TYPEC_CC_VSINK_MAX) {
            /* Partner is sink-capable, we can be source */
            port->current_role = PD_ROLE_SOURCE;
            port->state = TYPEC_STATE_ATTACHED_SRC;
            kprintf("[TYPEC] Port %d: Try.SRC -> attached source\n",
                    port->id);
        } else if (port->cc1_present || port->cc2_present) {
            /* Connection detected but unclear role — try again */
            break;
        } else {
            /* No connection — go back to unattached */
            port->state = TYPEC_STATE_UNATTACHED;
        }
        break;

    case TYPEC_STATE_TRY_SNK:
        /* DRP trying to become sink: monitor CC for sink-capable partner */
        typec_detect_cc(port);
        if (port->cc1_voltage_mv > 0 &&
            port->cc1_voltage_mv < TYPEC_CC_VSRC_DEFAULT && /* typical source range */
            port->cc1_present) {
            port->current_role = PD_ROLE_SINK;
            port->state = TYPEC_STATE_ATTACHED_SNK;
            kprintf("[TYPEC] Port %d: Try.SNK -> attached sink\n",
                    port->id);
        } else if (port->cc1_present || port->cc2_present) {
            break;
        } else {
            port->state = TYPEC_STATE_UNATTACHED;
        }
        break;

    case TYPEC_STATE_ACCESSORY:
        /* Audio adapter accessory mode */
        kprintf("[TYPEC] Port %d: accessory mode active\n", port->id);
        break;
    default:
        break;
    }

    if (old_state != port->state) {
        kprintf("[TYPEC] Port %d: state %s -> %s\n",
                port->id, typec_state_name(old_state),
                typec_state_name(port->state));
    }

    spinlock_release(&port->lock);
}

/* ── Periodic poll (called from timer or workqueue) ──────────────── */

void typec_poll(void)
{
    if (!g_typec_initialized) return;

    for (int i = 0; i < g_typec_port_count; i++)
        typec_run_state_machine(&g_typec_ports[i]);
}

/* ── Public API ──────────────────────────────────────────────────── */

int typec_port_register(int port_id, enum pd_role supported_roles)
{
    if (g_typec_port_count >= MAX_TYPEC_PORTS)
        return -ENOSPC;

    struct typec_port *port = &g_typec_ports[g_typec_port_count];
    memset(port, 0, sizeof(*port));

    port->id = port_id;
    port->state = TYPEC_STATE_UNATTACHED;
    port->supported_roles = supported_roles;
    port->current_role = PD_ROLE_DRP;
    port->contract_state = PD_CONTRACT_NONE;
    spinlock_init(&port->lock);

    kprintf("[TYPEC] Port %d registered (roles=%d)\n", port_id, supported_roles);

    g_typec_port_count++;
    return g_typec_port_count - 1;
}

void typec_port_unregister(int port_id)
{
    for (int i = 0; i < g_typec_port_count; i++) {
        if (g_typec_ports[i].id == port_id) {
            kprintf("[TYPEC] Port %d unregistered\n", port_id);
            /* Compact array by removing this entry */
            if (i < g_typec_port_count - 1)
                memmove(&g_typec_ports[i], &g_typec_ports[i + 1],
                        sizeof(struct typec_port) * (size_t)(g_typec_port_count - i - 1));
            g_typec_port_count--;
            return;
        }
    }
}

enum pd_role typec_get_role(int port_id)
{
    for (int i = 0; i < g_typec_port_count; i++) {
        if (g_typec_ports[i].id == port_id)
            return g_typec_ports[i].current_role;
    }
    return PD_ROLE_SINK;
}

enum typec_state typec_get_state(int port_id)
{
    for (int i = 0; i < g_typec_port_count; i++) {
        if (g_typec_ports[i].id == port_id)
            return g_typec_ports[i].state;
    }
    return TYPEC_STATE_UNATTACHED;
}

int typec_get_negotiated_voltage(int port_id)
{
    for (int i = 0; i < g_typec_port_count; i++) {
        if (g_typec_ports[i].id == port_id)
            return (int)g_typec_ports[i].negotiated_voltage;
    }
    return 0;
}

int typec_get_negotiated_current(int port_id)
{
    for (int i = 0; i < g_typec_port_count; i++) {
        if (g_typec_ports[i].id == port_id)
            return (int)g_typec_ports[i].negotiated_current;
    }
    return 0;
}

/* ── Initialization ──────────────────────────────────────────────── */

void typec_init(void)
{
    if (g_typec_initialized) return;

    memset(g_typec_ports, 0, sizeof(g_typec_ports));
    g_typec_port_count = 0;

    /* Register ports based on platform data or ACPI (2 DRP ports by default) */
    typec_port_register(0, PD_ROLE_DRP);
    typec_port_register(1, PD_ROLE_DRP);

    g_typec_initialized = 1;
    kprintf("[OK] USB Type-C/PD initialized (%d ports)\n", g_typec_port_count);
}

/* ── Stub: typec_register_port ─────────────────────────────── */
int typec_register_port(void *dev, void *cap)
{
    (void)dev;
    (void)cap;
    kprintf("[usb] typec_register_port: not yet implemented\n");
    return 0;
}
/* ── Stub: typec_unregister_port ─────────────────────────────── */
int typec_unregister_port(void *dev)
{
    (void)dev;
    kprintf("[usb] typec_unregister_port: not yet implemented\n");
    return 0;
}
/* ── Stub: typec_set_mode ─────────────────────────────── */
int typec_set_mode(void *dev, int mode)
{
    (void)dev;
    (void)mode;
    kprintf("[usb] typec_set_mode: not yet implemented\n");
    return 0;
}
