/*
 * c6_zb — what the bulb and the bench coordinator share: the APS receive logger, address and
 * hex helpers, a retrying commissioning timer, and the /zb status fields.
 *
 * The receive logger sits on APSDE-DATA.indication, below ZCL, so it sees every frame as it
 * arrived — group or unicast, ZDO or ZCL, with LQI/RSSI — before the stack interprets it. It
 * never consumes a frame itself; the bulb's tap may. One line per frame, prefixed "ZB-RX", so a log can be grepped.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "c6_zb.h"

static const char *TAG = "zb";

void c6_zb_eui64_str(const ezb_extaddr_t *a, char *out, size_t len)
{
    snprintf(out, len, "%016llX", (unsigned long long)a->u64);
}

void c6_zb_addr_str(const ezb_address_t *a, char *out, size_t len)
{
    switch (a->addr_mode) {
    case EZB_ADDR_MODE_GROUP:
        snprintf(out, len, "group:0x%04X", (unsigned)a->u.group_addr.group);
        break;
    case EZB_ADDR_MODE_SHORT:
        snprintf(out, len, a->u.short_addr >= 0xFFF8 ? "bcast:0x%04X" : "0x%04X",
                 (unsigned)a->u.short_addr);
        break;
    case EZB_ADDR_MODE_EXT:
        c6_zb_eui64_str(&a->u.extended_addr, out, len);
        break;
    default:
        snprintf(out, len, "none");
        break;
    }
}

int c6_zb_parse_hex(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (*hex) {
        if (*hex == ' ' || *hex == ':' || *hex == '-') {
            hex++;
            continue;
        }
        char byte[3] = {hex[0], hex[1], 0};
        char *end;
        if (!hex[1] || n >= max) {
            return -1;
        }
        out[n++] = (uint8_t)strtoul(byte, &end, 16);
        if (*end) {
            return -1;
        }
        hex += 2;
    }
    return (int)n;
}

bool c6_zb_parse_eui64(const char *hex, ezb_extaddr_t *out)
{
    uint8_t b[8];
    if (c6_zb_parse_hex(hex, b, sizeof b) != 8) {
        return false;
    }
    out->u64 = 0;
    for (int i = 0; i < 8; i++) {            /* written MSB first, as on a label */
        out->u64 = (out->u64 << 8) | b[i];
    }
    return true;
}

static void hex_str(const uint8_t *src, size_t n, char *out, size_t len)
{
    size_t o = 0, i = 0;
    for (; i < n && o + 4 < len - 2; i++) {    /* leave room for ".." and the NUL */
        o += snprintf(out + o, len - o, "%02X", src[i]);
    }
    if (i < n) {
        out[o++] = '.';
        out[o++] = '.';
    }
    out[o] = 0;
}

static volatile uint32_t s_rx_count, s_zcl_count;
static volatile int64_t s_rx_last_us;

void c6_zb_rx_stats(uint32_t *aps, uint32_t *zcl, int64_t *last_us)
{
    *aps = s_rx_count;
    *zcl = s_zcl_count;
    *last_us = s_rx_last_us;
}

static bool zcl_raw(const ezb_zcl_raw_frame_t *f)
{
    char src[24], dst[24], data[160];
    s_zcl_count++;
    c6_zb_addr_str(&f->header->src_addr, src, sizeof src);
    c6_zb_addr_str(&f->header->dst_addr, dst, sizeof dst);
    hex_str(f->payload, f->payload_length, data, sizeof data);
    ESP_LOGI(TAG, "ZB-ZCL src=%s dst=%s cluster=0x%04X fc=0x%02X manuf=0x%04X tsn=%u cmd=0x%02X payload=%s",
             src, dst, f->header->cluster_id, f->header->fc, f->header->manuf_code, f->header->tsn,
             f->header->cmd_id, data);
    return false;    /* the stack still handles it */
}

void c6_zb_install_zcl_logger(void)
{
    ezb_zcl_raw_command_handler_register(zcl_raw);
}

static c6_zb_rx_tap_t s_rx_tap;

void c6_zb_set_rx_tap(c6_zb_rx_tap_t tap)
{
    s_rx_tap = tap;
}

static bool apsde_rx(const ezb_apsde_data_ind_t *ind)
{
    s_rx_count++;
    s_rx_last_us = esp_timer_get_time();
    char src[24], dst[24], data[200];
    c6_zb_addr_str(&ind->src_address, src, sizeof src);
    c6_zb_addr_str(&ind->dst_address, dst, sizeof dst);
    hex_str(ind->asdu, ind->asdu_length, data, sizeof data);
    ESP_LOGI(TAG, "ZB-RX src=%s/%u dst=%s/%u profile=0x%04X cluster=0x%04X len=%u lqi=%u rssi=%d sec=%u status=%u asdu=%s",
             src, ind->src_endpoint, dst, ind->dst_endpoint, ind->profile_id, ind->cluster_id,
             ind->asdu_length, ind->lqi, ind->rssi, ind->security_status, ind->status, data);
    if (s_rx_tap && s_rx_tap(ind)) {
        return true;     /* the tap consumed it: the stack never sees it */
    }
    return false;    /* the stack still handles it */
}

void c6_zb_install_rx_logger(void)
{
    ezb_apsde_data_indication_handler_register(apsde_rx);
}

/* ---------------------------------------------------------------- commissioning */

ezb_err_t c6_zb_commission(uint8_t mode)
{
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    ESP_LOGI(TAG, "commissioning mode 0x%02X requested: %d", mode, err);
    return err;
}

static esp_timer_handle_t s_comm_timer;
static uint8_t s_comm_mode;

static void comm_cb(void *arg)
{
    if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        c6_zb_commission(s_comm_mode);
        esp_zigbee_lock_release();
    }
}

void c6_zb_commission_after(uint8_t mode, uint32_t ms)
{
    if (!s_comm_timer) {
        const esp_timer_create_args_t t = {.callback = comm_cb, .name = "zb_comm"};
        ESP_ERROR_CHECK(esp_timer_create(&t, &s_comm_timer));
    }
    esp_timer_stop(s_comm_timer);
    s_comm_mode = mode;
    esp_timer_start_once(s_comm_timer, (uint64_t)ms * 1000);
}

/* ---------------------------------------------------------------- status */

int c6_zb_status_json(char *out, size_t len)
{
    ezb_extaddr_t ieee;
    ezb_extpanid_t ext;
    char ieee_s[20], ext_s[20];
    ezb_get_extended_address(&ieee);
    ezb_nwk_get_extended_panid(&ext);
    c6_zb_eui64_str(&ieee, ieee_s, sizeof ieee_s);
    c6_zb_eui64_str(&ext, ext_s, sizeof ext_s);
    return snprintf(out, len,
                    "\"ieee\":\"%s\",\"joined\":%s,\"factory_new\":%s,\"short\":\"0x%04X\","
                    "\"pan\":\"0x%04X\",\"ext_pan\":\"%s\",\"channel\":%d,"
                    "\"bdb_mode\":\"0x%02X\",\"bdb_status\":%d,\"channel_mask\":\"0x%08lX\","
                    "\"nwk_frame_counter\":%lu",
                    ieee_s, ezb_bdb_dev_joined() ? "true" : "false",
                    ezb_bdb_is_factory_new() ? "true" : "false", (unsigned)ezb_get_short_address(),
                    (unsigned)ezb_nwk_get_panid(), ext_s, ezb_nwk_get_current_channel(),
                    (unsigned)ezb_bdb_get_commissioning_mode(), (int)ezb_bdb_get_commissioning_status(),
                    (unsigned long)ezb_bdb_get_primary_channel_set(), (unsigned long)ezb_nwk_get_frame_counter());
}
