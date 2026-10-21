/*
 * coord — a board as the bench Zigbee controller: a stand-in for a real one, so the bulb's join
 * and command handling can be proven without touching a real Zigbee controller or the home network.
 *
 * It forms its own network on CONFIG_C6_ZB_CHANNEL (25: clear of channel 11, where many Zigbee2MQTT and
 * other Zigbee networks sit) with a fixed extended PAN ID the bulb is told to look for.
 * The network stays closed until POST /zb/permit opens it. With CONFIG_C6_ZB_REQUIRE_IC only a
 * joiner whose install code it holds can get the network key — the "secured" path of a real Zigbee controller.
 *
 * Commands go out through APSDE-DATA.request as raw ZCL, so /zb/send can replay any frame
 * byte for byte; /zb/cmd builds the common light commands on the same path. Everything
 * received is logged as ZB-RX (c6_zb), everything sent as ZB-TX.
 *
 * HTTP, on top of c6_base:
 *   GET  /zb                                   network, and the devices that have announced
 *   POST /zb/permit?s=N                        open joining for N s (0 closes)          (token)
 *   POST /zb/form                              start network formation again            (token)
 *   POST /zb/cmd?dst=&ep=&op=&v=&v2=&t=        op: on off toggle level ct xy            (token)
 *   POST /zb/send?dst=&ep=&cluster=&zcl=HEX    one raw APS payload: a ZCL frame, header (token)
 *        [&profile=0x0000]                      included; profile=0x0000 (ep 0) sends ZDO
 *   POST /zb/ic?eui=&ic=                       hold a joiner's install code (36 hex)    (token)
 *   POST /zb/policy?ic_required=0|1&tclk_required=0|1   admit legacy (no-IC, HA 1.2) joiners (token)
 *        dst: 0x1234 (short) or group:0x0001; ep defaults to 1; t is tenths of a second
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "c6_base.h"
#include "c6_zb.h"

static const char *TAG = "coord";

#define EP          1
#define HA_PROFILE  0x0104
#define MAX_DEVICES 8

static volatile bool s_zb_up;     /* the Zigbee lock exists only after esp_zigbee_init() */
static uint8_t s_seq;

static struct {
    uint16_t short_addr;
    ezb_extaddr_t ieee;
    int64_t seen_us;
} s_dev[MAX_DEVICES];

static void remember(uint16_t short_addr, const ezb_extaddr_t *ieee)
{
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_dev[i].ieee.u64 == ieee->u64 || (slot < 0 && s_dev[i].ieee.u64 == 0)) {
            slot = i;
            if (s_dev[i].ieee.u64 == ieee->u64) {
                break;
            }
        }
    }
    if (slot >= 0) {
        s_dev[slot].short_addr = short_addr;
        s_dev[slot].ieee = *ieee;
        s_dev[slot].seen_us = esp_timer_get_time();
    }
}

/* ---------------------------------------------------------------- signals */

static bool on_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(sig);
    const void *params = ezb_app_signal_get_params(sig);
    char ieee[20];

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        c6_zb_commission(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t st = *(const ezb_bdb_comm_status_t *)params;
        if (st != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed (0x%02X); retrying in 5 s", ezb_app_signal_to_string(type), st);
            c6_zb_commission_after(EZB_BDB_MODE_INITIALIZATION, 5000);
        } else if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "factory new: forming the bench network");
            c6_zb_commission(EZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            ESP_LOGI(TAG, "network resumed: PAN 0x%04X channel %d (closed)", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel());
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t st = *(const ezb_bdb_comm_status_t *)params;
        if (st == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext;
            ezb_nwk_get_extended_panid(&ext);
            ESP_LOGI(TAG, "formed: PAN 0x%04X ext %016llX channel %d (closed until /zb/permit)",
                     ezb_nwk_get_panid(), (unsigned long long)ext.u64, ezb_nwk_get_current_channel());
        } else {
            ESP_LOGW(TAG, "formation failed (0x%02X); retrying in 5 s", st);
            c6_zb_commission_after(EZB_BDB_MODE_NETWORK_FORMATION, 5000);
        }
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        ESP_LOGI(TAG, "permit join: %u s", *(const uint8_t *)params);
        break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *p = params;
        c6_zb_eui64_str(&p->device_addr, ieee, sizeof ieee);
        ESP_LOGI(TAG, "device announced: short 0x%04X ieee %s capability 0x%02X", p->short_addr, ieee,
                 p->capability);
        remember(p->short_addr, &p->device_addr);
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *p = params;
        c6_zb_eui64_str(&p->device_addr, ieee, sizeof ieee);
        ESP_LOGI(TAG, "device authorization: short 0x%04X ieee %s type %u status %u", p->short_addr, ieee,
                 p->type, p->status);
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        /* status: 0 secure rejoin, 1 unsecure join, 2 left, 3 TC rejoin; tc_action: 0 accept, 1 deny, 2 ignore */
        const ezb_zdo_signal_device_update_params_t *p = params;
        c6_zb_eui64_str(&p->device_addr, ieee, sizeof ieee);
        ESP_LOGI(TAG, "device update: short 0x%04X ieee %s status %u tc_action %u parent 0x%04X", p->short_addr,
                 ieee, p->status, p->tc_action, p->parent_short);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *p = params;
        c6_zb_eui64_str(&p->device_addr, ieee, sizeof ieee);
        ESP_LOGI(TAG, "device left: short 0x%04X ieee %s type %u", p->short_addr, ieee, p->leave_type);
    } break;
    default:
        ESP_LOGI(TAG, "signal %s (0x%04X)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

/* ---------------------------------------------------------------- Zigbee task */

static void zb_task(void *arg)
{
    esp_zigbee_config_t cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
            .install_code_policy = CONFIG_C6_ZB_REQUIRE_IC,
            .zczr_config = {.max_children = 10},
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
    s_zb_up = true;

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(1UL << CONFIG_C6_ZB_CHANNEL));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(1UL << CONFIG_C6_ZB_CHANNEL));
    ezb_extpanid_t ext;
    ESP_ERROR_CHECK(c6_zb_parse_eui64(CONFIG_C6_ZB_EXT_PANID, &ext) ? ESP_OK : ESP_ERR_INVALID_ARG);
    ezb_set_use_extended_panid(&ext);

    if (CONFIG_C6_ZB_JOINER_EUI64[0] && CONFIG_C6_ZB_JOINER_IC[0]) {
        ezb_extaddr_t joiner;
        uint8_t ic[18];
        ESP_ERROR_CHECK(c6_zb_parse_eui64(CONFIG_C6_ZB_JOINER_EUI64, &joiner) &&
                                c6_zb_parse_hex(CONFIG_C6_ZB_JOINER_IC, ic, sizeof ic) == 18
                            ? ESP_OK
                            : ESP_ERR_INVALID_ARG);
        ESP_ERROR_CHECK(ezb_secur_ic_add(&joiner, EZB_SECUR_IC_TYPE_128, ic));
        ESP_LOGI(TAG, "install code held for %s (CRC %02X%02X); required: %s", CONFIG_C6_ZB_JOINER_EUI64,
                 ic[16], ic[17], CONFIG_C6_ZB_REQUIRE_IC ? "yes" : "no");
    }
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(on_signal));
    c6_zb_install_rx_logger();

    static char manuf[] = "\x09" "ESPRESSIF", model[] = "\x0E" "c6-bench-coord";
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_zha_color_dimmer_switch_config_t sw = EZB_ZHA_COLOR_DIMMER_SWITCH_CONFIG();
    ezb_af_ep_desc_t ep = ezb_zha_create_color_dimmer_switch(EP, &sw);
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC,
                                                                    EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manuf);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev));

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- sending */

static bool parse_dst(const char *s, ezb_address_t *dst)
{
    bool group = strncmp(s, "group:", 6) == 0;
    char *end;
    unsigned long v = strtoul(group ? s + 6 : s, &end, 0);
    if (*end || v > 0xFFFF) {
        return false;
    }
    dst->addr_mode = group ? EZB_ADDR_MODE_GROUP : EZB_ADDR_MODE_SHORT;
    if (group) {
        dst->u.group_addr.group = v;
        dst->u.group_addr.bcast = 0xFFFD;    /* group frames go out as a broadcast to rx-on devices */
    } else {
        dst->u.short_addr = v;
    }
    return true;
}

/* Caller holds the Zigbee lock. The frame is copied to a static buffer in case the stack
 * reads it after returning. */
static ezb_err_t send_zcl(const ezb_address_t *dst, uint8_t ep, uint16_t cluster, const uint8_t *zcl, size_t len,
                          uint16_t profile)
{
    static uint8_t frame[96];
    memcpy(frame, zcl, len);
    ezb_apsde_data_req_t req = {
        .dst_address = *dst,
        .src_endpoint = profile == 0 ? 0 : EP,     /* ZDO lives on endpoint 0 */
        .dst_endpoint = ep,
        .cluster_id = cluster,
        .profile_id = profile,
        .radius = 30,
        .tx_options = dst->addr_mode == EZB_ADDR_MODE_GROUP ? 0 : EZB_APSDE_TX_OPT_ACK_TX,
        .asdu_length = len,
        .asdu = frame,
    };
    ezb_err_t err = ezb_apsde_data_request(&req);
    char d[24], hex[2 * sizeof frame + 1];
    c6_zb_addr_str(dst, d, sizeof d);
    for (size_t i = 0; i < len; i++) {
        snprintf(hex + 2 * i, 3, "%02X", zcl[i]);
    }
    hex[2 * len] = 0;
    ESP_LOGI(TAG, "ZB-TX dst=%s/%u profile=0x%04X cluster=0x%04X asdu=%s err=%d", d, ep, profile, cluster, hex, err);
    return err;
}

/* /zb/cmd and /zb/send share everything after the frame is built */
static esp_err_t send_reply(httpd_req_t *req, const ezb_address_t *dst, uint8_t ep, uint16_t cluster,
                            const uint8_t *zcl, size_t len, uint16_t profile)
{
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_err_t err = send_zcl(dst, ep, cluster, zcl, len, profile);
    esp_zigbee_lock_release();
    char msg[48];
    snprintf(msg, sizeof msg, err == EZB_ERR_NONE ? "ok: sent, seq %u\n" : "send failed: %d\n",
             err == EZB_ERR_NONE ? zcl[1] : err);
    return httpd_resp_sendstr(req, msg);
}

static bool common_args(httpd_req_t *req, ezb_address_t *dst, uint8_t *ep)
{
    char v[24];
    if (!c6_base_query(req, "dst", v, sizeof v) || !parse_dst(v, dst)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dst= 0x1234 or group:0x0001");
        return false;
    }
    *ep = c6_base_query(req, "ep", v, sizeof v) ? (uint8_t)strtoul(v, NULL, 0) : 1;
    return true;
}

static uint32_t arg_u(httpd_req_t *req, const char *key, uint32_t dflt)
{
    char v[16];
    return c6_base_query(req, key, v, sizeof v) ? strtoul(v, NULL, 0) : dflt;
}

static esp_err_t cmd_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    ezb_address_t dst;
    uint8_t ep;
    if (!common_args(req, &dst, &ep)) {
        return ESP_OK;
    }
    char op[12];
    if (!c6_base_query(req, "op", op, sizeof op)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "op= on off toggle level ct xy");
    }
    uint32_t v = arg_u(req, "v", 0), v2 = arg_u(req, "v2", 0), t = arg_u(req, "t", 0);
    /* frame control 0x01: cluster-specific, client to server, default response wanted */
    uint8_t f[10] = {0x01, s_seq++};
    size_t n;
    uint16_t cluster;
    if (!strcmp(op, "on") || !strcmp(op, "off") || !strcmp(op, "toggle")) {
        cluster = 0x0006;
        f[2] = op[1] == 'n' ? 0x01 : op[1] == 'f' ? 0x00 : 0x02;
        n = 3;
    } else if (!strcmp(op, "level")) {          /* MoveToLevelWithOnOff */
        cluster = 0x0008;
        uint8_t b[] = {0x04, v, t, t >> 8};
        memcpy(f + 2, b, sizeof b);
        n = 2 + sizeof b;
    } else if (!strcmp(op, "ct")) {             /* MoveToColorTemperature, v = mireds */
        cluster = 0x0300;
        uint8_t b[] = {0x0A, v, v >> 8, t, t >> 8};
        memcpy(f + 2, b, sizeof b);
        n = 2 + sizeof b;
    } else if (!strcmp(op, "xy")) {             /* MoveToColor, v = x, v2 = y */
        cluster = 0x0300;
        uint8_t b[] = {0x07, v, v >> 8, v2, v2 >> 8, t, t >> 8};
        memcpy(f + 2, b, sizeof b);
        n = 2 + sizeof b;
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "op= on off toggle level ct xy");
    }
    return send_reply(req, &dst, ep, cluster, f, n, HA_PROFILE);
}

static esp_err_t send_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    ezb_address_t dst;
    uint8_t ep;
    if (!common_args(req, &dst, &ep)) {
        return ESP_OK;
    }
    char hex[130];
    uint8_t zcl[64];
    int n;
    if (!c6_base_query(req, "zcl", hex, sizeof hex) || (n = c6_zb_parse_hex(hex, zcl, sizeof zcl)) < 3) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zcl= the whole frame in hex, header included");
    }
    uint16_t profile = arg_u(req, "profile", HA_PROFILE);
    if (profile == 0 && !c6_base_query(req, "ep", hex, sizeof hex)) {
        ep = 0;
    }
    return send_reply(req, &dst, ep, arg_u(req, "cluster", 0), zcl, n, profile);
}

static esp_err_t ic_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    char eui_s[24], ic_s[48];
    ezb_extaddr_t eui;
    uint8_t ic[18];
    if (!c6_base_query(req, "eui", eui_s, sizeof eui_s) || !c6_zb_parse_eui64(eui_s, &eui) ||
        !c6_base_query(req, "ic", ic_s, sizeof ic_s) || c6_zb_parse_hex(ic_s, ic, sizeof ic) != 18) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "eui= 16 hex, ic= 36 hex (128-bit + CRC)");
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_err_t err = ezb_secur_ic_add(&eui, EZB_SECUR_IC_TYPE_128, ic);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "install code added over HTTP for %s (CRC %02X%02X): %d", eui_s, ic[16], ic[17], err);
    return httpd_resp_sendstr(req, err == EZB_ERR_NONE ? "ok\n" : "refused\n");
}

static esp_err_t permit_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    uint32_t s = arg_u(req, "s", 60);
    if (s > 254) {
        s = 254;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_err_t err = s ? ezb_bdb_open_network(s) : ezb_bdb_close_network();
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "permit join for %lu s requested over HTTP: %d", (unsigned long)s, err);
    return httpd_resp_sendstr(req, err == EZB_ERR_NONE ? "ok\n" : "refused\n");
}

static esp_err_t form_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_err_t err = c6_zb_commission(EZB_BDB_MODE_NETWORK_FORMATION);
    esp_zigbee_lock_release();
    char msg[32];
    snprintf(msg, sizeof msg, "formation requested: %d\n", err);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t policy_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    uint32_t ic = arg_u(req, "ic_required", 2), tclk = arg_u(req, "tclk_required", 2);
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_err_t e1 = ic < 2 ? ezb_secur_set_ic_required(ic) : EZB_ERR_NONE;
    ezb_err_t e2 = tclk < 2 ? ezb_secur_set_tclk_exchange_required(tclk) : EZB_ERR_NONE;
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "policy over HTTP: ic_required=%lu (%d) tclk_required=%lu (%d)  [2 = unchanged]",
             (unsigned long)ic, e1, (unsigned long)tclk, e2);
    return httpd_resp_sendstr(req, e1 == EZB_ERR_NONE && e2 == EZB_ERR_NONE ? "ok\n" : "refused\n");
}

static esp_err_t zb_get(httpd_req_t *req)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "{\"role\":\"coordinator\",");
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    n += c6_zb_status_json(buf + n, sizeof buf - n);
    esp_zigbee_lock_release();
    n += snprintf(buf + n, sizeof buf - n, ",\"devices\":[");
    int64_t now = esp_timer_get_time();
    for (int i = 0, first = 1; i < MAX_DEVICES; i++) {
        if (s_dev[i].ieee.u64) {
            char ieee[20];
            c6_zb_eui64_str(&s_dev[i].ieee, ieee, sizeof ieee);
            n += snprintf(buf + n, sizeof buf - n, "%s{\"short\":\"0x%04X\",\"ieee\":\"%s\",\"announced_s_ago\":%lld}",
                          first ? "" : ",", s_dev[i].short_addr, ieee, (now - s_dev[i].seen_us) / 1000000);
            first = 0;
        }
    }
    snprintf(buf + n, sizeof buf - n, "]}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

void app_main(void)
{
    c6_base_start();
    const httpd_uri_t uris[] = {
        {.uri = "/zb",        .method = HTTP_GET,  .handler = zb_get},
        {.uri = "/zb/permit", .method = HTTP_POST, .handler = permit_post},
        {.uri = "/zb/form",   .method = HTTP_POST, .handler = form_post},
        {.uri = "/zb/ic",     .method = HTTP_POST, .handler = ic_post},
        {.uri = "/zb/policy", .method = HTTP_POST, .handler = policy_post},
        {.uri = "/zb/cmd",    .method = HTTP_POST, .handler = cmd_post},
        {.uri = "/zb/send",   .method = HTTP_POST, .handler = send_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(c6_base_httpd(), &uris[i]);
    }
    xTaskCreate(zb_task, "zigbee", 8192, NULL, 5, NULL);
}
