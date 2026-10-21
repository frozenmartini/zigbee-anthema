/*
 * c6_wifi — the Wi-Fi station of c6_base: credentials in NVS with a one-time migration from
 * the build, the failure timer, the setup hotspot, scanning.
 *
 * Contexts: on_event and the three timer callbacks run in the event / esp_timer tasks and only
 * start or stop timers and flip flags (never NVS, never c6_wifi_set). c6_wifi_set and
 * c6_wifi_scan are for a task of the caller's (HTTP, Improv), never the stack's.
 *
 * The password: read from NVS or the build into a stack buffer, copied into a wifi_config_t,
 * both wiped after esp_wifi_set_config. It is never logged and nothing here returns it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif
#include "c6_base.h"
#include "c6_base_priv.h"
#include "c6_led.h"
#include "c6_wifi.h"

static const char *TAG = "c6";

#define WIFI_NVS_NS         "c6wifi"
#define WIFI_SSID_MAX       32          /* chars; wifi_sta_config_t.ssid is 32 bytes, no NUL needed */
#define WIFI_PASS_MIN       8
#define WIFI_PASS_MAX       63          /* chars; wifi_sta_config_t.password is 64 bytes */
#define WIFI_BACKOFF_MIN_MS 1000
#define WIFI_BACKOFF_MAX_MS 30000
#define WIFI_AP_LINGER_MS   10000       /* ap_start(0): the hotspot closes this long after the STA has an IP */
#define WIFI_AP_MAX_CONN    2
#define WIFI_AP_CHANNEL_DEF 1
#define WIFI_SCAN_MAX       16          /* results handed to the caller */
#define WIFI_SCAN_FETCH_MAX 32          /* records taken from the driver before sorting and dropping duplicates */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_netif_t *s_netif;                /* the station */
static esp_netif_t *s_ap_netif;             /* the hotspot; created once, on first use */
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_fail_timer;     /* CONFIG_C6_WIFI_FAIL_S after boot or c6_wifi_set with no IP */
static esp_timer_handle_t s_ap_timer;       /* closes the hotspot */
static uint32_t s_backoff_ms = WIFI_BACKOFF_MIN_MS;
static c6_wifi_state_t s_state = C6_WIFI_NO_CREDENTIALS;
static bool s_have_creds;                   /* a stored (or migrated) SSID exists: the station may connect */
static bool s_have_ip;
static bool s_ap_up;
static bool s_ap_until_ip;                  /* the hotspot was opened by ap_start(0): closes 10 s after an IP */
static bool s_scanning;
static char s_ssid[WIFI_SSID_MAX + 1];
static SemaphoreHandle_t s_store_lock;      /* one credential snapshot/write/rollback at a time */
static c6_wifi_ap_cb_t s_ap_cb;

/* ------------------------------------------------------------ pure helpers */

/* Length within [min, max] and no control character (0x00–0x1f, 0x7f). Any other byte passes. */
static bool wifi_text_ok(const char *s, size_t min, size_t max)
{
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        if (n >= max) {
            return false;
        }
        unsigned char c = (unsigned char)s[n];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return n >= min;
}

static bool wifi_ssid_ok(const char *ssid)
{
    return ssid != NULL && wifi_text_ok(ssid, 1, WIFI_SSID_MAX);
}

static bool wifi_pass_ok(const char *pass)
{
    return pass != NULL && (pass[0] == '\0' || wifi_text_ok(pass, WIFI_PASS_MIN, WIFI_PASS_MAX));
}

/* Today's retry rule: the delay doubles from 1 s and stops at 30 s. */
static uint32_t wifi_next_backoff_ms(uint32_t ms)
{
    return MIN(ms * 2, WIFI_BACKOFF_MAX_MS);
}

/* The hotspot's channel: the station's while it is connected (nonzero), else 1. */
static uint8_t wifi_ap_channel(uint8_t sta_channel)
{
    return sta_channel != 0 ? sta_channel : WIFI_AP_CHANNEL_DEF;
}

/* When the hotspot closes, in microseconds from now; 0 = not yet (ap_start(0) waits for the IP). */
static uint64_t wifi_ap_stop_delay_us(uint32_t seconds, bool have_ip)
{
    if (seconds > 0) {
        return (uint64_t)seconds * 1000000ULL;
    }
    return have_ip ? (uint64_t)WIFI_AP_LINGER_MS * 1000ULL : 0;
}

/* CONFIG_C6_AP_SSID_PREFIX + the six-hex serial: host "c6-xxxxxx" gives "xxxxxx". */
static void wifi_ap_ssid(char *out, size_t n, const char *host)
{
    const char *id = (strncmp(host, "c6-", 3) == 0) ? host + 3 : host;
    snprintf(out, n, "%s%s", CONFIG_C6_AP_SSID_PREFIX, id);
}

/* Strongest first, hidden (empty SSID) dropped, one per SSID with the strongest kept; at most max. */
static int wifi_scan_pick(const wifi_ap_record_t *r, int n, const wifi_ap_record_t **out, int max)
{
    bool used[WIFI_SCAN_FETCH_MAX] = {0};
    int count = 0;
    if (n > WIFI_SCAN_FETCH_MAX) {
        n = WIFI_SCAN_FETCH_MAX;
    }
    while (count < max) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used[i] || r[i].ssid[0] == '\0') {
                continue;
            }
            if (best < 0 || r[i].rssi > r[best].rssi) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        bool dup = false;
        for (int k = 0; k < count && !dup; k++) {
            dup = strncmp((const char *)out[k]->ssid, (const char *)r[best].ssid, sizeof r[best].ssid) == 0;
        }
        if (!dup) {
            out[count++] = &r[best];
        }
    }
    return count;
}

/* The station config from validated strings: a 32-char SSID fills its field with no NUL. */
static void wifi_sta_config(wifi_config_t *wc, const char *ssid, const char *pass)
{
    memset(wc, 0, sizeof *wc);
    memcpy(wc->sta.ssid, ssid, MIN(strlen(ssid), sizeof wc->sta.ssid));
    memcpy(wc->sta.password, pass, MIN(strlen(pass), sizeof wc->sta.password - 1));
}

/* ------------------------------------------------------------------ NVS */

/* The stored pair; false (and ssid empty) when no SSID is stored. A missing pass reads as empty. */
static bool wifi_load(char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    nvs_handle_t h;
    ssid[0] = '\0';
    pass[0] = '\0';
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = ssid_n;
    bool ok = nvs_get_str(h, "ssid", ssid, &n) == ESP_OK && ssid[0] != '\0';
    n = pass_n;
    if (ok && nvs_get_str(h, "pass", pass, &n) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    if (!ok) {
        ssid[0] = '\0';
    }
    return ok;
}

/* Both keys, one commit. Each set persists on its own in IDF, so put the old pair back on failure. */
static esp_err_t wifi_store(const char *ssid, const char *pass)
{
    if (s_store_lock) {
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        if (s_store_lock) {
            xSemaphoreGive(s_store_lock);
        }
        return err;
    }
    char old_ssid[WIFI_SSID_MAX + 1] = {0};
    char old_pass[WIFI_PASS_MAX + 1] = {0};
    const char *keys[] = {"ssid", "pass"};
    const char *next[] = {ssid, pass};
    char *old[] = {old_ssid, old_pass};
    size_t sizes[] = {sizeof old_ssid, sizeof old_pass};
    bool have[] = {false, false};
    unsigned done = 0;
    for (size_t i = 0; i < 2 && err == ESP_OK; i++) {
        err = nvs_get_str(h, keys[i], old[i], &sizes[i]);
        have[i] = err == ESP_OK;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    for (size_t i = 0; i < 2 && err == ESP_OK; i++) {
        err = nvs_set_str(h, keys[i], next[i]);
        if (err == ESP_OK) {
            done |= 1U << i;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK && done) {
        esp_err_t back = ESP_OK;
        for (size_t i = 0; i < 2; i++) {
            if (done & (1U << i)) {
                esp_err_t e = have[i] ? nvs_set_str(h, keys[i], old[i]) : nvs_erase_key(h, keys[i]);
                if (e != ESP_OK && !(e == ESP_ERR_NVS_NOT_FOUND && !have[i])) {
                    back = e;
                }
            }
        }
        if (back == ESP_OK) {
            back = nvs_commit(h);
        }
        if (back != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi: could not put the old credentials back (%s)", esp_err_to_name(back));
        }
    }
    nvs_close(h);
    mbedtls_platform_zeroize(old_ssid, sizeof old_ssid);
    mbedtls_platform_zeroize(old_pass, sizeof old_pass);
    if (s_store_lock) {
        xSemaphoreGive(s_store_lock);
    }
    return err;
}

/* Stored → used; else the build's, stored once. True if a pair results.
 * The only place the build's password is read. */
static bool wifi_load_or_migrate(char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    if (wifi_load(ssid, ssid_n, pass, pass_n)) {
        return true;
    }
    if (CONFIG_C6_WIFI_SSID[0] == '\0') {
        return false;
    }
    strlcpy(ssid, CONFIG_C6_WIFI_SSID, ssid_n);
    strlcpy(pass, CONFIG_C6_WIFI_PASS, pass_n);
    esp_err_t err = wifi_store(ssid, pass);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WIFI credentials migrated from the build");
    } else {
        ESP_LOGW(TAG, "Wi-Fi: the build's credentials could not be stored (%s); using them this boot",
                 esp_err_to_name(err));
    }
    return true;
}

/* -------------------------------------------------------------- hotspot */

/* 192.168.44.1/24: the ESP default AP subnet is a common home subnet. Stop → set → start, as
 * esp_netif.h requires; before the AP's first start the stop reports ALREADY_STOPPED or
 * IF_NOT_READY and the start only records the wish, which the AP_START glue then honours. */
static void wifi_ap_address(esp_netif_t *ap)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_set_ip4_addr(&ip.ip, 192, 168, 44, 1);
    esp_netif_set_ip4_addr(&ip.gw, 192, 168, 44, 1);
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    esp_err_t err = esp_netif_dhcps_stop(ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED && err != ESP_ERR_ESP_NETIF_IF_NOT_READY) {
        ESP_LOGW(TAG, "hotspot: dhcps stop: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(ap, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hotspot: address not set (%s); the ESP default stays", esp_err_to_name(err));
    }
    err = esp_netif_dhcps_start(ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "hotspot: dhcps start: %s", esp_err_to_name(err));
    }
}

static void wifi_ap_bring_up(void)
{
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        wifi_ap_address(s_ap_netif);
    }
    char ssid[WIFI_SSID_MAX + 1];
    wifi_ap_ssid(ssid, sizeof ssid, c6_base_host());
    wifi_ap_record_t sta = {0};
    uint8_t sta_ch = (esp_wifi_sta_get_ap_info(&sta) == ESP_OK) ? sta.primary : 0;
    uint8_t ch = wifi_ap_channel(sta_ch);

    wifi_config_t wc = {0};
    size_t n = strlen(ssid);
    memcpy(wc.ap.ssid, ssid, MIN(n, sizeof wc.ap.ssid));
    wc.ap.ssid_len = (uint8_t)MIN(n, sizeof wc.ap.ssid);
    wc.ap.channel = ch;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    wc.ap.max_connection = WIFI_AP_MAX_CONN;
    /* the AP interface only takes a config in a mode that has it, so the mode goes first */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hotspot: not started: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    s_ap_up = true;
    c6_led_set(C6_LED_AP, true);
    ESP_LOGI(TAG, "hotspot %s up on channel %d at 192.168.44.1", ssid, ch);
    if (s_ap_cb != NULL) {
        s_ap_cb(true);
    }
}

void c6_wifi_ap_start(uint32_t seconds)
{
    /* the newest request sets the stop rule: 0 = until the STA has an IP plus 10 s, else that long */
    esp_timer_stop(s_ap_timer);
    s_ap_until_ip = (seconds == 0);
    uint64_t delay = wifi_ap_stop_delay_us(seconds, s_have_ip);
    if (delay > 0) {
        esp_timer_start_once(s_ap_timer, delay);
    }
    if (s_ap_up) {
        ESP_LOGI(TAG, "hotspot: already up; closes %s", seconds ? "after the requested time" : "10 s after an IP");
        return;
    }
    wifi_ap_bring_up();
}

void c6_wifi_ap_stop(void)
{
    if (!s_ap_up) {
        return;
    }
    esp_timer_stop(s_ap_timer);
    s_ap_up = false;
    s_ap_until_ip = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hotspot: back to STA: %s", esp_err_to_name(err));
    }
    c6_led_set(C6_LED_AP, false);
    ESP_LOGI(TAG, "hotspot down");
    if (s_ap_cb != NULL) {
        s_ap_cb(false);
    }
}

bool c6_wifi_ap_up(void)
{
    return s_ap_up;
}

void c6_wifi_on_ap(c6_wifi_ap_cb_t cb)
{
    s_ap_cb = cb;
}

/* --------------------------------------------------------------- timers */

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

/* CONFIG_C6_WIFI_FAIL_S with no IP: FAILED, and the hotspot until the station gets one. */
static void wifi_fail_cb(void *arg)
{
    if (s_have_ip) {
        return;
    }
    s_state = C6_WIFI_FAILED;
    ESP_LOGW(TAG, "Wi-Fi: no IP after %d s; opening the hotspot", CONFIG_C6_WIFI_FAIL_S);
    c6_wifi_ap_start(0);
}

static void wifi_ap_timer_cb(void *arg)
{
    c6_wifi_ap_stop();
}

/* --------------------------------------------------------------- events */

/* Starts and stops timers, flips flags, logs: nothing else. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_have_ip = false;
        c6_base_got_ip(false);
        if (s_state == C6_WIFI_CONNECTED) {
            s_state = C6_WIFI_CONNECTING;
        }
        if (s_have_creds) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, reason %d; retry in %lu ms", d->reason,
                     (unsigned long)s_backoff_ms);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms = wifi_next_backoff_ms(s_backoff_ms);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        ESP_LOGI(TAG, "Wi-Fi up: " IPSTR " as %s.local, rssi %d, channel %d",
                 IP2STR(&e->ip_info.ip), c6_base_host(), ap.rssi, ap.primary);
        s_backoff_ms = WIFI_BACKOFF_MIN_MS;
        s_have_ip = true;
        s_state = C6_WIFI_CONNECTED;
        esp_timer_stop(s_fail_timer);
        if (s_ap_up && s_ap_until_ip) {
            esp_timer_stop(s_ap_timer);
            esp_timer_start_once(s_ap_timer, wifi_ap_stop_delay_us(0, true));
        }
        c6_base_got_ip(true);
        c6_app_state_mark(C6_ST_BOARD);
    }
}

/* ---------------------------------------------------------------- start */

static void wifi_start_cb(void *arg)
{
    if (CONFIG_C6_WIFI_START_DELAY_MS > 0) {
        ESP_LOGI(TAG, "starting Wi-Fi after the configured %d ms delay", CONFIG_C6_WIFI_START_DELAY_MS);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

static void start_wifi(void)
{
    if (!s_store_lock) {
        s_store_lock = xSemaphoreCreateMutex();
    }
    ESP_ERROR_CHECK(s_store_lock ? ESP_OK : ESP_ERR_NO_MEM);
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_netif, c6_base_host()));
    /* the AP netif is made and addressed here, in the starting task, so the failure timer's
     * callback (the esp_timer task, a small stack) only switches the mode */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_ap_address(s_ap_netif);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    const esp_timer_create_args_t rt = {.callback = reconnect_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&rt, &s_reconnect_timer));
    const esp_timer_create_args_t ft = {.callback = wifi_fail_cb, .name = "wifi_fail"};
    ESP_ERROR_CHECK(esp_timer_create(&ft, &s_fail_timer));
    const esp_timer_create_args_t at = {.callback = wifi_ap_timer_cb, .name = "wifi_ap"};
    ESP_ERROR_CHECK(esp_timer_create(&at, &s_ap_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
    if (wifi_load_or_migrate(ssid, sizeof ssid, pass, sizeof pass)) {
        strlcpy(s_ssid, ssid, sizeof s_ssid);
        s_have_creds = true;
        s_state = C6_WIFI_CONNECTING;
        wifi_config_t wc;
        wifi_sta_config(&wc, ssid, pass);
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        memset(&wc, 0, sizeof wc);
        if (err != ESP_OK) {
            /* not fatal on purpose: the failure timer still opens the hotspot, and a fix can arrive */
            ESP_LOGE(TAG, "Wi-Fi: the stored credentials were refused (%s)", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Wi-Fi: connecting to %s", ssid);
        ESP_ERROR_CHECK(esp_timer_start_once(s_fail_timer, (uint64_t)CONFIG_C6_WIFI_FAIL_S * 1000000ULL));
    } else {
        s_state = C6_WIFI_NO_CREDENTIALS;
        ESP_LOGW(TAG, "Wi-Fi: no credentials; opening the hotspot");
        c6_wifi_ap_start(0);
    }
    memset(pass, 0, sizeof pass);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    /* Wi-Fi and 802.15.4 share the radio; modem sleep is what gives Zigbee its slots */
    esp_coex_wifi_i154_enable();
#endif
    if (CONFIG_C6_WIFI_START_DELAY_MS > 0) {
        /* a diagnostic: lets another radio user run alone first. Rollback still covers it,
         * as long as the delay stays well inside CONFIG_C6_VERIFY_TIMEOUT_S */
        static esp_timer_handle_t t;
        const esp_timer_create_args_t a = {.callback = wifi_start_cb, .name = "wifi_start"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &t));
        ESP_ERROR_CHECK(esp_timer_start_once(t, CONFIG_C6_WIFI_START_DELAY_MS * 1000ULL));
        return;
    }
    wifi_start_cb(NULL);
}

void c6_wifi_start(void)
{
    start_wifi();
}

esp_netif_t *c6_wifi_netif(void)
{
    return s_netif;
}

/* --------------------------------------------------------------- queries */

c6_wifi_state_t c6_wifi_state(void)
{
    return s_state;
}

bool c6_wifi_ssid(char *out, size_t n)
{
    if (out == NULL || n == 0) {
        return false;
    }
    strlcpy(out, s_ssid, n);
    return s_ssid[0] != '\0';
}

/* ------------------------------------------------------------------ set */

/* Validate, store (one commit), apply: stop the retry, disconnect, set_config, connect; backoff and
 * the failure timer start over. Disconnect comes first: while the station is still connecting (the
 * hotspot is in use then) the IDF refuses set_config with ESP_ERR_WIFI_STATE. Nothing is touched
 * on a bad value. From a task, never the stack's context. */
esp_err_t c6_wifi_set(const char *ssid, const char *pass)
{
    if (!wifi_ssid_ok(ssid) || !wifi_pass_ok(pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = wifi_store(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi: storing credentials failed: %s", esp_err_to_name(err));
        return err;
    }
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    s_have_creds = true;
    esp_timer_stop(s_reconnect_timer);
    esp_wifi_disconnect();
    wifi_config_t wc;
    wifi_sta_config(&wc, ssid, pass);
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi: new credentials refused by the driver: %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_connect();
    s_backoff_ms = WIFI_BACKOFF_MIN_MS;
    s_state = C6_WIFI_CONNECTING;
    esp_timer_stop(s_fail_timer);
    esp_timer_start_once(s_fail_timer, (uint64_t)CONFIG_C6_WIFI_FAIL_S * 1000000ULL);
    ESP_LOGI(TAG, "Wi-Fi: credentials set; connecting to %s", ssid);
    return ESP_OK;
}

/* ----------------------------------------------------------------- scan */

static bool wifi_scan_claim(void)
{
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = !s_scanning;
    if (ok) {
        s_scanning = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

/* Blocking active scan; the callback runs in the caller's task, once per result. */
esp_err_t c6_wifi_scan(c6_wifi_scan_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_scan_claim()) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t sc = {0};                /* active, show_hidden false, the driver's dwell times */
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi: scan failed: %s", esp_err_to_name(err));
        s_scanning = false;
        return err;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        esp_wifi_clear_ap_list();
        s_scanning = false;
        return ESP_OK;
    }
    if (n > WIFI_SCAN_FETCH_MAX) {
        n = WIFI_SCAN_FETCH_MAX;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof *recs);
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&n, recs);   /* also frees the driver's list */
    if (err == ESP_OK) {
        const wifi_ap_record_t *pick[WIFI_SCAN_MAX];
        int count = wifi_scan_pick(recs, n, pick, WIFI_SCAN_MAX);
        for (int i = 0; i < count; i++) {
            cb((const char *)pick[i]->ssid, pick[i]->rssi, pick[i]->authmode != WIFI_AUTH_OPEN, ctx);
        }
    }
    free(recs);
    s_scanning = false;
    return err;
}
