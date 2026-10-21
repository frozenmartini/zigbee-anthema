/*
 * c6_auth — the board's claim, the claim window and the auth classes of /api/v1 (protocol §3, §4).
 *
 * The claim: POST /claim (window open, unclaimed) makes 32 random bytes, returns them once as 64 lowercase
 * hex, and stores the SHA-256 of that hex string (its 64 bytes, not the raw 32) as a 32-byte blob in NVS
 * "c6claim"/"sha". A request proves the claim with "Authorization: Bearer <hex>": the board hashes what it
 * was given and compares the two digests over all 32 bytes, with no early exit. The token and the digests
 * never reach a log line; every buffer that held one is wiped after use.
 *
 * The window: open 600 s from boot (c6_auth_start), from a button press and from a POST /release
 * (c6_auth_window_open_for); a one-shot timer marks its close. The LED shows UNCLAIMED while the board
 * holds no claim, the window open or not.
 *
 * The developer credential: X-OTA-Token equal to CONFIG_C6_OTA_TOKEN, only while that is 16 characters or
 * more (empty or shorter: every value is refused). OWNER and TOKEN accept it, and so does c6_auth_dev_ok,
 * on a claimed or an unclaimed board, so a tool that sends the developer token works before and after a client claims the board.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "c6_auth.h"
#include "c6_base.h"
#include "c6_discovery.h"
#include "c6_led.h"

static const char *TAG = "auth";

#define AUTH_NS        "c6claim"
#define AUTH_KEY       "sha"
#define AUTH_DIGEST    32                  /* bytes of SHA-256 */
#define AUTH_RAW       32                  /* random bytes in a token */
#define AUTH_HEX       64                  /* characters of a token */
#define AUTH_WINDOW_S  600                 /* the claim window, from boot, a press, a release */
#define AUTH_DEV_MIN   16                  /* the developer credential's minimum length (today's) */
#define AUTH_HDR_MAX   96                  /* an Authorization value longer than this is a wrong one */

/* What the request presented in Authorization. */
typedef enum { AUTH_CRED_NONE, AUTH_CRED_WRONG, AUTH_CRED_GOOD } auth_cred_t;

/* What auth_decide says. */
typedef enum {
    AUTH_ALLOW,
    AUTH_ALREADY_CLAIMED,
    AUTH_WINDOW_CLOSED,
    AUTH_CLAIM_REQUIRED,
    AUTH_UNAUTHORIZED,
} auth_verdict_t;

static portMUX_TYPE s_auth_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_claimed;                     /* guarded by s_auth_mux */
static uint8_t s_digest[AUTH_DIGEST];      /* guarded by s_auth_mux; zero while unclaimed */
static int64_t s_window_until_us;          /* guarded by s_auth_mux; esp_timer time the window closes */
static esp_timer_handle_t s_window_timer;  /* marks the window's close (logged); the LED follows the claim */
static SemaphoreHandle_t s_lock;           /* one claim or clear at a time: the HTTP task, the button task */

/* ---------------------------------------------------------------- the pure parts */

/* Zero a buffer that held a token, a digest or a header, in a way the compiler keeps. */
static void auth_wipe(void *p, size_t n)
{
    mbedtls_platform_zeroize(p, n);
}

/* True only if the two digests are identical. Visits all 32 bytes, with no early exit. */
static bool auth_digest_equal(const uint8_t *a, const uint8_t *b)
{
    uint32_t diff = 0;
    for (size_t i = 0; i < AUTH_DIGEST; i++) {
        diff |= (uint32_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* True if got equals want. Visits every character of want whatever got is, with no early exit; a
 * different length is a difference. */
static bool auth_text_equal(const char *got, const char *want)
{
    size_t lg = strlen(got);
    size_t lw = strlen(want);
    uint32_t diff = (uint32_t)(lg ^ lw);
    for (size_t i = 0; i < lw; i++) {
        size_t j = i < lg ? i : lg;        /* got[lg] is its NUL: never read past it */
        diff |= (uint32_t)((uint8_t)got[j] ^ (uint8_t)want[i]);
    }
    return diff == 0;
}

/* The developer credential: the X-OTA-Token value got against the build's want. False whenever want is
 * shorter than 16 characters (a public build has it empty). */
static bool auth_dev_match(const char *got, const char *want)
{
    return strlen(want) >= AUTH_DEV_MIN && auth_text_equal(got, want);
}

/* The window at now_us, given the time it closes. */
static bool auth_window_is_open(int64_t now_us, int64_t until_us)
{
    return now_us < until_us;
}

/* The value after "Bearer " (the scheme in any case, one or more spaces), or NULL for another scheme. */
static const char *auth_bearer_value(const char *h)
{
    static const char scheme[] = "bearer";
    for (size_t i = 0; i < sizeof scheme - 1; i++) {
        if (tolower((unsigned char)h[i]) != scheme[i]) {
            return NULL;                   /* a shorter h stops here at its NUL */
        }
    }
    h += sizeof scheme - 1;
    if (*h != ' ') {
        return NULL;
    }
    while (*h == ' ') {
        h++;
    }
    return h;
}

/* 32 bytes to 64 lowercase hex and a NUL. */
static void auth_hex(const uint8_t *raw, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < AUTH_RAW; i++) {
        out[2 * i] = digits[raw[i] >> 4];
        out[2 * i + 1] = digits[raw[i] & 0x0f];
    }
    out[AUTH_HEX] = '\0';
}

/* The class table (protocol §4). bearer: what Authorization held (GOOD only on a claimed board whose
 * digest matched); dev: the developer credential matched. */
static auth_verdict_t auth_decide(c6_auth_class_t cls, bool claimed, bool window_open, auth_cred_t bearer,
                                  bool dev)
{
    switch (cls) {
    case C6_AUTH_NONE:
        return AUTH_ALLOW;
    case C6_AUTH_WINDOW:
        if (claimed) {
            return AUTH_ALREADY_CLAIMED;
        }
        return window_open ? AUTH_ALLOW : AUTH_WINDOW_CLOSED;
    case C6_AUTH_OWNER:
        if (dev) {
            return AUTH_ALLOW;
        }
        if (!claimed) {
            if (bearer != AUTH_CRED_NONE) {
                return AUTH_CLAIM_REQUIRED;    /* its claim was cleared: claim again (the client's reauth) */
            }
            return window_open ? AUTH_ALLOW : AUTH_CLAIM_REQUIRED;
        }
        return bearer == AUTH_CRED_GOOD ? AUTH_ALLOW : AUTH_UNAUTHORIZED;
    case C6_AUTH_TOKEN:
        if (dev) {
            return AUTH_ALLOW;
        }
        if (!claimed) {
            return AUTH_CLAIM_REQUIRED;
        }
        return bearer == AUTH_CRED_GOOD ? AUTH_ALLOW : AUTH_UNAUTHORIZED;
    default:
        return AUTH_UNAUTHORIZED;          /* not a class: refuse */
    }
}

/* ---------------------------------------------------------------- state */

bool c6_auth_claimed(void)
{
    portENTER_CRITICAL(&s_auth_mux);
    bool c = s_claimed;
    portEXIT_CRITICAL(&s_auth_mux);
    return c;
}

bool c6_auth_window_open(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_auth_mux);
    int64_t until = s_window_until_us;
    portEXIT_CRITICAL(&s_auth_mux);
    return auth_window_is_open(now, until);
}

/* UNCLAIMED on while the board holds no claim, off once a client has claimed it. */
static void auth_led_update(void)
{
    c6_led_set(C6_LED_UNCLAIMED, !c6_auth_claimed());
}

/* The window timer fired: the window has closed. */
static void auth_window_cb(void *arg)
{
    (void)arg;
    auth_led_update();
    ESP_LOGI(TAG, "AUTH claim window closed");
}

void c6_auth_window_open_for(uint32_t seconds)
{
    int64_t until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    portENTER_CRITICAL(&s_auth_mux);
    s_window_until_us = until;
    portEXIT_CRITICAL(&s_auth_mux);
    if (s_window_timer) {
        esp_timer_stop(s_window_timer);    /* not running: ESP_ERR_INVALID_STATE, nothing to stop */
        esp_timer_start_once(s_window_timer, (uint64_t)seconds * 1000000);
    }
    auth_led_update();
    ESP_LOGI(TAG, "AUTH claim window open for %lu s", (unsigned long)seconds);
}

/* The stored digest at boot. A blob of any other length than 32 loads as unclaimed. One log line. */
static void auth_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTH_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "AUTH unclaimed");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AUTH claim not readable (%s); unclaimed", esp_err_to_name(err));
        return;
    }
    size_t len = 0;
    err = nvs_get_blob(h, AUTH_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "AUTH unclaimed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "AUTH claim not readable (%s); unclaimed", esp_err_to_name(err));
    } else if (len != AUTH_DIGEST) {
        ESP_LOGE(TAG, "AUTH stored claim is %u bytes, not %d; unclaimed", (unsigned)len, AUTH_DIGEST);
    } else {
        uint8_t d[AUTH_DIGEST];
        err = nvs_get_blob(h, AUTH_KEY, d, &len);
        if (err == ESP_OK && len == AUTH_DIGEST) {
            portENTER_CRITICAL(&s_auth_mux);
            memcpy(s_digest, d, AUTH_DIGEST);
            s_claimed = true;
            portEXIT_CRITICAL(&s_auth_mux);
            ESP_LOGI(TAG, "AUTH claimed");
        } else {
            ESP_LOGE(TAG, "AUTH claim not readable (%s); unclaimed", esp_err_to_name(err));
        }
        auth_wipe(d, sizeof d);
    }
    nvs_close(h);
}

void c6_auth_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_window_timer) {
        const esp_timer_create_args_t a = {.callback = auth_window_cb, .name = "claim_window"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_window_timer));
    }
    auth_load();
    c6_auth_window_open_for(AUTH_WINDOW_S);
}

static void auth_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void auth_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* Store d as c6claim/sha and commit. */
static esp_err_t auth_store(const uint8_t *d)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTH_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, AUTH_KEY, d, AUTH_DIGEST);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t c6_auth_claim(char token_hex[65])
{
    token_hex[0] = '\0';
    auth_lock();
    if (c6_auth_claimed()) {
        auth_unlock();
        return ESP_ERR_INVALID_STATE;      /* before any random byte is made */
    }
    uint8_t raw[AUTH_RAW];
    uint8_t d[AUTH_DIGEST];
    esp_fill_random(raw, sizeof raw);
    auth_hex(raw, token_hex);
    auth_wipe(raw, sizeof raw);
    esp_err_t err = mbedtls_sha256((const unsigned char *)token_hex, AUTH_HEX, d, 0) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        err = auth_store(d);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_auth_mux);
        memcpy(s_digest, d, AUTH_DIGEST);
        s_claimed = true;
        portEXIT_CRITICAL(&s_auth_mux);
    }
    auth_wipe(d, sizeof d);
    auth_unlock();
    if (err != ESP_OK) {
        auth_wipe(token_hex, AUTH_HEX + 1);
        ESP_LOGE(TAG, "AUTH claim not stored (%s); still unclaimed", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "AUTH claimed over HTTP");
    auth_led_update();
    c6_discovery_update();
    return ESP_OK;
}

esp_err_t c6_auth_clear(void)
{
    auth_lock();
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTH_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, AUTH_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;                  /* nothing stored: already clear */
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;                      /* the namespace was never written */
    }
    if (err != ESP_OK) {
        auth_unlock();
        ESP_LOGE(TAG, "AUTH claim not erased from storage (%s); claim kept", esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_auth_mux);
    s_claimed = false;
    memset(s_digest, 0, sizeof s_digest);
    portEXIT_CRITICAL(&s_auth_mux);
    auth_unlock();
    ESP_LOGW(TAG, "AUTH claim cleared");
    auth_led_update();
    c6_auth_window_open_for(AUTH_WINDOW_S);
    c6_app_events_close();
    c6_discovery_update();
    return ESP_OK;
}

/* ---------------------------------------------------------------- requests */

/* What the request's Authorization header proves. GOOD only on a claimed board whose digest matches. */
static auth_cred_t auth_bearer(httpd_req_t *req)
{
    size_t n = httpd_req_get_hdr_value_len(req, "Authorization");
    if (n == 0) {
        return AUTH_CRED_NONE;
    }
    char h[AUTH_HDR_MAX];
    if (n >= sizeof h || httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof h) != ESP_OK) {
        return AUTH_CRED_WRONG;            /* too long to be ours */
    }
    const char *v = auth_bearer_value(h);
    if (!v) {
        auth_wipe(h, sizeof h);
        return AUTH_CRED_NONE;             /* another scheme is no credential */
    }
    uint8_t stored[AUTH_DIGEST];
    portENTER_CRITICAL(&s_auth_mux);
    bool claimed = s_claimed;
    memcpy(stored, s_digest, AUTH_DIGEST);
    portEXIT_CRITICAL(&s_auth_mux);
    bool good = false;
    if (claimed) {
        uint8_t got[AUTH_DIGEST];
        good = mbedtls_sha256((const unsigned char *)v, strlen(v), got, 0) == 0 &&
               auth_digest_equal(got, stored);
        auth_wipe(got, sizeof got);
    }
    auth_wipe(stored, sizeof stored);
    auth_wipe(h, sizeof h);
    return good ? AUTH_CRED_GOOD : AUTH_CRED_WRONG;
}

/* The developer credential in X-OTA-Token. */
_Static_assert(sizeof(CONFIG_C6_OTA_TOKEN) <= 80, "C6_OTA_TOKEN: at most 79 characters (read into got[80])");
static bool auth_dev_header(httpd_req_t *req)
{
    const char *want = CONFIG_C6_OTA_TOKEN;
    if (strlen(want) < AUTH_DEV_MIN) {
        return false;                      /* no developer credential in this build: refuse every value */
    }
    char got[80];
    bool ok = httpd_req_get_hdr_value_str(req, "X-OTA-Token", got, sizeof got) == ESP_OK &&
              auth_dev_match(got, want);
    auth_wipe(got, sizeof got);
    return ok;
}

bool c6_auth_dev_ok(httpd_req_t *req)
{
    return auth_dev_header(req) || auth_bearer(req) == AUTH_CRED_GOOD;
}

static const char *auth_class_name(c6_auth_class_t cls)
{
    switch (cls) {
    case C6_AUTH_NONE:   return "NONE";
    case C6_AUTH_WINDOW: return "WINDOW";
    case C6_AUTH_OWNER:  return "OWNER";
    case C6_AUTH_TOKEN:  return "TOKEN";
    default:             return "?";
    }
}

bool c6_auth_ok(httpd_req_t *req, c6_auth_class_t cls)
{
    if (cls == C6_AUTH_NONE) {
        return true;
    }
    bool claimed = c6_auth_claimed();
    bool open = c6_auth_window_open();
    auth_cred_t bearer = AUTH_CRED_NONE;
    bool dev = false;
    if (cls == C6_AUTH_OWNER || cls == C6_AUTH_TOKEN) {
        dev = auth_dev_header(req);
        bearer = auth_bearer(req);
    }
    auth_verdict_t v = auth_decide(cls, claimed, open, bearer, dev);
    if (v == AUTH_ALLOW) {
        return true;
    }
    const char *code;
    switch (v) {
    case AUTH_ALREADY_CLAIMED:
        code = "already_claimed";
        c6_json_error(req, 409, code, "the board is claimed; to claim it again, hold its button 10 s first");
        break;
    case AUTH_WINDOW_CLOSED:
        code = "window_closed";
        c6_json_error(req, 409, code, "the claim window is closed; press the board's button, then try again");
        break;
    case AUTH_CLAIM_REQUIRED:
        code = "claim_required";
        c6_json_error(req, 403, code, "the board is not claimed; claim it first");
        break;
    default:
        code = "unauthorized";
        c6_json_error(req, 401, code, "the token is missing or wrong");
        break;
    }
    ESP_LOGW(TAG, "AUTH %s request refused: %s", auth_class_name(cls), code);
    return false;
}
