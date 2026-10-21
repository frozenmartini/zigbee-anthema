/*
 * light — the two commands the client sends (protocol §6.1, §6.2; 0.5.0).
 *
 *   POST /api/v1/light           (token) {"on":0|1, "level":1-254, and at most one of "ct":1-65279,
 *                                "hs":[0-254,0-254], "xy":[0-65279,0-65279]}, at most 256 bytes. The mirror
 *                                role's report of the real light: set as API-SET, by "api".
 *   POST /api/v1/button/release  (token, no body) the shortcut has run: the held press is released.
 *
 * Order: the credential first (nothing is read before it), then the body, then the role and the echo window
 * (bulb_light_set, which holds bulb.c's statics). Every refusal is c6_json_error's JSON (protocol §7):
 *   400 bad_request   the body: not JSON, not an object, too long, a key unknown, given twice, of the wrong
 *                     type or out of range, two colour keys, "on" missing; the message names the key
 *   409 wrong_role    /light in the shortcut role; /button/release outside it
 *   409 no_press      /button/release with nothing held
 *   503 zigbee_busy   the stack is not up or its lock was busy; for a release, also an Off refused (the press
 *                     stays held and its own timeout retries)
 * Answers that are not refusals: 200 {"ok":true}; 200 {"ok":false,"ignored":"controller_command_3s"} within 3 s of a
 * Zigbee controller command (the echo window, the client's loop guard); 200 {"ok":false,"ignored":"<light_apply's message>"} when an
 * attribute was refused (as /zb/light answers 200 with each status).
 *
 * The body gets its own nesting check, not settings.c's (which refuses every '['): '{' only at the top and '['
 * only one level in, so cJSON recurses at most twice. A key name is echoed in a message only when it is a
 * plain name (letters, digits, '_', at most 16).
 */
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "c6_base.h"
#include "c6_auth.h"
#include "state.h"

static const char *TAG = "light";

#define LIGHT_BODY_MAX 256                     /* bytes of JSON a POST /api/v1/light may carry */

/* The keys, by bit; the three colour keys exclude each other. */
static const char *const LIGHT_KEYS[] = {"on", "level", "ct", "hs", "xy"};
#define LIGHT_KEY_COUNT (sizeof LIGHT_KEYS / sizeof LIGHT_KEYS[0])
#define LIGHT_K_ON     (1u << 0)
#define LIGHT_K_COLOUR ((1u << 2) | (1u << 3) | (1u << 4))

/* ---------------------------------------------------------------- the body */

/* s[0..n) into out when it is a plain name (1-16 of letters, digits, '_'); else `other`. */
static const char *light_field(const char *s, size_t n, const char *other, char *out, size_t osz)
{
    if (!s || n == 0 || n > 16 || n >= osz) {
        return other;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) {
            return other;
        }
    }
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* True if body nests no deeper than the object and one array inside it, and no string can decode to a NUL.
 * Counts '{' and '[' outside JSON strings, backslash escapes honoured: '{' is allowed only at the top, '[' only
 * one level in. On false, err names the key whose value broke it (the last string seen at the top level), or
 * the body. */
static bool light_nesting_ok(const char *body, char *err, size_t esz)
{
    int depth = 0;
    bool in_str = false;
    const char *str = body, *key = NULL;
    size_t key_len = 0;
    char f[20];
    for (const char *c = body; *c; c++) {
        if (in_str) {
            if (*c == '\\' && c[1]) {
                if (c[1] == 'u' && strncmp(c + 2, "0000", 4) == 0) {
                    snprintf(err, esz, "%s: no NUL characters", light_field(key, key_len, "body", f, sizeof f));
                    return false;
                }
                c++;
            } else if (*c == '"') {
                in_str = false;
                if (depth == 1) {
                    key = str;
                    key_len = (size_t)(c - str);
                }
            }
        } else if (*c == '"') {
            in_str = true;
            str = c + 1;
        } else if (*c == '{' || *c == '[') {
            if ((*c == '{' && depth != 0) || (*c == '[' && depth != 1)) {
                snprintf(err, esz, "%s: nested too deep (an object of numbers; hs and xy two numbers)",
                         depth ? light_field(key, key_len, "body", f, sizeof f) : "body");
                return false;
            }
            depth++;
        } else if ((*c == '}' || *c == ']') && depth > 0) {
            depth--;
        }
    }
    return true;
}

/* A whole number in [lo, hi] into *out. NaN fails the range test. */
static bool light_int(const cJSON *it, int lo, int hi, int *out)
{
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    double d = it->valuedouble;
    if (!(d >= lo && d <= hi)) {
        return false;
    }
    int v = (int)d;
    if ((double)v != d) {
        return false;
    }
    *out = v;
    return true;
}

/* An array of exactly two whole numbers, each in [0, hi]. */
static bool light_pair(const cJSON *it, int hi, int *a, int *b)
{
    return cJSON_IsArray(it) && cJSON_GetArraySize(it) == 2 && light_int(cJSON_GetArrayItem(it, 0), 0, hi, a)
           && light_int(cJSON_GetArrayItem(it, 1), 0, hi, b);
}

/* One key of the body into want[] (SNAP_*); *keys collects the keys seen. False, with err naming the key, if
 * it is unknown, given twice, a second colour key, of the wrong type or out of range. */
static bool light_item(const cJSON *it, int want[SNAP_COUNT], uint32_t *keys, char *err, size_t esz)
{
    const char *k = it->string ? it->string : "";
    int idx = -1;
    for (size_t i = 0; i < LIGHT_KEY_COUNT; i++) {
        if (strcmp(k, LIGHT_KEYS[i]) == 0) {
            idx = (int)i;
        }
    }
    if (idx < 0) {
        char f[20];
        snprintf(err, esz, "%s: not a field (on, level, ct, hs, xy)", light_field(k, strlen(k), "a key", f, sizeof f));
        return false;
    }
    uint32_t bit = 1u << idx;
    if (*keys & bit) {
        snprintf(err, esz, "%s: given twice", k);
        return false;
    }
    if ((bit & LIGHT_K_COLOUR) && (*keys & LIGHT_K_COLOUR)) {
        snprintf(err, esz, "%s: at most one of ct, hs, xy", k);
        return false;
    }
    *keys |= bit;
    int a = 0, b = 0;
    switch (idx) {
    case 0:
        if (!light_int(it, 0, 1, &a)) {
            snprintf(err, esz, "on: 0 or 1");
            return false;
        }
        want[SNAP_ON] = a;
        return true;
    case 1:
        if (!light_int(it, 1, 254, &a)) {
            snprintf(err, esz, "level: a whole number, 1-254");
            return false;
        }
        want[SNAP_LEVEL] = a;
        return true;
    case 2:
        if (!light_int(it, 1, 0xFEFF, &a)) {
            snprintf(err, esz, "ct: mireds, a whole number, 1-65279");
            return false;
        }
        want[SNAP_CT] = a;
        want[SNAP_MODE] = 2;
        return true;
    case 3:
        if (!light_pair(it, 254, &a, &b)) {
            snprintf(err, esz, "hs: [hue, saturation], two whole numbers, 0-254");
            return false;
        }
        want[SNAP_HUE] = a;
        want[SNAP_SAT] = b;
        want[SNAP_MODE] = 0;
        return true;
    default:
        if (!light_pair(it, 0xFEFF, &a, &b)) {
            snprintf(err, esz, "xy: [x, y], two whole numbers, 0-65279");
            return false;
        }
        want[SNAP_X] = a;
        want[SNAP_Y] = b;
        want[SNAP_MODE] = 1;
        return true;
    }
}

/* A POST /api/v1/light body (len bytes, NUL-terminated) into want[] (SNAP_*, -1: leave as it is). All or
 * nothing: on false, err names the first bad key, or the body. */
static bool light_parse(const char *body, size_t len, int want[SNAP_COUNT], char *err, size_t esz)
{
    for (int i = 0; i < SNAP_COUNT; i++) {
        want[i] = -1;
    }
    if (len == 0 || len > LIGHT_BODY_MAX || memchr(body, 0, len)) {
        snprintf(err, esz, "body: a JSON object of at most %d bytes", LIGHT_BODY_MAX);
        return false;
    }
    if (!light_nesting_ok(body, err, esz)) {
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, esz, "body: not a JSON object");
        return false;
    }
    uint32_t keys = 0;
    bool ok = true;
    for (const cJSON *it = root->child; it && ok; it = it->next) {
        ok = light_item(it, want, &keys, err, esz);
    }
    cJSON_Delete(root);
    if (ok && !(keys & LIGHT_K_ON)) {
        snprintf(err, esz, "on: required, 0 or 1");
        ok = false;
    }
    if (!ok) {
        for (int i = 0; i < SNAP_COUNT; i++) {
            want[i] = -1;
        }
    }
    return ok;
}

/* ---------------------------------------------------------------- HTTP */

/* 200 {"ok":true}, or {"ok":false,"ignored":"<why>"} when `ignored`. `ignored` is this file's own text or
 * light_apply's (attribute names, numbers, parentheses): nothing to escape. */
static esp_err_t light_send_ok(httpd_req_t *req, const char *ignored)
{
    char buf[176];
    int n = ignored ? snprintf(buf, sizeof buf, "{\"ok\":false,\"ignored\":\"%s\"}", ignored)
                    : snprintf(buf, sizeof buf, "{\"ok\":true}");
    if (n < 0 || (size_t)n >= sizeof buf) {
        n = snprintf(buf, sizeof buf, "{\"ok\":false,\"ignored\":\"not all set\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

esp_err_t light_v1_post(httpd_req_t *req)
{
    static char body[LIGHT_BODY_MAX + 1];         /* the server runs one handler at a time */
    char err[96];
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;                            /* refused before a byte of the body is read */
    }
    if (req->content_len == 0 || req->content_len > LIGHT_BODY_MAX) {
        ESP_LOGW(TAG, "LIGHT-API refused: a body of %u bytes", (unsigned)req->content_len);
        return c6_json_error(req, 400, "bad_request", "body: a JSON object of at most 256 bytes");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            ESP_LOGW(TAG, "LIGHT-API: the body was not received (%d)", r);
            return ESP_FAIL;                      /* the server closes the connection */
        }
        got += (size_t)r;
    }
    body[got] = 0;
    int want[SNAP_COUNT];
    if (!light_parse(body, got, want, err, sizeof err)) {
        ESP_LOGW(TAG, "LIGHT-API refused: %s", err);
        return c6_json_error(req, 400, "bad_request", err);
    }
    char msg[120];
    msg[0] = 0;
    switch (bulb_light_set(want, msg, sizeof msg)) {
    case BULB_CMD_OK:
        return light_send_ok(req, NULL);
    case BULB_CMD_ECHO:
        return light_send_ok(req, "controller_command_3s");
    case BULB_CMD_REFUSED:
        return light_send_ok(req, msg);
    case BULB_CMD_WRONG_ROLE:
        ESP_LOGI(TAG, "LIGHT-API refused: the shortcut role takes /api/v1/button/release");
        return c6_json_error(req, 409, "wrong_role", "the board is in the shortcut role; it takes /button/release");
    default:
        return c6_json_error(req, 503, "zigbee_busy", "the Zigbee stack is not up or is busy; try again");
    }
}

esp_err_t button_release_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    char msg[60];
    msg[0] = 0;
    switch (bulb_button_release(msg, sizeof msg)) {
    case BULB_CMD_OK:
        return light_send_ok(req, NULL);
    case BULB_CMD_WRONG_ROLE:
        return c6_json_error(req, 409, "wrong_role", "the board is not in the shortcut role");
    case BULB_CMD_NO_PRESS:
        return c6_json_error(req, 409, "no_press", "no press is held");
    default:
        return c6_json_error(req, 503, "zigbee_busy",
                             "the Off could not be set (the Zigbee stack is busy or refused it); the press stays held");
    }
}
