/*
 * usb_boards.c - Discovers HCS08 relay boards on the USB host port (also
 * behind hubs) and talks to them with short open/command/close sessions.
 *
 * All USB work happens in one task (the client task); transfer callbacks are
 * delivered from usb_host_client_handle_events() in that same task, so the
 * session code can wait for transfers synchronously.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb_boards.h"

static const char *TAG = "boards";

#define GW_CLASS_CDC_DATA    0x0A
#define GW_CLASS_CDC_COMM    0x02
#define CDC_SET_CONTROL_LINE_STATE 0x22
#define MAX_SEEN_ADDR        32

typedef struct {
    bool          used;
    board_info_t  info;
    int64_t       last_ok_us;
} board_slot_t;

typedef struct {
    uint8_t         addr;
    char            cmd[64];
    char           *reply;
    size_t          reply_len;
    uint32_t        timeout_ms;
    esp_err_t       result;
    SemaphoreHandle_t done;
} cmd_req_t;

static usb_host_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmd_q;
static board_slot_t s_boards[BOARDS_MAX];
static uint8_t s_ignored[MAX_SEEN_ADDR];    /* addresses known to be no board */
static int s_num_ignored;
static volatile bool s_rescan = true;
static volatile int s_dev_count;
static usb_device_handle_t s_session_dev;   /* device of the running session */
static volatile bool s_session_gone;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static uint32_t ms_since(int64_t t_us)
{
    return (uint32_t)((esp_timer_get_time() - t_us) / 1000);
}

static void str_desc_to_ascii(const usb_str_desc_t *d, char *out, size_t len)
{
    size_t n = 0;
    if (d && d->bLength > 2) {
        int chars = (d->bLength - 2) / 2;
        for (int i = 0; i < chars && n + 1 < len; i++) {
            uint16_t c = d->wData[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = 0;
}

static board_slot_t *find_board(uint8_t addr)
{
    for (int i = 0; i < BOARDS_MAX; i++) {
        if (s_boards[i].used && s_boards[i].info.addr == addr) {
            return &s_boards[i];
        }
    }
    return NULL;
}

static bool is_ignored(uint8_t addr)
{
    for (int i = 0; i < s_num_ignored; i++) {
        if (s_ignored[i] == addr) {
            return true;
        }
    }
    return false;
}

static void set_error(uint8_t addr, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    board_slot_t *b = find_board(addr);
    if (b) {
        b->info.errors++;
        strlcpy(b->info.last_error, msg, sizeof(b->info.last_error));
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Client events and synchronous transfers                            */
/* ------------------------------------------------------------------ */

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_rescan = true;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl == s_session_dev) {
            s_session_gone = true;
        }
        s_rescan = true;
        break;
    default:
        break;
    }
}

static void xfer_cb(usb_transfer_t *xfer)
{
    *(volatile bool *)xfer->context = true;
}

/* Pumps client events until *done or timeout. If the device is unplugged,
 * pending transfers complete quickly with status NO_DEVICE. */
static bool wait_done(volatile bool *done, uint32_t timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!*done) {
        if (esp_timer_get_time() > end) {
            return *done;
        }
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
    }
    return true;
}

static esp_err_t ctrl_out(usb_device_handle_t dev, usb_transfer_t *x,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex)
{
    volatile bool done = false;
    usb_setup_packet_t *s = (usb_setup_packet_t *)x->data_buffer;
    s->bmRequestType = bmRequestType;
    s->bRequest = bRequest;
    s->wValue = wValue;
    s->wIndex = wIndex;
    s->wLength = 0;
    x->num_bytes = sizeof(usb_setup_packet_t);
    x->device_handle = dev;
    x->bEndpointAddress = 0;
    x->callback = xfer_cb;
    x->context = (void *)&done;
    x->timeout_ms = 1000;
    esp_err_t err = usb_host_transfer_submit_control(s_client, x);
    if (err != ESP_OK) {
        return err;
    }
    if (!wait_done(&done, 1000)) {
        return ESP_ERR_TIMEOUT;     /* control pipe recovers on next SETUP */
    }
    return x->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
}

static esp_err_t bulk_out(usb_device_handle_t dev, usb_transfer_t *x, uint8_t ep,
                          const void *data, int len)
{
    volatile bool done = false;
    memcpy(x->data_buffer, data, len);
    x->num_bytes = len;
    x->device_handle = dev;
    x->bEndpointAddress = ep;
    x->callback = xfer_cb;
    x->context = (void *)&done;
    esp_err_t err = usb_host_transfer_submit(x);
    if (err != ESP_OK) {
        return err;
    }
    if (!wait_done(&done, 1000)) {
        usb_host_endpoint_halt(dev, ep);
        usb_host_endpoint_flush(dev, ep);
        wait_done(&done, 200);
        usb_host_endpoint_clear(dev, ep);
        return ESP_ERR_TIMEOUT;
    }
    return x->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
}

/* Reads one packet (up to 64 bytes). Returns bytes read, 0 on timeout, <0 on error. */
static int bulk_in(usb_device_handle_t dev, usb_transfer_t *x, uint8_t ep,
                   uint8_t *buf, uint32_t timeout_ms)
{
    volatile bool done = false;
    x->num_bytes = 64;          /* multiple of MPS */
    x->device_handle = dev;
    x->bEndpointAddress = ep;
    x->callback = xfer_cb;
    x->context = (void *)&done;
    if (usb_host_transfer_submit(x) != ESP_OK) {
        return -1;
    }
    if (!wait_done(&done, timeout_ms)) {
        /* nothing arrived: cancel the pending transfer */
        usb_host_endpoint_halt(dev, ep);
        usb_host_endpoint_flush(dev, ep);
        wait_done(&done, 200);
        usb_host_endpoint_clear(dev, ep);
        return 0;
    }
    if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
        return -1;
    }
    memcpy(buf, x->data_buffer, x->actual_num_bytes);
    return x->actual_num_bytes;
}

/* ------------------------------------------------------------------ */
/* One command session                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t data_if, comm_if, ep_in, ep_out;
} cdc_eps_t;

static bool find_cdc_eps(usb_device_handle_t dev, cdc_eps_t *e)
{
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    bool have_data = false, have_comm = false;
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength,
                                                  USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
        if (intf->bAlternateSetting != 0) {
            continue;
        }
        if (intf->bInterfaceClass == GW_CLASS_CDC_COMM && !have_comm) {
            e->comm_if = intf->bInterfaceNumber;
            have_comm = true;
        } else if (intf->bInterfaceClass == GW_CLASS_CDC_DATA && !have_data) {
            e->data_if = intf->bInterfaceNumber;
            for (int i = 0; i < intf->bNumEndpoints; i++) {
                int eoff = off;
                const usb_ep_desc_t *ep =
                    usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &eoff);
                if (!ep || (ep->bmAttributes & 0x03) != USB_BM_ATTRIBUTES_XFER_BULK) {
                    continue;
                }
                if (ep->bEndpointAddress & 0x80) {
                    e->ep_in = ep->bEndpointAddress;
                } else {
                    e->ep_out = ep->bEndpointAddress;
                }
            }
            have_data = e->ep_in && e->ep_out;
        }
    }
    return have_data && have_comm;
}

static esp_err_t session(uint8_t addr, const char *cmd, char *reply, size_t reply_len,
                         uint32_t timeout_ms)
{
    usb_device_handle_t dev = NULL;
    usb_transfer_t *xc = NULL, *xo = NULL, *xi = NULL;
    cdc_eps_t e;
    bool claimed = false;
    esp_err_t err;
    uint8_t buf[64];
    size_t rn = 0;

    if (reply_len) {
        reply[0] = 0;
    }
    err = usb_host_device_open(s_client, addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    s_session_dev = dev;
    s_session_gone = false;

    if (!find_cdc_eps(dev, &e)) {
        err = ESP_ERR_NOT_FOUND;
        goto out;
    }
    if (usb_host_transfer_alloc(64, 0, &xc) != ESP_OK ||
        usb_host_transfer_alloc(64, 0, &xo) != ESP_OK ||
        usb_host_transfer_alloc(64, 0, &xi) != ESP_OK) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    err = usb_host_interface_claim(s_client, dev, e.data_if, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "addr %d: claim failed (%s) - out of USB host channels?",
                 addr, esp_err_to_name(err));
        goto out;
    }
    claimed = true;

    /* New host pipes start with DATA0: reset the board's data toggles too
     * (CLEAR_FEATURE ENDPOINT_HALT), then assert DTR so it sends replies. */
    ctrl_out(dev, xc, 0x02, USB_B_REQUEST_CLEAR_FEATURE, 0, e.ep_out);
    ctrl_out(dev, xc, 0x02, USB_B_REQUEST_CLEAR_FEATURE, 0, e.ep_in);
    err = ctrl_out(dev, xc, 0x21, CDC_SET_CONTROL_LINE_STATE, 0x0003, e.comm_if);
    if (err != ESP_OK) {
        goto out;
    }

    /* Discard stale output from an earlier session */
    for (int i = 0; i < 8 && bulk_in(dev, xi, e.ep_in, buf, 30) > 0; i++) {
    }

    char line[72];
    int len = snprintf(line, sizeof(line), "%s\n", cmd);
    err = bulk_out(dev, xo, e.ep_out, line, len);
    if (err != ESP_OK) {
        goto out;
    }

    /* Collect the first reply line */
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    err = ESP_ERR_TIMEOUT;
    while (esp_timer_get_time() < end && !s_session_gone) {
        uint32_t left = (uint32_t)((end - esp_timer_get_time()) / 1000) + 1;
        int n = bulk_in(dev, xi, e.ep_in, buf, left);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        bool eol = false;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                eol = true;
                break;
            }
            if (buf[i] != '\r' && rn + 1 < reply_len) {
                reply[rn++] = (char)buf[i];
            }
        }
        if (reply_len) {
            reply[rn] = 0;
        }
        if (eol) {
            err = ESP_OK;
            break;
        }
    }

out:
    if (s_session_gone) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (claimed) {
        /* must be released before closing, also if the device is gone */
        usb_host_interface_release(s_client, dev, e.data_if);
    }
    if (xc) usb_host_transfer_free(xc);
    if (xo) usb_host_transfer_free(xo);
    if (xi) usb_host_transfer_free(xi);
    usb_host_device_close(s_client, dev);
    s_session_dev = NULL;
    return err;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                          */
/* ------------------------------------------------------------------ */

static void probe_device(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        return;
    }
    const usb_device_desc_t *dd;
    usb_device_info_t info;
    bool is_board = false;
    board_info_t bi = { .addr = addr, .state = -1 };

    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK &&
        dd->idVendor == CONFIG_GW_USB_VID && dd->idProduct == CONFIG_GW_USB_PID &&
        usb_host_device_info(dev, &info) == ESP_OK) {
        is_board = true;
        str_desc_to_ascii(info.str_desc_serial_num, bi.serial, sizeof(bi.serial));
        str_desc_to_ascii(info.str_desc_product, bi.product, sizeof(bi.product));
        if (info.parent.dev_hdl) {
            usb_device_info_t pinfo;
            if (usb_host_device_info(info.parent.dev_hdl, &pinfo) == ESP_OK) {
                snprintf(bi.location, sizeof(bi.location), "hub%u.%u",
                         pinfo.dev_addr, info.parent.port_num);
            }
        } else {
            strcpy(bi.location, "root");
        }
    }
    usb_host_device_close(s_client, dev);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (is_board) {
        for (int i = 0; i < BOARDS_MAX; i++) {
            if (!s_boards[i].used) {
                s_boards[i].used = true;
                s_boards[i].info = bi;
                s_boards[i].last_ok_us = 0;
                ESP_LOGI(TAG, "board at addr %d, serial \"%s\", %s",
                         addr, bi.serial, bi.location);
                break;
            }
        }
    } else if (s_num_ignored < MAX_SEEN_ADDR) {
        s_ignored[s_num_ignored++] = addr;
    }
    xSemaphoreGive(s_lock);
}

static void rescan(void)
{
    uint8_t list[MAX_SEEN_ADDR];
    int n = 0;
    if (usb_host_device_addr_list_fill(MAX_SEEN_ADDR, list, &n) != ESP_OK) {
        return;
    }
    s_dev_count = n;

    /* forget boards / ignored addresses that disappeared */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BOARDS_MAX; i++) {
        if (!s_boards[i].used) {
            continue;
        }
        bool present = false;
        for (int k = 0; k < n; k++) {
            present |= list[k] == s_boards[i].info.addr;
        }
        if (!present) {
            ESP_LOGI(TAG, "board at addr %d gone", s_boards[i].info.addr);
            s_boards[i].used = false;
        }
    }
    for (int i = 0; i < s_num_ignored;) {
        bool present = false;
        for (int k = 0; k < n; k++) {
            present |= list[k] == s_ignored[i];
        }
        if (!present) {
            s_ignored[i] = s_ignored[--s_num_ignored];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_lock);

    for (int k = 0; k < n; k++) {
        if (!find_board(list[k]) && !is_ignored(list[k])) {
            probe_device(list[k]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Tasks                                                              */
/* ------------------------------------------------------------------ */

static void poll_board(board_slot_t *b)
{
    char reply[32];
    uint8_t addr = b->info.addr;
    esp_err_t err = session(addr, "GET", reply, sizeof(reply), 1500);
    unsigned v;
    if (err == ESP_OK && sscanf(reply, "STATE %x", &v) == 1) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        board_slot_t *bb = find_board(addr);
        if (bb) {
            bb->info.state = (int)v;
            bb->last_ok_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
    } else {
        set_error(addr, err == ESP_OK ? reply : esp_err_to_name(err));
    }
}

static void client_task(void *arg)
{
    usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_client));

    int next_poll = 0;
    int64_t last_scan = 0, last_poll = 0;

    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));

        int64_t now = esp_timer_get_time();
        if (s_rescan || now - last_scan > 2000000) {
            s_rescan = false;
            last_scan = now;
            rescan();
        }

        cmd_req_t *req;
        if (xQueueReceive(s_cmd_q, &req, 0) == pdTRUE) {
            req->result = find_board(req->addr)
                ? session(req->addr, req->cmd, req->reply, req->reply_len, req->timeout_ms)
                : ESP_ERR_NOT_FOUND;
            if (req->result != ESP_OK) {
                set_error(req->addr, esp_err_to_name(req->result));
            }
            xSemaphoreGive(req->done);
            continue;
        }

        /* round-robin status poll: each board every CONFIG_GW_POLL_INTERVAL_MS */
        int count = 0;
        for (int i = 0; i < BOARDS_MAX; i++) {
            count += s_boards[i].used;
        }
        if (count && now - last_poll > (int64_t)CONFIG_GW_POLL_INTERVAL_MS * 1000 / count) {
            last_poll = now;
            for (int k = 0; k < BOARDS_MAX; k++) {
                next_poll = (next_poll + 1) % BOARDS_MAX;
                if (s_boards[next_poll].used) {
                    poll_board(&s_boards[next_poll]);
                    break;
                }
            }
        }
    }
}

static void host_lib_task(void *arg)
{
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void usb_boards_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(4, sizeof(cmd_req_t *));
    xTaskCreatePinnedToCore(host_lib_task, "usb_lib", 4096,
                            xTaskGetCurrentTaskHandle(), 6, NULL, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xTaskCreatePinnedToCore(client_task, "usb_boards", 6144, NULL, 5, NULL, 0);
}

int usb_boards_snapshot(board_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BOARDS_MAX && n < max; i++) {
        if (s_boards[i].used) {
            out[n] = s_boards[i].info;
            out[n].age_ms = s_boards[i].last_ok_us ? ms_since(s_boards[i].last_ok_us) : UINT32_MAX;
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t usb_boards_command(uint8_t addr, const char *cmd,
                             char *reply, size_t reply_len, uint32_t timeout_ms)
{
    cmd_req_t req = {
        .addr = addr,
        .reply = reply,
        .reply_len = reply_len,
        .timeout_ms = timeout_ms,
        .done = xSemaphoreCreateBinary(),
    };
    if (!req.done) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(req.cmd, cmd, sizeof(req.cmd));
    cmd_req_t *p = &req;
    xQueueSend(s_cmd_q, &p, portMAX_DELAY);
    /* the USB task always completes a request within its own timeouts */
    xSemaphoreTake(req.done, portMAX_DELAY);
    vSemaphoreDelete(req.done);

    if (req.result == ESP_OK && strncmp(reply, "STATE ", 6) == 0) {
        unsigned v;
        if (sscanf(reply, "STATE %x", &v) == 1) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            board_slot_t *b = find_board(addr);
            if (b) {
                b->info.state = (int)v;
                b->last_ok_us = esp_timer_get_time();
            }
            xSemaphoreGive(s_lock);
        }
    }
    return req.result;
}

int usb_boards_device_count(void)
{
    return s_dev_count;
}
