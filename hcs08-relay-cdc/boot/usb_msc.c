/*
 * usb_msc.c - Polled USB mass storage device (bulk-only transport, SCSI
 * transparent command set) for the MC9S08JM60 bootloader.
 *
 * The control endpoint code is the same as in src/usb_cdc.c.
 *
 * Endpoint usage:
 *   EP0        control, 32 bytes
 *   EP2 OUT    bulk, 64 bytes, CBW and write data
 *   EP3 IN     bulk, 64 bytes, read data and CSW
 *
 * USB RAM layout (0x1860..0x195F, buffers 16-byte aligned):
 *   0x1860 BDT, 0x1880 EP0 OUT 32, 0x18A0 EP0 IN 32,
 *   0x18D0 EP2 OUT 64, 0x1910 EP3 IN 64
 */
#include "jm60.h"
#include "util.h"
#include "board.h"
#include "usb_msc.h"
#include "disk.h"

#define EP0_SIZE   32
#define BULK_SIZE  64
#define OUT_EP     2
#define IN_EP      3

#define BUF_EP0_OUT 0x1880
#define BUF_EP0_IN  0x18A0
#define BUF_EP2_OUT 0x18D0
#define BUF_EP3_IN  0x1910

#define UBUF(a) HW_PTR(a)

#define LO(x) ((uint8_t)((x) & 0xFF))
#define HI(x) ((uint8_t)((x) >> 8))

/* ------------------------------------------------------------------ */
/* Descriptors                                                        */
/* ------------------------------------------------------------------ */

static const uint8_t device_desc[18] = {
    18, 0x01, 0x00, 0x02,
    0x00, 0x00, 0x00,               /* class defined by the interface */
    EP0_SIZE,
    LO(USB_VID), HI(USB_VID), LO(USB_PID), HI(USB_PID),
    LO(BOOT_BCD_DEVICE), HI(BOOT_BCD_DEVICE),
    1, 2, 3, 1
};

#define CONFIG_LEN 32
static const uint8_t config_desc[CONFIG_LEN] = {
    9, 0x02, CONFIG_LEN, 0, 1, 1, 0, 0x80, 50,
    /* Interface 0: mass storage, SCSI transparent, bulk-only */
    9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 0x05, OUT_EP, 0x02, BULK_SIZE, 0, 0,
    7, 0x05, 0x80 | IN_EP, 0x02, BULK_SIZE, 0, 0,
};

static const char * const strings[3] = {
    USB_MANUFACTURER_STR,
    BOOT_PRODUCT_STR,
    0                               /* serial number, built at runtime */
};

/* ------------------------------------------------------------------ */
/* Control endpoint                                                   */
/* ------------------------------------------------------------------ */

enum { CS_IDLE, CS_DATA_IN, CS_STATUS_IN, CS_STATUS_OUT };

static uint8_t  sp_type, sp_req;
static uint16_t sp_value, sp_index, sp_length;
static uint8_t  ctrl_state;
static const uint8_t *ep0_ptr;
static uint8_t  ep0_remaining, ep0_zlp, ep0_toggle;
static uint8_t  pending_addr;
static __xdata uint8_t ep0_scratch[40];   /* xdata: direct page is small */

static uint8_t  usb_active, usb_configuration;
static uint8_t  ep2_toggle, ep3_toggle;
static uint8_t  rx_ready;           /* packet waiting in the EP2 buffer */

static void ep0_arm_setup(void)
{
    BDT[BD_EP0_OUT].cnt = EP0_SIZE;
    BDT[BD_EP0_OUT].stat = BD_OWN | BD_DTS;
}

static void ep0_arm_out_any(void)
{
    BDT[BD_EP0_OUT].cnt = EP0_SIZE;
    BDT[BD_EP0_OUT].stat = BD_OWN;
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
        ep0_zlp = 0;
    BDT[BD_EP0_IN].cnt = n;
    BDT[BD_EP0_IN].stat = BD_OWN | BD_DTS | (ep0_toggle ? BD_DATA1 : 0);
    ep0_toggle ^= 1;
}

static void ep0_send_data(const uint8_t *data, uint8_t len)
{
    if (len > sp_length)
        len = (uint8_t)sp_length;
    ep0_ptr = data;
    ep0_remaining = len;
    ep0_zlp = (len < sp_length) && ((len % EP0_SIZE) == 0);
    ep0_toggle = 1;
    ctrl_state = CS_DATA_IN;
    ep0_send_chunk();
    ep0_arm_out_any();
}

static void ep2_arm(void)
{
    BDT[BD_EP(2)].cnt = BULK_SIZE;
    BDT[BD_EP(2)].stat = BD_OWN | BD_DTS | (ep2_toggle ? BD_DATA1 : 0);
}

static void msc_reset_state(void);

static void configure(uint8_t cfg)
{
    usb_configuration = cfg;
    rx_ready = 0;
    ep2_toggle = ep3_toggle = 0;
    BDT[BD_EP(2)].stat = 0;
    BDT[BD_EP(3)].stat = 0;
    msc_reset_state();
    if (cfg) {
        BDT[BD_EP(2)].epadr = USB_EPADR(BUF_EP2_OUT);
        BDT[BD_EP(3)].epadr = USB_EPADR(BUF_EP3_IN);
        EPCTL(2) = EPCTL_EPHSHK | EPCTL_EPRXEN | EPCTL_EPCTLDIS;
        EPCTL(3) = EPCTL_EPHSHK | EPCTL_EPTXEN | EPCTL_EPCTLDIS;
        ep2_arm();
    } else {
        EPCTL(2) = 0;
        EPCTL(3) = 0;
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
    configure(0);
    BDT[BD_EP0_IN].epadr = USB_EPADR(BUF_EP0_IN);
    BDT[BD_EP0_OUT].epadr = USB_EPADR(BUF_EP0_OUT);
    ep0_arm_setup();
    ctrl_state = CS_IDLE;
    EPCTL(0) = EPCTL_EPHSHK | EPCTL_EPTXEN | EPCTL_EPRXEN;
    ERRSTAT_CLR(0xFF);
    INTSTAT_CLR(0xFF);
    CTL &= (uint8_t)~CTL_TSUSPEND;
}

static uint8_t string_desc(uint8_t idx)
{
    char ser[17];
    const char *s;
    uint8_t n = 2;

    if (idx == 0) {
        ep0_scratch[0] = 4;
        ep0_scratch[1] = 0x03;
        ep0_scratch[2] = 0x09;
        ep0_scratch[3] = 0x04;
        return 4;
    }
    if (idx > 3)
        return 0;
    s = strings[idx - 1];
    if (!s) {
        serial_string(ser);
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
    if ((n != OUT_EP && n != IN_EP) || !usb_configuration)
        return 0;
    return &EPCTL(n);
}

static void handle_standard(void)
{
    uint8_t recip = sp_type & 0x1F;
    volatile uint8_t *epc;

    switch (sp_req) {
    case 0:                                         /* GET_STATUS */
        ep0_scratch[0] = 0;
        ep0_scratch[1] = 0;
        if (recip == 2 && (sp_index & 0x0F) != 0) {
            epc = ep_ctl_for((uint8_t)sp_index);
            if (!epc) { ep0_stall(); return; }
            if (*epc & EPCTL_EPSTALL)
                ep0_scratch[0] = 1;
        }
        ep0_send_data(ep0_scratch, 2);
        return;

    case 1:                                         /* CLEAR_FEATURE */
    case 3:                                         /* SET_FEATURE */
        if (recip == 2 && sp_value == 0) {          /* ENDPOINT_HALT */
            uint8_t ep = (uint8_t)sp_index;
            if ((ep & 0x0F) == 0) { ep0_send_status(); return; }
            epc = ep_ctl_for(ep);
            if (!epc) { ep0_stall(); return; }
            if (sp_req == 3) {
                *epc |= EPCTL_EPSTALL;
            } else {
                *epc &= (uint8_t)~EPCTL_EPSTALL;
                if ((ep & 0x0F) == OUT_EP) {
                    ep2_toggle = 0;
                    if (!rx_ready)
                        ep2_arm();
                } else {
                    ep3_toggle = 0;
                    BDT[BD_EP(3)].stat = 0;
                }
            }
            ep0_send_status();
            return;
        }
        if (recip == 0 && sp_value == 1) {
            ep0_send_status();
            return;
        }
        break;

    case 5:                                         /* SET_ADDRESS */
        pending_addr = 0x80 | (uint8_t)(sp_value & 0x7F);
        ep0_send_status();
        return;

    case 6: {                                       /* GET_DESCRIPTOR */
        uint8_t type = (uint8_t)(sp_value >> 8);
        if (type == 1) {
            ep0_send_data(device_desc, sizeof(device_desc));
            return;
        }
        if (type == 2) {
            ep0_send_data(config_desc, CONFIG_LEN);
            return;
        }
        if (type == 3) {
            uint8_t n = string_desc((uint8_t)sp_value);
            if (n) {
                ep0_send_data(ep0_scratch, n);
                return;
            }
        }
        break;
    }

    case 8:                                         /* GET_CONFIGURATION */
        ep0_scratch[0] = usb_configuration;
        ep0_send_data(ep0_scratch, 1);
        return;

    case 9:                                         /* SET_CONFIGURATION */
        if (sp_value <= 1) {
            configure((uint8_t)sp_value);
            ep0_send_status();
            return;
        }
        break;

    case 10:                                        /* GET_INTERFACE */
        if (usb_configuration && sp_index == 0) {
            ep0_scratch[0] = 0;
            ep0_send_data(ep0_scratch, 1);
            return;
        }
        break;

    case 11:                                        /* SET_INTERFACE */
        if (usb_configuration && sp_index == 0 && sp_value == 0) {
            ep0_send_status();
            return;
        }
        break;
    }
    ep0_stall();
}

static void handle_class(void)
{
    if (sp_req == 0xFE && sp_type == 0xA1) {        /* GET_MAX_LUN */
        ep0_scratch[0] = 0;
        ep0_send_data(ep0_scratch, 1);
        return;
    }
    if (sp_req == 0xFF && sp_type == 0x21) {        /* bulk-only mass storage reset */
        msc_reset_state();
        ep0_send_status();
        return;
    }
    ep0_stall();
}

static void handle_setup(void)
{
    volatile uint8_t *p = UBUF(BUF_EP0_OUT);

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
        if (ctrl_state == CS_DATA_IN) {
            if (ep0_remaining || ep0_zlp)
                ep0_send_chunk();
            else
                ctrl_state = CS_STATUS_OUT;
        } else if (ctrl_state == CS_STATUS_IN) {
            if (pending_addr) {
                ADDR = pending_addr & 0x7F;
                pending_addr = 0;
            }
            ctrl_state = CS_IDLE;
        }
        return;
    }
    if (BD_PID(BDT[BD_EP0_OUT].stat) == PID_SETUP) {
        handle_setup();
        return;
    }
    ctrl_state = CS_IDLE;
    ep0_arm_setup();
}

/* ------------------------------------------------------------------ */
/* Bulk-only transport + SCSI                                         */
/* ------------------------------------------------------------------ */

enum { BOT_CBW, BOT_DATA_IN, BOT_DATA_OUT, BOT_STALL_IN, BOT_CSW };

static uint8_t  bot_state;
static __xdata uint8_t cbw_tag[4];
static uint32_t host_len;           /* dCBWDataTransferLength */
static uint32_t done_len;           /* bytes transferred in the data phase */
static uint32_t phase_len;          /* bytes we transfer in the data phase */
static uint8_t  csw_status;
static uint8_t  is_read, is_write;  /* data phase from/to the disk */
static uint16_t xfer_lba, xfer_off;

static uint8_t  sense_key, sense_asc;
static uint8_t  ejected;
uint8_t msc_eject_request;          /* polled by boot_main */
uint8_t msc_activity;               /* set on every READ/WRITE */

static __xdata uint8_t sector[DISK_SECTOR];

static void msc_reset_state(void)
{
    bot_state = BOT_CBW;
    is_read = is_write = 0;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint16_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void fail(uint8_t key, uint8_t asc)
{
    sense_key = key;
    sense_asc = asc;
    csw_status = 1;
}

/* Data-in response of n bytes, already in sector[] */
static void reply(uint8_t n)
{
    phase_len = n < host_len ? n : host_len;
}

static void scsi_command(const uint8_t *cb, uint8_t dir_in)
{
    uint32_t lba;
    uint16_t blocks;

    switch (cb[0]) {
    case 0x00:                                      /* TEST UNIT READY */
        if (ejected)
            fail(0x02, 0x3A);                       /* not ready, medium not present */
        break;

    case 0x03:                                      /* REQUEST SENSE */
        mem_set(sector, 0, 18);
        sector[0] = 0x70;
        sector[2] = sense_key;
        sector[7] = 10;
        sector[12] = sense_asc;
        sense_key = sense_asc = 0;
        reply(18);
        break;

    case 0x12:                                      /* INQUIRY */
        if (cb[1] & 0x01) {                         /* EVPD not supported */
            fail(0x05, 0x24);
            break;
        }
        mem_set(sector, 0, 36);
        sector[1] = 0x80;                           /* removable */
        sector[2] = 0x02;
        sector[3] = 0x02;
        sector[4] = 31;
        mem_cpy(sector + 8, "RELAY   ", 8);
        mem_cpy(sector + 16, "Bootloader      ", 16);
        mem_cpy(sector + 32, "1.0 ", 4);
        reply(36);
        break;

    case 0x1A:                                      /* MODE SENSE (6) */
        mem_set(sector, 0, 4);
        sector[0] = 3;
        reply(4);
        break;

    case 0x5A:                                      /* MODE SENSE (10) */
        mem_set(sector, 0, 8);
        sector[1] = 6;
        reply(8);
        break;

    case 0x1B:                                      /* START STOP UNIT */
        if ((cb[4] & 0x03) == 0x02) {               /* eject */
            ejected = 1;
            msc_eject_request = 1;
        } else if (cb[4] & 0x01) {
            ejected = 0;
        }
        break;

    case 0x1E:                                      /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2F:                                      /* VERIFY (10) */
    case 0x35:                                      /* SYNCHRONIZE CACHE (10) */
        break;

    case 0x23:                                      /* READ FORMAT CAPACITIES */
        mem_set(sector, 0, 12);
        sector[3] = 8;
        put_be32(sector + 4, DISK_SECTORS);
        sector[8] = 0x02;                           /* formatted media */
        sector[10] = HI(DISK_SECTOR);
        reply(12);
        break;

    case 0x25:                                      /* READ CAPACITY (10) */
        put_be32(sector, DISK_SECTORS - 1);
        put_be32(sector + 4, DISK_SECTOR);
        reply(8);
        break;

    case 0x28:                                      /* READ (10) */
    case 0x2A:                                      /* WRITE (10) */
        lba = be32(cb + 2);
        blocks = ((uint16_t)cb[7] << 8) | cb[8];
        if (lba + blocks > DISK_SECTORS) {
            fail(0x05, 0x21);                       /* LBA out of range */
            break;
        }
        msc_activity = 1;                           /* data access, not just polling */
        xfer_lba = (uint16_t)lba;
        xfer_off = 0;
        phase_len = (uint32_t)blocks * DISK_SECTOR;
        if (phase_len > host_len)
            phase_len = host_len;
        if (cb[0] == 0x28)
            is_read = 1;
        else
            is_write = 1;
        break;

    default:
        fail(0x05, 0x20);                           /* invalid command */
        break;
    }
    (void)dir_in;
}

static void start_csw(void)
{
    bot_state = BOT_CSW;
}

static void handle_cbw(void)
{
    volatile uint8_t *p = UBUF(BUF_EP2_OUT);
    uint8_t cb[16], i, n = BDT[BD_EP(2)].cnt, dir_in;

    if (n != 31 || p[0] != 'U' || p[1] != 'S' || p[2] != 'B' || p[3] != 'C') {
        /* invalid CBW: stall both bulk endpoints until reset recovery */
        EPCTL(2) |= EPCTL_EPSTALL;
        EPCTL(3) |= EPCTL_EPSTALL;
        return;
    }
    for (i = 0; i < 4; i++)
        cbw_tag[i] = p[4 + i];
    host_len = ((uint32_t)p[11] << 24) | ((uint32_t)p[10] << 16) |
               ((uint16_t)p[9] << 8) | p[8];
    dir_in = p[12] & 0x80;
    for (i = 0; i < 16; i++)
        cb[i] = p[15 + i];

    csw_status = 0;
    done_len = 0;
    phase_len = 0;
    is_read = is_write = 0;
    scsi_command(cb, dir_in);

    if (host_len == 0) {
        start_csw();
    } else if (dir_in) {
        if (is_write) {                             /* direction mismatch */
            is_write = 0;
            phase_len = 0;
            csw_status = 2;
        }
        bot_state = BOT_DATA_IN;
    } else {
        if (is_read) {
            is_read = 0;
            phase_len = 0;
            csw_status = 2;
        }
        bot_state = BOT_DATA_OUT;                   /* extra data is discarded */
    }
}

/* Called from the main loop: advances the bulk-only state machine */
void msc_task(void)
{
    volatile uint8_t *buf;
    uint8_t n, i;

    if (!usb_configuration)
        return;

    switch (bot_state) {
    case BOT_CBW:
        if (rx_ready) {
            handle_cbw();
            rx_ready = 0;
            ep2_toggle ^= 1;
            ep2_arm();
        }
        break;

    case BOT_DATA_IN:
        if (BDT[BD_EP(3)].stat & BD_OWN)
            break;                                  /* previous packet still queued */
        if (done_len >= phase_len) {
            /* everything sent; host wants more: a short packet already ended
             * the transfer, otherwise stall so the host stops reading */
            if (done_len < host_len && (done_len % BULK_SIZE) == 0) {
                EPCTL(3) |= EPCTL_EPSTALL;
                bot_state = BOT_STALL_IN;
            } else {
                start_csw();
            }
            break;
        }
        n = (phase_len - done_len) > BULK_SIZE ? BULK_SIZE : (uint8_t)(phase_len - done_len);
        buf = UBUF(BUF_EP3_IN);
        if (is_read) {
            if (xfer_off == 0)
                disk_read(xfer_lba, sector);
            for (i = 0; i < n; i++)
                buf[i] = sector[xfer_off + i];
            xfer_off += n;
            if (xfer_off >= DISK_SECTOR) {
                xfer_off = 0;
                xfer_lba++;
            }
        } else {
            for (i = 0; i < n; i++)
                buf[i] = sector[(uint8_t)done_len + i];
        }
        done_len += n;
        BDT[BD_EP(3)].cnt = n;
        BDT[BD_EP(3)].stat = BD_OWN | BD_DTS | (ep3_toggle ? BD_DATA1 : 0);
        ep3_toggle ^= 1;
        break;

    case BOT_DATA_OUT:
        if (!rx_ready)
            break;
        n = BDT[BD_EP(2)].cnt;
        if (is_write && done_len < phase_len) {
            uint8_t k = n;
            if ((uint32_t)k > phase_len - done_len)
                k = (uint8_t)(phase_len - done_len);
            /* copy out of USB RAM, then hand to the disk */
            buf = UBUF(BUF_EP2_OUT);
            for (i = 0; i < k; i++)
                sector[i] = buf[i];
            disk_write(xfer_lba, xfer_off, sector, k);
            xfer_off += k;
            if (xfer_off >= DISK_SECTOR) {
                xfer_off = 0;
                xfer_lba++;
            }
        }
        done_len += n;
        rx_ready = 0;
        ep2_toggle ^= 1;
        if (done_len >= host_len || n < BULK_SIZE) {
            if (is_write)
                disk_write_end();
            start_csw();
        }
        ep2_arm();
        break;

    case BOT_STALL_IN:
        if (!(EPCTL(3) & EPCTL_EPSTALL))           /* host cleared the halt */
            start_csw();
        break;

    case BOT_CSW:
        if (BDT[BD_EP(3)].stat & BD_OWN)
            break;
        {
            uint32_t residue = host_len - done_len;
            if (is_write && done_len > phase_len)
                residue = host_len - phase_len;
            buf = UBUF(BUF_EP3_IN);
            buf[0] = 'U'; buf[1] = 'S'; buf[2] = 'B'; buf[3] = 'S';
            for (i = 0; i < 4; i++)
                buf[4 + i] = cbw_tag[i];
            buf[8] = (uint8_t)residue;
            buf[9] = (uint8_t)(residue >> 8);
            buf[10] = (uint8_t)(residue >> 16);
            buf[11] = (uint8_t)(residue >> 24);
            buf[12] = csw_status;
            BDT[BD_EP(3)].cnt = 13;
            BDT[BD_EP(3)].stat = BD_OWN | BD_DTS | (ep3_toggle ? BD_DATA1 : 0);
            ep3_toggle ^= 1;
            is_read = is_write = 0;
            bot_state = BOT_CBW;
        }
        break;
    }
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
    sense_key = sense_asc = 0;
    ejected = 0;
    msc_eject_request = 0;
    msc_activity = 0;
    usb_bus_reset();
    usb_active = 1;
    c = USBCTL0_USBPHYEN;
#if USB_INTERNAL_VREG
    c |= USBCTL0_USBVREN;
#endif
    USBCTL0 = c;
    CTL = CTL_USBEN;
    INTENB = 0;
    ERRENB = 0;
#if USB_INTERNAL_PULLUP
    c |= USBCTL0_USBPU;
#endif
    USBCTL0 = c;
}

void usb_poll(void)
{
    uint8_t s;

    if (!usb_active)
        return;
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
        uint8_t st = STAT;
        uint8_t ep = STAT_ENDP(st);
        INTSTAT_CLR(INT_TOKDNE);
        if (ep == 0)
            handle_ep0(st);
        else if (ep == OUT_EP && !(st & STAT_IN))
            rx_ready = 1;           /* processed and re-armed in msc_task() */
    }
}

uint8_t usb_is_configured(void)
{
    return usb_configuration != 0;
}

void usb_detach(void)
{
    USBCTL0 = 0;                    /* pull-up off: host sees a disconnect */
    CTL = 0;
}
