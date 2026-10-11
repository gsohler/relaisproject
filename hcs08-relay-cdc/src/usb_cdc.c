/*
 * usb_cdc.c - Minimal polled USB CDC-ACM device stack for MC9S08JM60/JM32.
 *
 * Endpoint usage:
 *   EP0        control, 32 bytes
 *   EP1 IN     interrupt, CDC notifications (never sent, always NAKs)
 *   EP2 OUT    bulk, 64 bytes, host -> device data
 *   EP3 IN     bulk, 64 bytes, device -> host data
 *
 * USB RAM layout (0x1860..0x195F, buffers 16-byte aligned):
 *   0x1860 BDT (32 bytes)
 *   0x1880 EP0 OUT  32
 *   0x18A0 EP0 IN   32
 *   0x18C0 EP1 IN   16
 *   0x18D0 EP2 OUT  64
 *   0x1910 EP3 IN   64   (ends 0x194F)
 */
#include "jm60.h"
#include "board.h"
#include "usb_cdc.h"
#include "usb_desc.h"
#include "serial.h"

#define BUF_EP0_OUT 0x1880
#define BUF_EP0_IN  0x18A0
#define BUF_EP1_IN  0x18C0
#define BUF_EP2_OUT 0x18D0
#define BUF_EP3_IN  0x1910

#define UBUF(a) HW_PTR(a)

/* Standard requests */
#define REQ_GET_STATUS        0
#define REQ_CLEAR_FEATURE     1
#define REQ_SET_FEATURE       3
#define REQ_SET_ADDRESS       5
#define REQ_GET_DESCRIPTOR    6
#define REQ_GET_CONFIGURATION 8
#define REQ_SET_CONFIGURATION 9
#define REQ_GET_INTERFACE     10
#define REQ_SET_INTERFACE     11

/* CDC class requests */
#define CDC_SET_LINE_CODING        0x20
#define CDC_GET_LINE_CODING        0x21
#define CDC_SET_CONTROL_LINE_STATE 0x22
#define CDC_SEND_BREAK             0x23

enum { CS_IDLE, CS_DATA_IN, CS_DATA_OUT, CS_STATUS_IN, CS_STATUS_OUT };

/* SETUP packet fields */
static uint8_t  sp_type, sp_req;
static uint16_t sp_value, sp_index, sp_length;

static uint8_t ctrl_state;
static const uint8_t *ep0_ptr;
static uint8_t ep0_remaining;
static uint8_t ep0_zlp;
static uint8_t ep0_toggle;
static uint8_t pending_addr;        /* 0x80 | addr when SET_ADDRESS pending */

static uint8_t ep0_scratch[64];     /* GET_STATUS answers, string descriptors */

static uint8_t usb_active;
static uint8_t usb_configuration;
static uint8_t ep2_toggle, ep3_toggle;
static uint8_t line_state;          /* bit0 DTR, bit1 RTS */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 }; /* 115200 8N1 */

/* OUT data received on EP2, handed to the application by cdc_read() */
static volatile uint8_t rx_pending;

/* TX ring buffer */
#define TX_SIZE 128                 /* power of two */
static uint8_t tx_buf[TX_SIZE];
static uint8_t tx_head, tx_tail;    /* head: write, tail: read */
static uint8_t tx_zlp;

#define TX_COUNT() ((uint8_t)(tx_head - tx_tail) & (TX_SIZE - 1))

/* ------------------------------------------------------------------ */
/* EP0 helpers                                                        */
/* ------------------------------------------------------------------ */

static void ep0_arm_setup(void)
{
    BDT[BD_EP0_OUT].cnt = EP0_SIZE;
    BDT[BD_EP0_OUT].stat = BD_OWN | BD_DTS;             /* DATA0 */
}

/* Waiting for the status OUT of an IN data stage. No DTS, so both the
 * zero-length DATA1 status packet and a new (DATA0) SETUP are accepted. */
static void ep0_arm_out_any(void)
{
    BDT[BD_EP0_OUT].cnt = EP0_SIZE;
    BDT[BD_EP0_OUT].stat = BD_OWN;
}

static void ep0_arm_out_data1(void)
{
    BDT[BD_EP0_OUT].cnt = EP0_SIZE;
    BDT[BD_EP0_OUT].stat = BD_OWN | BD_DTS | BD_DATA1;
}

static void ep0_send_status(void)
{
    BDT[BD_EP0_IN].cnt = 0;
    BDT[BD_EP0_IN].stat = BD_OWN | BD_DTS | BD_DATA1;
    ctrl_state = CS_STATUS_IN;
    ep0_arm_setup();
}

static void ep0_stall(void)
{
    BDT[BD_EP0_IN].stat = BD_OWN | BD_BDTSTALL;
    ep0_arm_setup();
    ctrl_state = CS_IDLE;
}

static void ep0_send_chunk(void)
{
    uint8_t n = ep0_remaining > EP0_SIZE ? EP0_SIZE : ep0_remaining;
    uint8_t i;
    volatile uint8_t *dst = UBUF(BUF_EP0_IN);

    for (i = 0; i < n; i++)
        dst[i] = ep0_ptr[i];
    ep0_ptr += n;
    ep0_remaining -= n;
    if (n < EP0_SIZE)
        ep0_zlp = 0;                /* short packet ends the transfer */

    BDT[BD_EP0_IN].cnt = n;
    BDT[BD_EP0_IN].stat = BD_OWN | BD_DTS | (ep0_toggle ? BD_DATA1 : 0);
    ep0_toggle ^= 1;
}

static void ep0_send_data(const uint8_t *data, uint16_t len)
{
    if (len > sp_length)
        len = sp_length;
    if (len > 255)
        len = 255;
    ep0_ptr = data;
    ep0_remaining = (uint8_t)len;
    /* a ZLP is needed if we send less than requested and end on a full packet */
    ep0_zlp = (len < sp_length) && ((len % EP0_SIZE) == 0);
    ep0_toggle = 1;
    ctrl_state = CS_DATA_IN;
    ep0_send_chunk();
    ep0_arm_out_any();
}

/* ------------------------------------------------------------------ */
/* Endpoint configuration                                             */
/* ------------------------------------------------------------------ */

static void ep2_arm(void)
{
    BDT[BD_EP(2)].cnt = CDC_BULK_SIZE;
    BDT[BD_EP(2)].stat = BD_OWN | BD_DTS | (ep2_toggle ? BD_DATA1 : 0);
}

static void cdc_configure(uint8_t cfg)
{
    usb_configuration = cfg;
    rx_pending = 0;
    tx_head = tx_tail = 0;
    tx_zlp = 0;
    ep2_toggle = ep3_toggle = 0;

    BDT[BD_EP(1)].stat = 0;
    BDT[BD_EP(2)].stat = 0;
    BDT[BD_EP(3)].stat = 0;

    if (cfg) {
        BDT[BD_EP(1)].epadr = USB_EPADR(BUF_EP1_IN);
        BDT[BD_EP(2)].epadr = USB_EPADR(BUF_EP2_OUT);
        BDT[BD_EP(3)].epadr = USB_EPADR(BUF_EP3_IN);
        EPCTL(1) = EPCTL_EPHSHK | EPCTL_EPTXEN | EPCTL_EPCTLDIS;
        EPCTL(2) = EPCTL_EPHSHK | EPCTL_EPRXEN | EPCTL_EPCTLDIS;
        EPCTL(3) = EPCTL_EPHSHK | EPCTL_EPTXEN | EPCTL_EPCTLDIS;
        ep2_arm();
    } else {
        EPCTL(1) = 0;
        EPCTL(2) = 0;
        EPCTL(3) = 0;
        line_state = 0;
    }
}

static void usb_bus_reset(void)
{
    uint8_t i;

    ADDR = 0;
    pending_addr = 0;
    CTL |= CTL_ODDRST;
    CTL &= (uint8_t)~CTL_ODDRST;

    for (i = 1; i <= 6; i++)
        EPCTL(i) = 0;
    for (i = 0; i < 10; i++)
        BDT[i].stat = 0;

    cdc_configure(0);

    BDT[BD_EP0_IN].epadr = USB_EPADR(BUF_EP0_IN);
    BDT[BD_EP0_OUT].epadr = USB_EPADR(BUF_EP0_OUT);
    ep0_arm_setup();
    ctrl_state = CS_IDLE;

    EPCTL(0) = EPCTL_EPHSHK | EPCTL_EPTXEN | EPCTL_EPRXEN;   /* 0x0D control */

    ERRSTAT_CLR(0xFF);
    INTSTAT_CLR(0xFF);
    CTL &= (uint8_t)~CTL_TSUSPEND;
}

/* ------------------------------------------------------------------ */
/* SETUP processing                                                   */
/* ------------------------------------------------------------------ */

static uint8_t string_desc(uint8_t idx)
{
    const char *s;
    uint8_t n = 2;

    if (idx == 0) {
        ep0_scratch[0] = 4;
        ep0_scratch[1] = 0x03;
        ep0_scratch[2] = 0x09;      /* 0x0409 English (US) */
        ep0_scratch[3] = 0x04;
        return 4;
    }
    if (idx >= USB_NUM_STRINGS)
        return 0;
    s = usb_strings[idx];
    if (!s) {                       /* serial number from the flash slot */
        static __xdata char ser[SERIAL_STR_LEN + 1];
        serial_get(ser);
        s = ser;
    }
    while (*s && n < sizeof(ep0_scratch) - 1) {
        ep0_scratch[n++] = (uint8_t)*s++;
        ep0_scratch[n++] = 0;
    }
    ep0_scratch[0] = n;
    ep0_scratch[1] = 0x03;
    return n;
}

static volatile uint8_t *ep_ctl_for(uint8_t ep_addr)
{
    uint8_t n = ep_addr & 0x0F;
    if (n == 0 || n > 3 || !usb_configuration)
        return 0;
    return &EPCTL(n);
}

static void handle_standard(void)
{
    uint8_t recip = sp_type & 0x1F;
    volatile uint8_t *epc;

    switch (sp_req) {
    case REQ_GET_STATUS:
        ep0_scratch[0] = 0;
        ep0_scratch[1] = 0;
        if (recip == 2) {                       /* endpoint */
            epc = ep_ctl_for((uint8_t)sp_index);
            if ((sp_index & 0x0F) != 0) {
                if (!epc) { ep0_stall(); return; }
                if (*epc & EPCTL_EPSTALL)
                    ep0_scratch[0] = 1;
            }
        }
        ep0_send_data(ep0_scratch, 2);
        return;

    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        if (recip == 2 && sp_value == 0) {      /* ENDPOINT_HALT */
            uint8_t ep = (uint8_t)sp_index;
            epc = ep_ctl_for(ep);
            if ((ep & 0x0F) == 0) { ep0_send_status(); return; }
            if (!epc) { ep0_stall(); return; }
            if (sp_req == REQ_SET_FEATURE) {
                *epc |= EPCTL_EPSTALL;
            } else {
                *epc &= (uint8_t)~EPCTL_EPSTALL;
                if ((ep & 0x0F) == CDC_OUT_EP) {
                    ep2_toggle = 0;
                    ep2_arm();
                } else if ((ep & 0x0F) == CDC_IN_EP) {
                    ep3_toggle = 0;
                    BDT[BD_EP(3)].stat = 0;
                }
            }
            ep0_send_status();
            return;
        }
        if (recip == 0 && sp_value == 1) {      /* DEVICE_REMOTE_WAKEUP: ignore */
            ep0_send_status();
            return;
        }
        break;

    case REQ_SET_ADDRESS:
        /* The address takes effect after the status stage */
        pending_addr = 0x80 | (uint8_t)(sp_value & 0x7F);
        ep0_send_status();
        return;

    case REQ_GET_DESCRIPTOR: {
        uint8_t type = (uint8_t)(sp_value >> 8);
        uint8_t idx = (uint8_t)sp_value;
        if (type == 1) {
            ep0_send_data(usb_device_desc, sizeof(usb_device_desc));
            return;
        }
        if (type == 2) {
            ep0_send_data(usb_config_desc, USB_CONFIG_DESC_LEN);
            return;
        }
        if (type == 3) {
            uint8_t n = string_desc(idx);
            if (n) {
                ep0_send_data(ep0_scratch, n);
                return;
            }
        }
        break;                                  /* incl. device qualifier */
    }

    case REQ_GET_CONFIGURATION:
        ep0_scratch[0] = usb_configuration;
        ep0_send_data(ep0_scratch, 1);
        return;

    case REQ_SET_CONFIGURATION:
        if (sp_value <= 1) {
            cdc_configure((uint8_t)sp_value);
            ep0_send_status();
            return;
        }
        break;

    case REQ_GET_INTERFACE:
        if (usb_configuration && sp_index < 2) {
            ep0_scratch[0] = 0;
            ep0_send_data(ep0_scratch, 1);
            return;
        }
        break;

    case REQ_SET_INTERFACE:
        if (usb_configuration && sp_index < 2 && sp_value == 0) {
            ep0_send_status();
            return;
        }
        break;
    }
    ep0_stall();
}

static void handle_class(void)
{
    switch (sp_req) {
    case CDC_SET_LINE_CODING:
        if (sp_length == 7) {
            ctrl_state = CS_DATA_OUT;
            ep0_arm_out_data1();
            return;
        }
        break;
    case CDC_GET_LINE_CODING:
        ep0_send_data(line_coding, 7);
        return;
    case CDC_SET_CONTROL_LINE_STATE:
        line_state = (uint8_t)sp_value & 0x03;
        ep0_send_status();
        return;
    case CDC_SEND_BREAK:
        ep0_send_status();
        return;
    }
    ep0_stall();
}

static void handle_setup(void)
{
    volatile uint8_t *p = UBUF(BUF_EP0_OUT);

    /* Take EP0 IN back from the SIE (cancels stall / unfinished transfer).
     * Safe: the SIE sets TSUSPEND on SETUP and waits for us. */
    BDT[BD_EP0_IN].stat = 0;

    sp_type   = p[0];
    sp_req    = p[1];
    sp_value  = p[2] | ((uint16_t)p[3] << 8);
    sp_index  = p[4] | ((uint16_t)p[5] << 8);
    sp_length = p[6] | ((uint16_t)p[7] << 8);

    ctrl_state = CS_IDLE;
    pending_addr = 0;

    switch (sp_type & 0x60) {
    case 0x00: handle_standard(); break;
    case 0x20: handle_class();    break;
    default:   ep0_stall();       break;
    }

    CTL &= (uint8_t)~CTL_TSUSPEND;
}

static void handle_ep0(uint8_t stat)
{
    if (stat & STAT_IN) {
        /* IN transaction on EP0 completed */
        if (ctrl_state == CS_DATA_IN) {
            if (ep0_remaining || ep0_zlp)
                ep0_send_chunk();
            else
                ctrl_state = CS_STATUS_OUT;     /* OUT already armed */
        } else if (ctrl_state == CS_STATUS_IN) {
            if (pending_addr) {
                ADDR = pending_addr & 0x7F;
                pending_addr = 0;
            }
            ctrl_state = CS_IDLE;
        }
        return;
    }

    /* OUT or SETUP on EP0 */
    if (BD_PID(BDT[BD_EP0_OUT].stat) == PID_SETUP) {
        handle_setup();
        return;
    }

    if (ctrl_state == CS_DATA_OUT) {
        volatile uint8_t *p = UBUF(BUF_EP0_OUT);
        uint8_t i, n = BDT[BD_EP0_OUT].cnt;
        if (sp_req == CDC_SET_LINE_CODING) {
            for (i = 0; i < n && i < 7; i++)
                line_coding[i] = p[i];
        }
        ep0_send_status();
        return;
    }

    /* status OUT of an IN data stage, or unexpected packet */
    ctrl_state = CS_IDLE;
    ep0_arm_setup();
}

/* ------------------------------------------------------------------ */
/* Bulk IN                                                            */
/* ------------------------------------------------------------------ */

static void cdc_tx_service(void)
{
    uint8_t n = 0;
    volatile uint8_t *dst;

    if (!usb_configuration || (BDT[BD_EP(3)].stat & BD_OWN))
        return;
    if (EPCTL(3) & EPCTL_EPSTALL)
        return;
    if (TX_COUNT() == 0 && !tx_zlp)
        return;

    dst = UBUF(BUF_EP3_IN);
    while (n < CDC_BULK_SIZE && tx_tail != tx_head) {
        dst[n++] = tx_buf[tx_tail];
        tx_tail = (tx_tail + 1) & (TX_SIZE - 1);
    }
    /* After a full packet with nothing following, terminate with a ZLP */
    tx_zlp = (n == CDC_BULK_SIZE && tx_tail == tx_head);

    BDT[BD_EP(3)].cnt = n;
    BDT[BD_EP(3)].stat = BD_OWN | BD_DTS | (ep3_toggle ? BD_DATA1 : 0);
    ep3_toggle ^= 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void usb_init(void)
{
    uint8_t c;

    USBCTL0 = USBCTL0_USBRESET;
#ifndef HOST_TEST
    while (USBCTL0 & USBCTL0_USBRESET)
        ;
#endif

    usb_bus_reset();
    usb_active = 1;

    c = USBCTL0_USBPHYEN;
#if USB_INTERNAL_VREG
    c |= USBCTL0_USBVREN;
#endif
    USBCTL0 = c;
    CTL = CTL_USBEN;
    INTENB = 0;                     /* polled operation */
    ERRENB = 0;
#if USB_INTERNAL_PULLUP
    c |= USBCTL0_USBPU;             /* attach: host sees the device now */
#endif
    USBCTL0 = c;
}

void usb_poll(void)
{
    uint8_t s;

    /* usb_poll() runs in every wait loop (main loop, delay_ms, cdc_putc),
     * so this is the one place that services the COP watchdog */
    COP_SERVICE();
    if (!usb_active)
        return;                     /* e.g. delay_ms() before usb_init() */
    s = INTSTAT;

    if (s & INT_USBRST) {
        usb_bus_reset();
        return;
    }
    if (s & INT_STALL) {
        EPCTL(0) &= (uint8_t)~EPCTL_EPSTALL;
        INTSTAT_CLR(INT_STALL);
    }
    if (s & INT_ERROR) {
        ERRSTAT_CLR(0xFF);
        INTSTAT_CLR(INT_ERROR);
    }
    if (s & (INT_SLEEP | INT_RESUME | INT_SOFTOK))
        INTSTAT_CLR(s & (INT_SLEEP | INT_RESUME | INT_SOFTOK));

    while (INTSTAT & INT_TOKDNE) {
        uint8_t st = STAT;          /* must be read before clearing TOKDNE */
        uint8_t ep = STAT_ENDP(st);
        INTSTAT_CLR(INT_TOKDNE);

        if (ep == 0) {
            handle_ep0(st);
        } else if (ep == CDC_OUT_EP && !(st & STAT_IN)) {
            rx_pending = 1;         /* re-armed in cdc_read() */
        }
        /* EP3 IN completions need no action: cdc_tx_service() below
         * refills the buffer once the BD is back with the CPU. */
    }

    cdc_tx_service();
}

uint8_t cdc_is_configured(void)
{
    return usb_configuration != 0;
}

uint8_t cdc_dtr(void)
{
    return line_state & 0x01;
}

uint8_t cdc_rx_pending(void)
{
    return rx_pending;
}

uint8_t cdc_read(uint8_t *buf)
{
    uint8_t i, n;
    volatile uint8_t *src = UBUF(BUF_EP2_OUT);

    if (!rx_pending)
        return 0;
    n = BDT[BD_EP(2)].cnt;
    for (i = 0; i < n; i++)
        buf[i] = src[i];
    rx_pending = 0;
    ep2_toggle ^= 1;
    ep2_arm();
    return n;
}

void cdc_putc(char c)
{
    uint16_t timeout = 0;

    if (!usb_configuration || !cdc_dtr())
        return;
    while (TX_COUNT() == TX_SIZE - 1) {
        usb_poll();
        if (!usb_configuration || !cdc_dtr() || ++timeout == 0)
            return;                 /* host gone or not reading: drop */
    }
    tx_buf[tx_head] = (uint8_t)c;
    tx_head = (tx_head + 1) & (TX_SIZE - 1);
}

void cdc_puts(const char *s)
{
    while (*s)
        cdc_putc(*s++);
}
