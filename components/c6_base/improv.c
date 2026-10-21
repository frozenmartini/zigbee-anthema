/*
 * improv — Improv Wi-Fi over the NanoC6's own USB (USB-Serial-JTAG).
 *
 * The frame, as https://www.improv-wifi.com/serial/ gives it: "IMPROV" · version 1 · type · length ·
 * data · checksum (the sum of every byte before it, mod 256; the page does not state it). Each frame
 * this side sends is followed by '\n'. Types: 1 current state, 2 error state, 3 RPC command, 4 RPC
 * result. Only RPC commands are read; every other byte on the port (log text, a client's newline, a
 * bad frame) is ignored. Commands 1-4 are served; any other answers error 2 ("unknown command").
 *
 * The password from command 1 is never logged or echoed; every buffer that held it is wiped.
 * Improv is optional: a driver that does not install logs once and the hotspot remains.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "c6_base.h"
#include "c6_wifi.h"

static const char *TAG = "improv";

#define IMPROV_VERSION     1
#define IMPROV_HDR_LEN     9                 /* "IMPROV", version, type, length */
#define IMPROV_DATA_MAX    128               /* a command 1 is at most 2 + 1 + 32 + 1 + 64 = 100 */
#define IMPROV_RAW_MAX     (IMPROV_HDR_LEN + IMPROV_DATA_MAX + 1)
#define IMPROV_OUT_MAX     (IMPROV_HDR_LEN + 255 + 2)   /* the largest frame sent, checksum and '\n' */
#define IMPROV_DRV_BUF     512               /* the driver's rx and tx buffers */
#define IMPROV_READ_MS     100
#define IMPROV_CONNECT_MS  30000
#define IMPROV_STACK       6144              /* as the HTTP task, which runs the same c6_wifi_set and scan */

#define IMPROV_T_STATE     1
#define IMPROV_T_ERROR     2
#define IMPROV_T_RPC       3
#define IMPROV_T_RESULT    4

#define IMPROV_ST_READY        2
#define IMPROV_ST_PROVISIONING 3
#define IMPROV_ST_PROVISIONED  4

#define IMPROV_E_NONE          0
#define IMPROV_E_INVALID_RPC   1
#define IMPROV_E_UNKNOWN_CMD   2
#define IMPROV_E_NO_CONNECT    3

#define IMPROV_CMD_WIFI    1
#define IMPROV_CMD_STATE   2
#define IMPROV_CMD_INFO    3
#define IMPROV_CMD_SCAN    4

typedef struct {
    uint8_t raw[IMPROV_RAW_MAX];             /* the frame being collected, from its first 'I' */
    size_t n;
} improv_parser_t;

static improv_parser_t s_improv;
static bool s_improv_session;                /* an RPC command has arrived since boot */
static int s_improv_sent = -1;               /* the last state sent */
static bool s_improv_provisioning;           /* command 1 accepted, waiting for an IP */
static int64_t s_improv_deadline_us;

/* ------------------------------------------------------------- frames out */

/* A store the compiler cannot drop: for buffers that held the password. */
static void improv_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n > 0) {
        *v++ = 0;
        n--;
    }
}

static uint8_t improv_sum(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

/* One frame into out: header, version, type, length, data, checksum, '\n'. Its size; 0 if it does not fit. */
static size_t improv_frame(uint8_t *out, size_t cap, uint8_t type, const uint8_t *data, size_t len)
{
    if (len > 255 || cap < IMPROV_HDR_LEN + len + 2) {
        return 0;
    }
    memcpy(out, "IMPROV", 6);
    out[6] = IMPROV_VERSION;
    out[7] = type;
    out[8] = (uint8_t)len;
    if (len > 0) {
        memcpy(out + IMPROV_HDR_LEN, data, len);
    }
    out[IMPROV_HDR_LEN + len] = improv_sum(out, IMPROV_HDR_LEN + len);
    out[IMPROV_HDR_LEN + len + 1] = '\n';
    return IMPROV_HDR_LEN + len + 2;
}

/* An RPC result's data: the command answered, the data length, then each string length-prefixed.
 * Its size; 0 if the strings do not fit in cap (at most 255, the frame's length byte). */
static size_t improv_result_data(uint8_t *out, size_t cap, uint8_t cmd, const char *const *strs, int count)
{
    if (cap < 2 || cap > 255) {
        return 0;
    }
    size_t n = 2;
    for (int i = 0; i < count; i++) {
        size_t l = strlen(strs[i]);
        if (l > cap || n + 1 + l > cap) {
            return 0;
        }
        out[n++] = (uint8_t)l;
        memcpy(out + n, strs[i], l);
        n += l;
    }
    out[0] = cmd;
    out[1] = (uint8_t)(n - 2);
    return n;
}

static void improv_send(uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t out[IMPROV_OUT_MAX];
    size_t n = improv_frame(out, sizeof out, type, data, len);
    if (n > 0) {
        usb_serial_jtag_write_bytes(out, n, pdMS_TO_TICKS(IMPROV_READ_MS));
    }
}

static void improv_send_state(uint8_t st)
{
    improv_send(IMPROV_T_STATE, &st, 1);
    s_improv_sent = st;
}

static void improv_send_error(uint8_t e)
{
    improv_send(IMPROV_T_ERROR, &e, 1);
}

static void improv_send_result(uint8_t cmd, const char *const *strs, int count)
{
    uint8_t d[255];
    size_t n = improv_result_data(d, sizeof d, cmd, strs, count);
    if (n == 0) {
        ESP_LOGW(TAG, "IMPROV result for command %d does not fit a frame; not sent", (int)cmd);
        return;
    }
    improv_send(IMPROV_T_RESULT, d, n);
}

/* ---------------------------------------------------------------- commands */

/* 3 while a command 1 waits for its IP; else 4 when the station has an IP, 2 when it has not. */
static uint8_t improv_state_now(void)
{
    if (s_improv_provisioning) {
        return IMPROV_ST_PROVISIONING;
    }
    return c6_wifi_state() == C6_WIFI_CONNECTED ? IMPROV_ST_PROVISIONED : IMPROV_ST_READY;
}

/* The two addresses the board answers on; the first is where the client sends the user. */
static void improv_send_urls(uint8_t cmd)
{
    char local[40];
    char ip[32];
    esp_netif_ip_info_t info = {0};
    esp_netif_get_ip_info(c6_wifi_netif(), &info);
    snprintf(local, sizeof local, "http://%s.local/", c6_base_host());
    snprintf(ip, sizeof ip, "http://" IPSTR "/", IP2STR(&info.ip));
    const char *strs[] = {local, ip};
    improv_send_result(cmd, strs, 2);
}

/* Command 1: d is the command's data, ssid length · ssid · password length · password. */
static void improv_wifi_settings(const uint8_t *d, size_t len)
{
    if (len < 2 || (size_t)d[0] + 2 > len || (size_t)d[0] + 2 + d[1 + d[0]] != len) {
        improv_send_error(IMPROV_E_INVALID_RPC);
        return;
    }
    size_t sl = d[0];
    size_t pl = d[1 + sl];
    const uint8_t *sp = d + 1;
    const uint8_t *pp = d + 2 + sl;
    char ssid[33];
    char pass[65];
    if (sl >= sizeof ssid || pl >= sizeof pass || memchr(sp, 0, sl) != NULL || memchr(pp, 0, pl) != NULL) {
        ESP_LOGW(TAG, "IMPROV Wi-Fi settings refused: a length or a byte is out of range");
        improv_send_error(IMPROV_E_NO_CONNECT);
        return;
    }
    memcpy(ssid, sp, sl);
    ssid[sl] = 0;
    memcpy(pass, pp, pl);
    pass[pl] = 0;
    esp_err_t err = c6_wifi_set(ssid, pass);
    improv_wipe(pass, sizeof pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMPROV Wi-Fi settings refused: %s", esp_err_to_name(err));
        improv_send_error(IMPROV_E_NO_CONNECT);
        return;
    }
    ESP_LOGI(TAG, "IMPROV Wi-Fi settings stored; connecting to %s", ssid);
    s_improv_provisioning = true;
    s_improv_deadline_us = esp_timer_get_time() + (int64_t)IMPROV_CONNECT_MS * 1000;
    improv_send_state(IMPROV_ST_PROVISIONING);
}

static void improv_send_info(void)
{
    char name[64];
    c6_base_name(name, sizeof name);
    const esp_app_desc_t *app = esp_app_get_description();
    const char *strs[] = {app->project_name, app->version, "ESP32-C6", name};
    improv_send_result(IMPROV_CMD_INFO, strs, 4);
}

static void improv_scan_cb(const char *ssid, int rssi, bool secured, void *ctx)
{
    (void)ctx;
    char r[8];
    snprintf(r, sizeof r, "%d", rssi);
    const char *strs[] = {ssid, r, secured ? "YES" : "NO"};
    improv_send_result(IMPROV_CMD_SCAN, strs, 3);
}

/* One result per network, then the empty one that ends the list (also when the scan fails). */
static void improv_send_scan(void)
{
    esp_err_t err = c6_wifi_scan(improv_scan_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMPROV scan: %s", esp_err_to_name(err));
    }
    improv_send_result(IMPROV_CMD_SCAN, NULL, 0);
}

/* A whole, checksummed frame. Only an RPC command is acted on. */
static void improv_handle(uint8_t type, const uint8_t *data, size_t len)
{
    if (type != IMPROV_T_RPC) {
        return;
    }
    s_improv_session = true;
    if (len < 2 || (size_t)data[1] + 2 != len) {
        improv_send_error(IMPROV_E_INVALID_RPC);
        return;
    }
    improv_send_error(IMPROV_E_NONE);
    switch (data[0]) {
    case IMPROV_CMD_WIFI:
        improv_wifi_settings(data + 2, data[1]);
        break;
    case IMPROV_CMD_STATE:
        improv_send_state(improv_state_now());
        if (s_improv_sent == IMPROV_ST_PROVISIONED) {
            improv_send_urls(IMPROV_CMD_STATE);
        }
        break;
    case IMPROV_CMD_INFO:
        improv_send_info();
        break;
    case IMPROV_CMD_SCAN:
        improv_send_scan();
        break;
    default:
        improv_send_error(IMPROV_E_UNKNOWN_CMD);
        break;
    }
}

/* ------------------------------------------------------------------ parser */

/* Drop the first k pending bytes, wiping the bytes they leave behind. */
static void improv_drop(improv_parser_t *p, size_t k)
{
    if (k > p->n) {
        k = p->n;
    }
    memmove(p->raw, p->raw + k, p->n - k);
    improv_wipe(p->raw + p->n - k, k);
    p->n -= k;
}

/* Act on what is pending. A complete valid frame is handled and removed; a byte that cannot start a
 * frame, a version other than 1, a length past the buffer or a bad checksum drops one byte, so a
 * frame that begins inside a bad one is still found. Otherwise wait for more bytes. */
static void improv_parse(improv_parser_t *p)
{
    for (;;) {
        size_t h = p->n < 6 ? p->n : 6;
        if (h == 0) {
            return;
        }
        if (memcmp(p->raw, "IMPROV", h) != 0) {
            improv_drop(p, 1);
            continue;
        }
        if (p->n < IMPROV_HDR_LEN) {
            return;
        }
        size_t len = p->raw[8];
        if (p->raw[6] != IMPROV_VERSION || len > IMPROV_DATA_MAX) {
            improv_drop(p, 1);
            continue;
        }
        if (p->n < IMPROV_HDR_LEN + len + 1) {
            return;
        }
        if (improv_sum(p->raw, IMPROV_HDR_LEN + len) != p->raw[IMPROV_HDR_LEN + len]) {
            improv_drop(p, 1);
            continue;
        }
        improv_handle(p->raw[7], p->raw + IMPROV_HDR_LEN, len);
        improv_drop(p, IMPROV_HDR_LEN + len + 1);
    }
}

/* One byte from the port. */
static void improv_feed(improv_parser_t *p, uint8_t b)
{
    if (p->n >= sizeof p->raw) {
        improv_drop(p, 1);                   /* not reached: a pending frame is never longer than raw */
    }
    p->raw[p->n++] = b;
    improv_parse(p);
}

/* The port was quiet for a whole read: a frame left unfinished is abandoned, and any frame that
 * begins inside it is still parsed. */
static void improv_idle(improv_parser_t *p)
{
    while (p->n > 0) {
        improv_drop(p, 1);
        improv_parse(p);
    }
}

/* -------------------------------------------------------------------- task */

/* After each read: a command 1 that got its IP (state 4 and the URLs) or ran out of time (error 3,
 * state 2); otherwise, once a session has begun, a state that changed is sent. */
static void improv_poll(void)
{
    if (s_improv_provisioning) {
        if (c6_wifi_state() == C6_WIFI_CONNECTED) {
            s_improv_provisioning = false;
            ESP_LOGI(TAG, "IMPROV provisioned");
            improv_send_state(IMPROV_ST_PROVISIONED);
            improv_send_urls(IMPROV_CMD_WIFI);
        } else if (esp_timer_get_time() >= s_improv_deadline_us) {
            s_improv_provisioning = false;
            ESP_LOGW(TAG, "IMPROV no address within %d s", IMPROV_CONNECT_MS / 1000);
            improv_send_error(IMPROV_E_NO_CONNECT);
            improv_send_state(IMPROV_ST_READY);
        }
        return;
    }
    if (s_improv_session) {
        uint8_t st = improv_state_now();
        if (st != s_improv_sent) {
            improv_send_state(st);
        }
    }
}

static void improv_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    improv_send_state(improv_state_now());
    for (;;) {
        int got = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(IMPROV_READ_MS));
        if (got > 0) {
            for (int i = 0; i < got; i++) {
                improv_feed(&s_improv, buf[i]);
            }
            improv_wipe(buf, (size_t)got);
        } else {
            improv_idle(&s_improv);
        }
        improv_poll();
    }
}

void improv_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = IMPROV_DRV_BUF,
        .rx_buffer_size = IMPROV_DRV_BUF,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMPROV off: the USB serial driver did not install (%s); the hotspot remains",
                 esp_err_to_name(err));
        return;
    }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    usb_serial_jtag_vfs_use_driver();        /* the console shares the port: logs go through the driver */
#endif
    if (xTaskCreate(improv_task, "improv", IMPROV_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "IMPROV off: task not created");
    }
}
