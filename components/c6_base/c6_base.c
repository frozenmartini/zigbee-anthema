/*
 * c6_base — the foundation every C6 image in this project carries (base, bulb, coord).
 *
 * Wi-Fi STA (c6_wifi.c), a push-OTA endpoint, a log ring readable over HTTP, SNTP so log lines
 * carry wall-clock time, mDNS as c6-<last three MAC bytes>.local with the _zigbee-anthema._tcp record
 * (c6_discovery.c), the claim (c6_auth.c), the button (c6_button.c) and the LED (c6_led.c). Every
 * image calls c6_base_start() first; the OTA endpoint and the rollback check must stay in every
 * image, or the next update needs USB again. Images add their own endpoints with c6_base_httpd().
 *
 * HTTP, port 80. The developer endpoints (unversioned; X-OTA-Token or the claim token, plain text):
 *   GET  /info             JSON: identity, build, partition, uptime, Wi-Fi
 *   GET  /log?from=N       log bytes from absolute offset N; X-Log-Next is the offset to ask for next
 *   POST /identify?s=N     the colour LED solid for N seconds (default 5); &rgb=RRGGBB its colour (white)
 *   POST /ota              body = app image (.bin); X-OTA-Token required
 *   POST /reboot           X-OTA-Token required
 * The protocol's base endpoints (auth class per c6_auth.h; every refusal JSON, c6_json_error):
 *   GET  /api/v1/info          none    identity, claim state (open/closed/claimed), uptime, ip, rssi, build
 *   POST /api/v1/identify?s=N  none    the colour LED solid N seconds, 1-60 (default 5); &rgb=RRGGBB as /identify
 *   GET  /api/v1/log?from=N    token   as /log
 *   POST /api/v1/claim         window  the claim token (64 hex), once
 *   POST /api/v1/release       token   clear the claim; the claim window opens for 600 s
 *   POST /api/v1/reboot        token   {"ok":true}, then a reboot after 2 s
 *   POST /api/v1/update        token   body = app image; X-Firmware-SHA256 required (below)
 * The button (GPIO CONFIG_C6_BUTTON_GPIO): a press opens the claim window for 600 s, a 3-9 s hold opens
 * the setup hotspot for 600 s, a 10 s hold clears the claim.
 *
 * OTA, two endpoints into the same spare slot, never at once (s_ota_busy is shared):
 *   POST /api/v1/update is the path the client and ota_push.py use. Before writing
 *   anything it needs Content-Length and X-Firmware-SHA256 (64 hex); it hashes the body as it writes
 *   it, then refuses the slot unless the IDF's own image check passes (400 bad_image), the SHA-256
 *   matches the header (400 sha_mismatch) and the image's project_name is the running image's
 *   (409 wrong_firmware: each image accepts only its own kind). Only then is the boot slot switched
 *   and the board rebooted. Its progress is the update object (c6_base_update_info, protocol §5):
 *   idle -> receiving (percent every 5 points) -> verifying -> rebooting, or back to idle with the
 *   error code. Every answer is JSON.
 *   POST /ota is the developer path, unchanged by 3.5: plain-text answers, no SHA or project
 *   check, no update object. ota_push.py falls back to it on a 0.5.x board, which has no /update.
 *
 * Rollback: an image written by OTA boots PENDING_VERIFY and verify_task marks it valid in three
 * stages, timed from boot: (1) Wi-Fi has an address and the HTTP server is up, i.e. it could take
 * the next OTA, within CONFIG_C6_VERIFY_TIMEOUT_S; (2) if the image asks for it
 * (c6_app_verify_zigbee: the bulb does, c6_base and c6_coord do not), Zigbee is back --
 * c6_base_zigbee_ready(), joined or factory new -- within CONFIG_C6_VERIFY_ZIGBEE_S of boot;
 * (3) it then runs CONFIG_C6_VERIFY_STABLE_S without a reset. A missed stage rolls it back at
 * once; a reset before it is marked valid (a crash) makes the bootloader roll it back. Either way
 * the previous image comes back. CONFIG_C6_VERIFY_BREAK_TEST (rollback test builds only) makes
 * stage 2 never pass.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif
#include "c6_base.h"
#include "c6_base_priv.h"
#include "c6_auth.h"
#include "c6_button.h"
#include "c6_discovery.h"
#include "c6_led.h"
#include "c6_wifi.h"

static const char *TAG = "c6";

/* The setup surface (improv.c, dns.c, portal.c); wired here. */
void improv_start(void);
void dns_start(void);
void portal_register(httpd_handle_t h);

#define BIT_GOT_IP BIT0
#define BIT_HTTPD  BIT1
#define BIT_ZIGBEE BIT2                     /* c6_base_zigbee_ready(); verify_task's stage 2, not in READY_BITS */
#define READY_BITS (BIT_GOT_IP | BIT_HTTPD)

static EventGroupHandle_t s_ev;
static char s_host[16];
static char s_mac[18];
static volatile bool s_ota_busy;           /* /ota or /api/v1/update running; cleared on a refusal, kept until the reboot */
static portMUX_TYPE s_update_mux = portMUX_INITIALIZER_UNLOCKED;   /* the update object's own, never s_ring_mux */
static c6_update_t s_update = {.state = "idle", .version = "", .percent = -1, .error = ""};   /* protocol §5 `update` */
static bool s_zb_ready_logged;             /* "VERIFY: zigbee ready" logged; once per boot */
static httpd_handle_t s_httpd;

/* ---------------------------------------------------------------- log ring */

#define LOG_RING 16384
static char s_ring[LOG_RING];
static uint32_t s_ring_total;           /* bytes ever written; the absolute offset /log speaks in */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_vprintf_orig;

static void ring_copy_in(const char *src, size_t len)
{
    size_t at = s_ring_total % LOG_RING;
    size_t first = MIN(len, LOG_RING - at);
    memcpy(s_ring + at, src, first);
    memcpy(s_ring, src + first, len - first);
    s_ring_total += len;
}

static int ring_vprintf(const char *fmt, va_list ap)
{
    char line[512];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = MIN((size_t)n, sizeof line - 1);
        portENTER_CRITICAL(&s_ring_mux);
        ring_copy_in(line, len);
        portEXIT_CRITICAL(&s_ring_mux);
    }
    return s_vprintf_orig(fmt, ap);
}

/* ---------------------------------------------------------------- HTTP */

/* The developer endpoints keep their plain-text 403; the check itself is c6_auth's. */
bool c6_base_token_ok(httpd_req_t *req)
{
    if (c6_auth_dev_ok(req)) {
        return true;
    }
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "bad or missing X-OTA-Token\n");
    return false;
}

static const char *ota_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

bool c6_base_query(httpd_req_t *req, const char *key, char *val, size_t len)
{
    char q[160];
    return httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
           httpd_query_key_value(q, key, val, len) == ESP_OK;
}

httpd_handle_t c6_base_httpd(void)
{
    return s_httpd;
}

const char *c6_base_host(void)
{
    return s_host;
}

void c6_base_got_ip(bool up)
{
    if (up) {
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
    }
}

/* The app hooks (c6_base.h): no-ops until an image defines its own. */
__attribute__((weak))
void c6_app_state_mark(uint32_t keys)
{
    (void)keys;
}

__attribute__((weak))
void c6_app_events_close(void)
{
}

/* false: this image's first-start check does not wait for Zigbee (c6_base, c6_coord). The bulb's
 * strong definition returns true. */
__attribute__((weak))
bool c6_app_verify_zigbee(void)
{
    return false;
}

/* in as a JSON string's contents into out (n > 0): '"', '\\' and control characters escaped. A value
 * that does not fit is cut before the escape or the UTF-8 character that would not fit whole. */
static void json_escape(char *out, size_t n, const char *in)
{
    static const char digits[] = "0123456789abcdef";
    size_t o = 0;
    if (n == 0) {
        return;
    }
    for (; *in; in++) {
        unsigned char ch = (unsigned char)*in;
        char e[6];
        size_t k = 1;
        e[0] = (char)ch;
        if (ch == '"' || ch == '\\') {
            e[0] = '\\';
            e[1] = (char)ch;
            k = 2;
        } else if (ch < 0x20) {
            e[0] = '\\';
            e[1] = 'u';
            e[2] = '0';
            e[3] = '0';
            e[4] = digits[ch >> 4];
            e[5] = digits[ch & 0x0f];
            k = 6;
        }
        if (o + k >= n) {
            break;
        }
        memcpy(out + o, e, k);
        o += k;
    }
    if (*in) {                             /* cut short: drop a UTF-8 character that may be partial */
        while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) {
            o--;
        }
        if (o > 0 && (unsigned char)out[o - 1] >= 0xC0) {
            o--;
        }
    }
    out[o] = '\0';
}

static const char *http_status_line(int status)
{
    switch (status) {
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
    }
}

esp_err_t c6_json_error(httpd_req_t *req, int status, const char *code, const char *message)
{
    char c[40], m[320], body[400];
    json_escape(c, sizeof c, code ? code : "internal");
    json_escape(m, sizeof m, message ? message : "");
    snprintf(body, sizeof body, "{\"error\":\"%s\",\"message\":\"%s\"}", c, m);
    httpd_resp_set_status(req, http_status_line(status));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

void c6_base_info(c6_base_info_t *out)
{
    memset(out, 0, sizeof *out);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    wifi_ap_record_t ap = {0};
    out->rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *nif = c6_wifi_netif();
    if (nif) {
        esp_netif_get_ip_info(nif, &ip);
    }
    esp_ip4addr_ntoa(&ip.ip, out->ip, sizeof out->ip);
    strlcpy(out->id, s_host[0] ? s_host + 3 : "", sizeof out->id);    /* s_host is "c6-<id>" */
    strlcpy(out->mac, s_mac, sizeof out->mac);
    strlcpy(out->fw, app->version, sizeof out->fw);
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    strlcpy(out->ota_state, ota_state_name(st), sizeof out->ota_state);
    strlcpy(out->partition, run->label, sizeof out->partition);
}

void c6_base_name(char *out, size_t n)
{
    if (!out || n == 0) {
        return;
    }
    char nm[48];
    size_t len = sizeof nm;
    bool got = false;
    nvs_handle_t h;
    if (nvs_open("c6cfg", NVS_READONLY, &h) == ESP_OK) {
        got = nvs_get_str(h, "name", nm, &len) == ESP_OK && nm[0] != '\0';
        nvs_close(h);
    }
    if (got) {
        strlcpy(out, nm, n);
    } else {
        snprintf(out, n, "Zigbee Anthema %s", s_host[0] ? s_host + 3 : "");
    }
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    wifi_ap_record_t ap = {0};
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(c6_wifi_netif(), &ip);
    char elf[9];
    esp_app_get_elf_sha256(elf, sizeof elf);

    char buf[640];
    snprintf(buf, sizeof buf,
             "{\"host\":\"%s\",\"mac\":\"%s\",\"project\":\"%s\",\"version\":\"%s\","
             "\"elf_sha256\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\","
             "\"partition\":\"%s\",\"ota_state\":\"%s\",\"uptime_s\":%.1f,\"reset_reason\":%d,"
             "\"ip\":\"" IPSTR "\",\"rssi\":%d,\"channel\":%d,\"free_heap\":%lu,\"min_free_heap\":%lu,"
             "\"time\":%lld}\n",
             s_host, s_mac, app->project_name, app->version, elf, app->date, app->time,
             app->idf_ver, run->label, ota_state_name(st), esp_timer_get_time() / 1e6,
             (int)esp_reset_reason(), IP2STR(&ip.ip), rssi, ap.primary,
             (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
             (long long)time(NULL));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t log_send(httpd_req_t *req, bool json)
{
    uint32_t from = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "from", v, sizeof v) == ESP_OK) {
        from = strtoul(v, NULL, 10);
    }
    char *out = malloc(LOG_RING);
    if (!out) {
        return json ? c6_json_error(req, 500, "internal", "out of memory; try again") : httpd_resp_send_500(req);
    }
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t total = s_ring_total;
    uint32_t oldest = total > LOG_RING ? total - LOG_RING : 0;
    if (from < oldest || from > total) {
        from = oldest;            /* overrun, or the board rebooted and the counter restarted */
    }
    uint32_t len = total - from;
    size_t at = from % LOG_RING;
    size_t first = MIN((size_t)len, LOG_RING - at);
    memcpy(out, s_ring + at, first);
    memcpy(out + first, s_ring, len - first);
    portEXIT_CRITICAL(&s_ring_mux);

    char next[16];
    snprintf(next, sizeof next, "%lu", (unsigned long)total);
    httpd_resp_set_hdr(req, "X-Log-Next", next);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, out, len);
    free(out);
    return err;
}

static esp_err_t log_get(httpd_req_t *req)
{
    return log_send(req, false);
}

/* Identify: the LED's IDENTIFY source (over every other pattern) for a time; a timer clears it. */
static esp_timer_handle_t s_identify_timer;

static void identify_end(void *arg)
{
    (void)arg;
    c6_led_set(C6_LED_IDENTIFY, false);
}

/* From the HTTP task only, rgb 0xRRGGBB. A second call while one runs restarts the count and takes its colour. */
static void identify_start(int secs, uint32_t rgb)
{
    if (!s_identify_timer) {
        const esp_timer_create_args_t a = {.callback = identify_end, .name = "identify"};
        if (esp_timer_create(&a, &s_identify_timer) != ESP_OK) {
            ESP_LOGE(TAG, "identify: no timer");
            return;
        }
    }
    esp_timer_stop(s_identify_timer);      /* not running: ESP_ERR_INVALID_STATE, nothing to stop */
    c6_led_identify_rgb(rgb);
    c6_led_set(C6_LED_IDENTIFY, true);
    esp_timer_start_once(s_identify_timer, (uint64_t)secs * 1000000);
}

/* Identify's colour from the query: no rgb, white; exactly six hex digits (either case), that colour.
 * Anything else is false and *rgb is left as it was. */
static bool identify_rgb(httpd_req_t *req, uint32_t *rgb)
{
    char q[160], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) {
        *rgb = C6_LED_WHITE;                       /* no query (or one too long to read): no colour asked for */
        return true;
    }
    esp_err_t err = httpd_query_key_value(q, "rgb", v, sizeof v);
    if (err == ESP_ERR_NOT_FOUND) {
        *rgb = C6_LED_WHITE;
        return true;
    }
    if (err != ESP_OK || strlen(v) != 6) {        /* ESP_ERR_HTTPD_RESULT_TRUNC: more than seven characters */
        return false;
    }
    uint32_t c = 0;
    for (int i = 0; i < 6; i++) {
        char d = v[i];
        uint32_t n = d >= '0' && d <= '9' ? (uint32_t)(d - '0')
                   : d >= 'a' && d <= 'f' ? (uint32_t)(d - 'a' + 10)
                   : d >= 'A' && d <= 'F' ? (uint32_t)(d - 'A' + 10) : 16;
        if (n > 15) {
            return false;
        }
        c = c << 4 | n;
    }
    *rgb = c;
    return true;
}

static esp_err_t identify_post(httpd_req_t *req)
{
    int secs = 5;
    uint32_t rgb = C6_LED_WHITE;
    char v[8];
    if (c6_base_query(req, "s", v, sizeof v)) {      /* the whole query, as rgb is read: a long one keeps s */
        secs = MAX(1, MIN(atoi(v), 60));
    }
    if (!identify_rgb(req, &rgb)) {
        return c6_json_error(req, 400, "bad_request", "rgb must be six hex digits, RRGGBB");
    }
    ESP_LOGI(TAG, "identify: %d s, colour %06lX", secs, (unsigned long)rgb);
    identify_start(secs, rgb);
    return httpd_resp_sendstr(req, "ok\n");
}

/* A weak link's TCP retransmit takes ~1.5 s: the restart waits long enough for the answer and the stream's
 * end to get one. */
#define REBOOT_DELAY_MS 2000

/* Shared by /reboot, /api/v1/reboot, ota_post and update_post: the open event stream ends before the restart. */
static void reboot_task(void *arg)
{
    c6_app_events_close();
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "reboot requested over HTTP");
    httpd_resp_sendstr(req, "ok: rebooting\n");
    xTaskCreate(reboot_task, "reboot", 6144, NULL, 5, NULL);   /* 6144: it ends the event stream first (0.5.0; 6 KB since 0.7.0) */
    return ESP_OK;
}

/* ---------------------------------------------------------------- /api/v1 base endpoints */

static esp_err_t api_info_get(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_NONE)) {
        return ESP_OK;
    }
    c6_base_info_t bi;
    c6_base_info(&bi);
    char name[48], esc[256];
    c6_base_name(name, sizeof name);
    json_escape(esc, sizeof esc, name);
    const esp_app_desc_t *app = esp_app_get_description();
    const char *claim = c6_auth_claimed() ? "claimed" : c6_auth_window_open() ? "open" : "closed";
    char buf[768];
    int n = snprintf(buf, sizeof buf,
                     "{\"id\":\"%s\",\"name\":\"%s\",\"mac\":\"%s\",\"hw\":\"nanoc6\",\"fw\":\"%s\",\"api\":1,"
                     "\"claim\":\"%s\",\"uptime_s\":%lu,\"ip\":\"%s\",\"rssi\":%d,\"partition\":\"%s\","
                     "\"ota_state\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\"}",
                     bi.id, esc, bi.mac, bi.fw, claim, (unsigned long)bi.uptime_s, bi.ip, bi.rssi,
                     bi.partition, bi.ota_state, app->date, app->time, app->idf_ver);
    if (n < 0 || (size_t)n >= sizeof buf) {
        return c6_json_error(req, 500, "internal", "the answer did not fit; try again");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t api_identify_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_NONE)) {
        return ESP_OK;
    }
    int secs = 5;
    uint32_t rgb = C6_LED_WHITE;
    char v[8];
    if (c6_base_query(req, "s", v, sizeof v)) {
        secs = MAX(1, MIN(atoi(v), 60));
    }
    if (!identify_rgb(req, &rgb)) {
        return c6_json_error(req, 400, "bad_request", "rgb must be six hex digits, RRGGBB");
    }
    ESP_LOGI(TAG, "identify: %d s, colour %06lX", secs, (unsigned long)rgb);
    identify_start(secs, rgb);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Today's /log behind the token: from=N, X-Log-Next, text/plain (log_get, unchanged). */
static esp_err_t api_log_get(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    return log_send(req, true);               /* out of memory: the JSON 500, not httpd's plain one */
}

static esp_err_t api_claim_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_WINDOW)) {
        return ESP_OK;
    }
    char tok[65];
    esp_err_t err = c6_auth_claim(tok);
    if (err == ESP_ERR_INVALID_STATE) {
        return c6_json_error(req, 409, "already_claimed",
                             "the board is claimed; to claim it again, hold its button 10 s first");
    }
    if (err != ESP_OK) {
        return c6_json_error(req, 500, "storage", "the claim could not be stored; nothing changed, try again");
    }
    char out[80];
    snprintf(out, sizeof out, "{\"%s\":\"%s\"}", "token", tok);   /* the one place the key is spelled */
    mbedtls_platform_zeroize(tok, sizeof tok);
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, out);
    mbedtls_platform_zeroize(out, sizeof out);
    return err;
}

static esp_err_t api_release_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "release requested over HTTP");
    if (c6_auth_clear() != ESP_OK) {        /* erase; LED; window 600 s; the stream ended; TXT unclaimed */
        return c6_json_error(req, 500, "storage", "the claim could not be erased; nothing changed, try again");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_reboot_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "reboot requested over HTTP (api/v1)");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    c6_app_events_close();                 /* here, on the HTTP task's stack; reboot_task's call is then a no-op */
    xTaskCreate(reboot_task, "reboot", 6144, NULL, 5, NULL);   /* 6144: it ends the event stream first (0.5.0; 6 KB since 0.7.0) */
    return ESP_OK;
}

static esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *why)
{
    if (h) {
        esp_ota_abort(h);
    }
    s_ota_busy = false;
    ESP_LOGE(TAG, "OTA failed: %s", why);
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, why);
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    if (s_ota_busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "an OTA is already in progress\n");
    }
    if (req->content_len == 0) {
        httpd_resp_set_status(req, "411 Length Required");
        return httpd_resp_sendstr(req, "send the image with a Content-Length\n");
    }
    s_ota_busy = true;
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || req->content_len > dst->size) {
        return ota_fail(req, 0, "413 Content Too Large", "image larger than the OTA slot\n");
    }
    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", (unsigned)req->content_len, dst->label);
    int64_t t0 = esp_timer_get_time();
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, req->content_len, &h) != ESP_OK) {
        return ota_fail(req, 0, "500 Internal Server Error", "esp_ota_begin failed\n");
    }
    char *buf = malloc(4096);
    if (!buf) {
        return ota_fail(req, h, "500 Internal Server Error", "out of memory\n");
    }
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, MIN(remaining, 4096));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return ota_fail(req, h, "408 Request Timeout", "receive failed\n");
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            free(buf);
            return ota_fail(req, h, "500 Internal Server Error", "esp_ota_write failed\n");
        }
        remaining -= r;
    }
    free(buf);
    esp_err_t err = esp_ota_end(h);           /* verifies the image */
    if (err != ESP_OK) {
        s_ota_busy = false;
        ESP_LOGE(TAG, "OTA: image rejected: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "image failed verification\n");
    }
    if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        return ota_fail(req, 0, "500 Internal Server Error", "esp_ota_set_boot_partition failed\n");
    }
    ESP_LOGI(TAG, "OTA: %u bytes written to %s in %.1f s; rebooting",
             (unsigned)req->content_len, dst->label, (esp_timer_get_time() - t0) / 1e6);
    char msg[64];
    snprintf(msg, sizeof msg, "ok: written to %s, rebooting\n", dst->label);
    httpd_resp_sendstr(req, msg);
    xTaskCreate(reboot_task, "reboot", 6144, NULL, 5, NULL);   /* 6144: it ends the event stream first (0.5.0; 6 KB since 0.7.0) */
    return ESP_OK;
}

/* ---------------------------------------------------------------- the update endpoint */

/* The percent cadence: mark the object again once the integer percent
 * has moved by 5 or more since the last mark, or has reached 100. */
static bool update_percent_due(int last, int now)
{
    if (now >= 100) {
        return last < 100;
    }
    return now - last >= 5;
}

/* The whole object under s_update_mux; the mark (C6_ST_UPDATE) after it, outside the critical section. */
static void update_set(const char *state, int percent, const char *version, const char *error)
{
    portENTER_CRITICAL(&s_update_mux);
    strlcpy(s_update.state, state, sizeof s_update.state);
    s_update.percent = percent;
    strlcpy(s_update.version, version, sizeof s_update.version);
    strlcpy(s_update.error, error, sizeof s_update.error);
    portEXIT_CRITICAL(&s_update_mux);
    c6_app_state_mark(C6_ST_UPDATE);
}

void c6_base_update_info(c6_update_t *out)
{
    portENTER_CRITICAL(&s_update_mux);
    *out = s_update;
    portEXIT_CRITICAL(&s_update_mux);
}

/* ota_fail's twin for /api/v1/update: abort the handle if there is one, free the busy flag, put the
 * object back to idle with the code, answer with the JSON error. Only after the object left idle. */
static esp_err_t update_fail(httpd_req_t *req, esp_ota_handle_t h, int status, const char *code, const char *message)
{
    if (h) {
        esp_ota_abort(h);
    }
    s_ota_busy = false;
    update_set("idle", -1, "", code);
    ESP_LOGE(TAG, "UPDATE: refused, %s: %s", code, message);
    return c6_json_error(req, status, code, message);
}

/* POST /api/v1/update (token; protocol §6.5). The checks, in order, before any flash write: busy,
 * Content-Length, X-Firmware-SHA256 (refusals there leave the object as it was), then the slot and
 * esp_ota_begin. The body goes to the slot and to a SHA-256 as it arrives; then esp_ota_end (the
 * IDF's image check), the digest against the header, the slot's project_name against the running
 * image's, and only then the boot slot. Stack: the SHA context, two digests, the header and one
 * message (~500 B of the HTTP task's 6,144); the chunk buffer is heap and later holds the slot's
 * 256 B app description. */
static esp_err_t update_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    if (s_ota_busy) {
        return c6_json_error(req, 409, "busy", "an update is already running");
    }
    if (req->content_len == 0) {
        return c6_json_error(req, 400, "bad_request", "send the image with a Content-Length");
    }
    char hdr[65];
    uint8_t want[32] = {0};
    if (httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256") != 64 ||
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", hdr, sizeof hdr) != ESP_OK) {
        return c6_json_error(req, 400, "bad_request", "X-Firmware-SHA256 must be the image's SHA-256, 64 hex characters");
    }
    for (int i = 0; i < 64; i++) {
        char c = hdr[i];
        int v = (c >= '0' && c <= '9') ? c - '0' :
                (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) {
            return c6_json_error(req, 400, "bad_request", "X-Firmware-SHA256 must be the image's SHA-256, 64 hex characters");
        }
        want[i / 2] = (i % 2) ? (uint8_t)(want[i / 2] | v) : (uint8_t)(v << 4);
    }

    s_ota_busy = true;                         /* from here every exit is update_fail or the reboot */
    update_set("receiving", 0, "", "");        /* clears the last update's error */
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        return update_fail(req, 0, 413, "too_large", "this board has no update slot");
    }
    if (req->content_len > dst->size) {
        return update_fail(req, 0, 413, "too_large", "the image is larger than the update slot");
    }
    ESP_LOGI(TAG, "UPDATE: receiving %u bytes into %s, X-Firmware-SHA256 %s",
             (unsigned)req->content_len, dst->label, hdr);
    int64_t t0 = esp_timer_get_time();
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, req->content_len, &h) != ESP_OK) {
        return update_fail(req, 0, 500, "internal", "esp_ota_begin failed; nothing changed");
    }
    char *buf = malloc(4096);
    if (!buf) {
        return update_fail(req, h, 500, "internal", "out of memory");
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int sha_rc = mbedtls_sha256_starts(&sha, 0);    /* any failure: refused below, after esp_ota_end */
    size_t written = 0;
    int marked = 0;                            /* the percent of the last mark */
    int timeouts = 0;
    while (written < req->content_len) {
        int r = httpd_req_recv(req, buf, MIN(req->content_len - written, 4096));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) {
            continue;
        }
        if (r <= 0) {
            mbedtls_sha256_free(&sha);
            free(buf);
            return update_fail(req, h, 400, "bad_request", "the body ended early");
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            mbedtls_sha256_free(&sha);
            free(buf);
            return update_fail(req, h, 500, "internal", "esp_ota_write failed");
        }
        sha_rc |= mbedtls_sha256_update(&sha, (const unsigned char *)buf, r);
        written += r;
        int pct = (int)((uint64_t)written * 100 / req->content_len);
        if (update_percent_due(marked, pct)) {
            marked = pct;
            update_set("receiving", pct, "", "");
        }
    }
    uint8_t got[32];
    sha_rc |= mbedtls_sha256_finish(&sha, got);
    mbedtls_sha256_free(&sha);
    update_set("verifying", 100, "", "");

    esp_err_t err = esp_ota_end(h);            /* the IDF's image check; the handle is gone either way */
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "UPDATE: esp_ota_end: %s", esp_err_to_name(err));
        return update_fail(req, 0, 400, "bad_image", "the image failed verification");
    }
    if (sha_rc != 0) {
        free(buf);
        return update_fail(req, 0, 500, "internal", "mbedtls_sha256 failed; the image was not checked");
    }
    if (memcmp(got, want, sizeof got) != 0) {
        free(buf);
        return update_fail(req, 0, 400, "sha_mismatch", "the image's SHA-256 does not match X-Firmware-SHA256");
    }
    char msg[128];
    esp_app_desc_t *desc = (esp_app_desc_t *)buf;     /* 256 B, off the HTTP task's stack */
    memset(desc, 0, sizeof *desc);
    const esp_app_desc_t *self = esp_app_get_description();
    bool have = esp_ota_get_partition_description(dst, desc) == ESP_OK;
    desc->project_name[sizeof desc->project_name - 1] = '\0';
    desc->version[sizeof desc->version - 1] = '\0';
    if (!have || strcmp(desc->project_name, self->project_name) != 0) {
        snprintf(msg, sizeof msg, "the image is %s, this board runs %s",
                 have ? desc->project_name : "unreadable", self->project_name);
        free(buf);
        return update_fail(req, 0, 409, "wrong_firmware", msg);
    }
    if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        free(buf);
        return update_fail(req, 0, 500, "internal", "esp_ota_set_boot_partition failed; the running image is unchanged");
    }
    update_set("rebooting", 100, desc->version, "");
    char ver[48];
    json_escape(ver, sizeof ver, desc->version);
    snprintf(msg, sizeof msg, "{\"ok\":true,\"version\":\"%s\"}", ver);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "UPDATE: %u bytes to %s in %.1f s, %s %s; rebooting",
             (unsigned)req->content_len, dst->label, (esp_timer_get_time() - t0) / 1e6,
             desc->project_name, desc->version);
    free(buf);
    c6_app_events_close();                     /* here, on the HTTP task's stack; reboot_task's call is then a no-op */
    xTaskCreate(reboot_task, "reboot", 6144, NULL, 5, NULL);   /* 6144: it ends the event stream first; s_ota_busy stays true until the restart */
    return ESP_OK;
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 48;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));
    const httpd_uri_t uris[] = {
        {.uri = "/info",     .method = HTTP_GET,  .handler = info_get},
        {.uri = "/log",      .method = HTTP_GET,  .handler = log_get},
        {.uri = "/identify", .method = HTTP_POST, .handler = identify_post},
        {.uri = "/ota",      .method = HTTP_POST, .handler = ota_post},
        {.uri = "/reboot",   .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/v1/info",     .method = HTTP_GET,  .handler = api_info_get},
        {.uri = "/api/v1/identify", .method = HTTP_POST, .handler = api_identify_post},
        {.uri = "/api/v1/log",      .method = HTTP_GET,  .handler = api_log_get},
        {.uri = "/api/v1/claim",    .method = HTTP_POST, .handler = api_claim_post},
        {.uri = "/api/v1/release",  .method = HTTP_POST, .handler = api_release_post},
        {.uri = "/api/v1/reboot",   .method = HTTP_POST, .handler = api_reboot_post},
        {.uri = "/api/v1/update",   .method = HTTP_POST, .handler = update_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uris[i]));
    }
    xEventGroupSetBits(s_ev, BIT_HTTPD);
}


static void time_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "time synced: %lld", (long long)tv->tv_sec);
}

static void start_services(void)
{
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_host);
        c6_discovery_start();              /* _zigbee-anthema._tcp, the board's name, the TXT record */
    }
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = time_synced;
    esp_netif_sntp_init(&sc);
}

/* ---------------------------------------------------------------- rollback */

/* The image's Zigbee is back: joined, or factory new with nothing to join. From the
 * Zigbee task, outside every critical section; safe before s_ev exists and on every later call. The bit
 * is never cleared: once is enough, the question is whether this image can join. */
void c6_base_zigbee_ready(void)
{
    if (!s_ev) {
        return;
    }
#if CONFIG_C6_VERIFY_BREAK_TEST
    if (!s_zb_ready_logged) {
        s_zb_ready_logged = true;
        ESP_LOGW(TAG, "VERIFY: zigbee ready ignored (C6_VERIFY_BREAK_TEST)");
    }
#else
    xEventGroupSetBits(s_ev, BIT_ZIGBEE);
    if (!s_zb_ready_logged) {
        s_zb_ready_logged = true;
        ESP_LOGI(TAG, "VERIFY: zigbee ready");
    }
#endif
}

/* The first-start check of a PENDING_VERIFY image, three stages timed from t0 (task start, within
 * milliseconds of boot): Wi-Fi + HTTP within CONFIG_C6_VERIFY_TIMEOUT_S; Zigbee, if the image asks,
 * within CONFIG_C6_VERIFY_ZIGBEE_S of t0; then CONFIG_C6_VERIFY_STABLE_S without a reset. A miss
 * rolls back. */
static void verify_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    EventBits_t b = xEventGroupWaitBits(s_ev, READY_BITS, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(CONFIG_C6_VERIFY_TIMEOUT_S * 1000));
    if ((b & READY_BITS) != READY_BITS) {
        ESP_LOGE(TAG, "VERIFY: not reachable after %d s: rolling back", CONFIG_C6_VERIFY_TIMEOUT_S);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    } else {
        ESP_LOGI(TAG, "VERIFY: wifi+http up after %lld s", (long long)((esp_timer_get_time() - t0) / 1000000));
        bool zigbee_ok = true;
        if (c6_app_verify_zigbee()) {
            int64_t left_ms = (int64_t)CONFIG_C6_VERIFY_ZIGBEE_S * 1000 - (esp_timer_get_time() - t0) / 1000;
            if (left_ms < 0) {
                left_ms = 0;
            }
            b = xEventGroupWaitBits(s_ev, BIT_ZIGBEE, pdFALSE, pdTRUE, pdMS_TO_TICKS((uint32_t)left_ms));
            zigbee_ok = (b & BIT_ZIGBEE) != 0;
            if (zigbee_ok) {
                ESP_LOGI(TAG, "VERIFY: zigbee ready after %lld s", (long long)((esp_timer_get_time() - t0) / 1000000));
            } else {
                ESP_LOGE(TAG, "VERIFY: zigbee not back after %d s: rolling back", CONFIG_C6_VERIFY_ZIGBEE_S);
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        } else {
            ESP_LOGI(TAG, "VERIFY: no zigbee check in this image");
        }
        if (zigbee_ok) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_C6_VERIFY_STABLE_S * 1000));
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "VERIFY: new image marked valid");
        }
    }
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- start */

/* The button's three gestures. From the button's task. */
static void button_action(int gesture)
{
    switch (gesture) {
    case C6_BUTTON_PRESS:
        c6_auth_window_open_for(600);
        break;
    case C6_BUTTON_HOLD_SHORT:
        c6_wifi_ap_start(600);
        break;
    case C6_BUTTON_HOLD_LONG:
        c6_auth_clear();
        break;
    default:
        break;
    }
}

void c6_base_start(void)
{
    s_vprintf_orig = esp_log_set_vprintf(ring_vprintf);
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_mac, sizeof s_mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_host, sizeof s_host, "c6-%02x%02x%02x", mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    ESP_LOGI(TAG, "%s %s on %s (%s), partition %s, ota_state %s, reset reason %d",
             app->project_name, app->version, s_host, s_mac, run->label, ota_state_name(st),
             (int)esp_reset_reason());
    c6_led_start();
    c6_auth_start();                       /* the claim from NVS; the window open 600 s from boot */

    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        xTaskCreate(verify_task, "verify", 3584, NULL, 5, NULL);   /* 3,584: two more %lld log formats */
    }
    c6_wifi_start();
    c6_button_start(button_action);
    start_services();
    start_http();
    improv_start();
    dns_start();
    portal_register(c6_base_httpd());
}
