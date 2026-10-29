/*
 * settings — the board's settings, stored in NVS and changed live over HTTP (0.4.0).
 *
 * One c6_bulb binary serves every board: what used to be the button-mode option, the Kconfig webhook
 * URL and the watchdog constant are now these settings, in NVS namespace "c6cfg" of the
 * existing "nvs" partition:
 *
 *   role         u8   0 none, 1 mirror, 2 shortcut                      default 0 none
 *   webhook      str  the webhook URL; empty: no pushes   default empty
 *   rx_wd_s      u32  RX-silence watchdog; 0: off                      default CONFIG_C6_ZB_RX_WATCHDOG_S
 *   ext_pan      str  the only extended PAN ID to join, 16 hex MSB first; empty: any network
 *                                                                       default CONFIG_C6_ZB_EXT_PANID
 *   name         str  the board's name: 1-40 bytes, no control character (0.5.0)
 *                                                    default "Zigbee Anthema <id>", computed (not stored)
 *   hue          u8   the hue answer; 1: CurrentHue served, 0: answered 0x86   default 1
 *                     (applies at the next reboot or /zigbee/erase; true or false on the wire, 0.7.0)
 *
 * ext_pan (0.5.0) is read by zb_task once, before the stack starts: a change is stored and answered at
 * once, and takes effect at the next reboot or POST /api/v1/zigbee/erase. Until then PUT's answer
 * carries "pending":["ext_pan"]. A stored empty value beats the default.
 *
 * hue (0.7.0) is read the same way, once, by create_device before the stack starts, and "pending" lists
 * it the same way (["ext_pan"], ["hue"] or ["ext_pan","hue"]); the board does not reboot for it. On, the
 * Color Control cluster serves CurrentHue and CurrentSaturation; off, the Zigbee controller's read of CurrentHue is
 * answered 0x86 (unsupported attribute), which the Zigbee controller takes as brightness only.
 *
 * name (0.5.0) is the mDNS instance name and the one the client shows: a change is passed to
 * c6_discovery_set_name and marks the state object's settings and board keys. A role change asks
 * c6_discovery_update for the TXT record, from settings_put after the mux.
 *
 * Read once at boot (cfg_load) into cfg_live, which bulb.c's s_light_mux guards. An absent key is not an
 * error: its default applies, and GET shows which keys are stored.
 *
 *   GET /api/v1/settings   {"role":..,"webhook":"<masked>","rx_wd_s":..,..,"hue":true,"stored":[..]} (token;
 *                          the state object's "settings" is the same JSON without "stored", cfg_json_add)
 *   PUT /api/v1/settings   a JSON body of at most 512 bytes with any of the keys above (token). Every
 *                          key is validated before anything is written: all or nothing. "webhook":"-"
 *                          clears the URL. A role change goes through bulb_role_switch (an Off first).
 *                          Answers with the GET JSON, plus "pending" listing ext_pan and/or hue while
 *                          either differs from the value the stack started with ("hue" takes true or
 *                          false only). Errors: {"error":"<code>","message":"<text>"}
 *                          (c6_json_error; 0.5.0: both handlers authorise with c6_auth_ok, class token).
 *
 * The webhook URL never appears in a log line: CFG logs it as "set" or "cleared", and GET shows it with
 * its last path segment cut to 4 characters and "…".
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "http_parser.h"
#include "c6_base.h"
#include "c6_auth.h"
#include "c6_discovery.h"
#include "settings.h"
#include "state.h"

static const char *TAG = "cfg";

#define CFG_NVS_NS   "c6cfg"
#define CFG_BODY_MAX 512                   /* bytes of JSON a PUT may carry */

/* Indexed by the bit number of CFG_K_*; also the NVS key and the JSON key. */
static const char *const CFG_KEY_NAMES[CFG_K_COUNT] = {
    "role", "webhook", "rx_wd_s", "ext_pan", "name", "hue",
};

/* The default ext_pan is copied into cfg_t.ext_pan[17] as it is: empty or 16 characters (its digits are
 * checked by zb_task, which stops the boot on a bad pin as 0.4.0 did). */
_Static_assert(sizeof(CONFIG_C6_ZB_EXT_PANID) == 1 || sizeof(CONFIG_C6_ZB_EXT_PANID) == 17,
               "CONFIG_C6_ZB_EXT_PANID: empty or 16 hex digits");

/* The one number: where it lives in cfg_t, its default, and its valid range. zero_ok: 0 is allowed
 * below lo (off / never). */
static const struct {
    const char *key;
    uint32_t bit;
    size_t off;
    uint32_t def, lo, hi;
    bool zero_ok;
    const char *range;
} CFG_NUMS[] = {
    {"rx_wd_s",     CFG_K_RX_WD_S,     offsetof(cfg_t, rx_wd_s),     (uint32_t)CONFIG_C6_ZB_RX_WATCHDOG_S,
     30, 3600, true, "0 or 30-3600"},
};
#define CFG_NUM_COUNT (sizeof CFG_NUMS / sizeof CFG_NUMS[0])

cfg_t cfg_live = {
    .role = CFG_ROLE_NONE,
    .rx_wd_s = (uint32_t)CONFIG_C6_ZB_RX_WATCHDOG_S,
    .ext_pan = CONFIG_C6_ZB_EXT_PANID,
    .hue = true,
};
/* The effective ext_pan when cfg_load ran, i.e. the one the stack starts with (s_light_mux). */
static char s_ext_pan_boot[17];
/* The hue answer when cfg_load ran, the one the stack starts with (s_light_mux). */
static bool s_hue_boot;

/* ---------------------------------------------------------------- pure helpers */

static uint32_t cfg_num_get(const cfg_t *c, size_t i)
{
    uint32_t v;
    memcpy(&v, (const char *)c + CFG_NUMS[i].off, sizeof v);
    return v;
}

static void cfg_num_set(cfg_t *c, size_t i, uint32_t v)
{
    memcpy((char *)c + CFG_NUMS[i].off, &v, sizeof v);
}

/* True if d is a whole number in CFG_NUMS[i]'s range; the value in *out. NaN fails the first test. */
static bool cfg_num_ok(double d, size_t i, uint32_t *out)
{
    if (!(d >= 0 && d <= CFG_NUMS[i].hi)) {
        return false;
    }
    uint32_t v = (uint32_t)d;
    if ((double)v != d) {
        return false;
    }
    if (v == 0 ? !(CFG_NUMS[i].zero_ok || CFG_NUMS[i].lo == 0) : v < CFG_NUMS[i].lo) {
        return false;
    }
    *out = v;
    return true;
}

const char *cfg_role_name(uint8_t role)
{
    static const char *const names[CFG_ROLE_COUNT] = {"none", "mirror", "shortcut"};
    return role < CFG_ROLE_COUNT ? names[role] : "?";
}

/* The role for a word; -1 if it is not one. */
static int cfg_role_parse(const char *s)
{
    for (int i = 0; i < CFG_ROLE_COUNT; i++) {
        if (strcmp(s, cfg_role_name((uint8_t)i)) == 0) {
            return i;
        }
    }
    return -1;
}

/* Empty, or http:// or https:// followed by at least one character, CFG_WEBHOOK_MAX characters in all,
 * printable ASCII only: no whitespace, no control characters. Not ending in '/', and one
 * http_parser_parse_url accepts: esp_http_client logs a URL it cannot parse, whole. */
static bool cfg_webhook_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0) {
        return true;
    }
    size_t p = strncmp(s, "http://", 7) == 0 ? 7 : strncmp(s, "https://", 8) == 0 ? 8 : 0;
    if (!p || n <= p || n > CFG_WEBHOOK_MAX || s[n - 1] == '/') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch >= 0x7F) {
            return false;
        }
    }
    struct http_parser_url u;
    http_parser_url_init(&u);
    return http_parser_parse_url(s, n, 0, &u) == 0;
}

/* GET: the last nonempty path segment cut to its first 4 characters and "…", without query or fragment. */
static void cfg_mask_webhook(const char *url, char *out, size_t sz)
{
    if (!url[0]) {
        out[0] = 0;
        return;
    }
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url, strlen(url), 0, &u) != 0) {
        out[0] = 0;
        return;
    }
    size_t head, tail = 0;
    if (u.field_set & (1u << UF_PATH)) {
        size_t start = u.field_data[UF_PATH].off;
        size_t end = start + u.field_data[UF_PATH].len;
        while (end > start && url[end - 1] == '/') {
            end--;
        }
        head = end;
        while (head > start && url[head - 1] != '/') {
            head--;
        }
        tail = end - head;
    } else {
        head = strlen(url);
        if (u.field_set & (1u << UF_QUERY)) {
            head = (size_t)u.field_data[UF_QUERY].off - 1;
        }
        if ((u.field_set & (1u << UF_FRAGMENT)) && (size_t)u.field_data[UF_FRAGMENT].off - 1 < head) {
            head = (size_t)u.field_data[UF_FRAGMENT].off - 1;
        }
    }
    if (tail > 4) {
        tail = 4;
    }
    /* Query and fragment text may contain the id too; neither belongs in the masked display. */
    snprintf(out, sz, "%.*s%.*s\xE2\x80\xA6", (int)head, url, (int)tail, url + head);
}

/* Empty, or exactly 16 hex digits in either case: an extended PAN ID, MSB first. */
static bool cfg_ext_pan_ok(const char *s)
{
    size_t n = strnlen(s, 17);
    if (n != 0 && n != 16) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* 1 to CFG_NAME_MAX bytes, none of them a control character (below 0x20, or 0x7F): the board's name. */
static bool cfg_name_ok(const char *s)
{
    size_t n = strnlen(s, CFG_NAME_MAX + 1);
    if (n == 0 || n > CFG_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7F) {
            return false;
        }
    }
    return true;
}

/* The name when none is stored: "Zigbee Anthema <id>", <id> the six hex of c6_base_host() ("c6-<id>",
 * the MAC's last three bytes). */
static void cfg_default_name(char *out, size_t sz)
{
    const char *h = c6_base_host();
    snprintf(out, sz, "Zigbee Anthema %s", strncmp(h, "c6-", 3) == 0 ? h + 3 : h);
}

/* "role webhook ..." for the keys in `keys`; "-" for none. */
static void cfg_key_list(uint32_t keys, char *out, size_t sz)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < CFG_K_COUNT; i++) {
        if ((keys & (1u << i)) && n < sz) {
            n += (size_t)snprintf(out + n, sz - n, "%s%s", n ? " " : "", CFG_KEY_NAMES[i]);
        }
    }
    if (!n) {
        snprintf(out, sz, "-");
    }
}

/* One key of a PUT body into next. False, with the reason in err (naming the key), if it is not a
 * setting, is given twice, has the wrong JSON type or is out of range. */
static bool cfg_parse_item(const cJSON *it, cfg_t *next, uint32_t *keys, char *err, size_t esz)
{
    const char *k = it->string ? it->string : "";
    int idx = -1;
    for (int i = 0; i < CFG_K_COUNT; i++) {
        if (strcmp(k, CFG_KEY_NAMES[i]) == 0) {
            idx = i;
        }
    }
    if (idx < 0) {
        snprintf(err, esz, "%.40s: not a setting", k);
        return false;
    }
    uint32_t bit = 1u << idx;
    if (*keys & bit) {
        snprintf(err, esz, "%s: given twice", k);
        return false;
    }
    *keys |= bit;
    if (bit == CFG_K_ROLE) {
        int r = cJSON_IsString(it) ? cfg_role_parse(it->valuestring) : -1;
        if (r < 0) {
            snprintf(err, esz, "role: none, mirror or shortcut");
            return false;
        }
        next->role = (uint8_t)r;
        return true;
    }
    if (bit == CFG_K_WEBHOOK) {
        const char *s = cJSON_IsString(it) ? it->valuestring : NULL;
        if (s && strcmp(s, "-") == 0) {
            s = "";                               /* "-" clears it */
        }
        if (!s || !cfg_webhook_ok(s)) {
            snprintf(err, esz, "webhook: \"-\", empty, or http:// or https:// and at most %d characters, "
                     "no spaces", CFG_WEBHOOK_MAX);
            return false;
        }
        snprintf(next->webhook, sizeof next->webhook, "%s", s);
        return true;
    }
    if (bit == CFG_K_EXT_PAN) {
        const char *s = cJSON_IsString(it) ? it->valuestring : NULL;
        if (!s || !cfg_ext_pan_ok(s)) {
            snprintf(err, esz, "ext_pan: empty, or 16 hex digits (the extended PAN ID, MSB first)");
            return false;
        }
        snprintf(next->ext_pan, sizeof next->ext_pan, "%s", s);
        return true;
    }
    if (bit == CFG_K_NAME) {
        const char *s = cJSON_IsString(it) ? it->valuestring : NULL;
        if (!s || !cfg_name_ok(s)) {
            snprintf(err, esz, "name: 1-%d characters, no control characters", CFG_NAME_MAX);
            return false;
        }
        snprintf(next->name, sizeof next->name, "%s", s);
        return true;
    }
    if (bit == CFG_K_HUE) {
        if (!cJSON_IsBool(it)) {
            snprintf(err, esz, "hue: true or false");
            return false;
        }
        next->hue = cJSON_IsTrue(it);
        return true;
    }
    for (size_t i = 0; i < CFG_NUM_COUNT; i++) {
        if (CFG_NUMS[i].bit == bit) {
            uint32_t v;
            if (!cJSON_IsNumber(it) || !cfg_num_ok(it->valuedouble, i, &v)) {
                snprintf(err, esz, "%s: a whole number, %s", k, CFG_NUMS[i].range);
                return false;
            }
            cfg_num_set(next, i, v);
            return true;
        }
    }
    snprintf(err, esz, "%s: not a setting", k);
    return false;
}

/* True if the body nests no deeper than one level and strings cannot decode to an embedded NUL.
 * Counts { and [ outside JSON strings, backslash escapes honoured. Run before cJSON parses, which
 * recurses once per level (x4), and decodes \u0000 into C strings that would hide the suffix. */
static bool cfg_nesting_ok(const char *body)
{
    int depth = 0;
    bool in_str = false;
    for (const char *c = body; *c; c++) {
        if (in_str) {
            if (*c == '\\' && c[1]) {
                if (c[1] == 'u' && strncmp(c + 2, "0000", 4) == 0) {
                    return false;
                }
                c++;
            } else if (*c == '"') {
                in_str = false;
            }
        } else if (*c == '"') {
            in_str = true;
        } else if (*c == '{' || *c == '[') {
            if (++depth > 1) {
                return false;
            }
        } else if ((*c == '}' || *c == ']') && depth > 0) {
            depth--;
        }
    }
    return true;
}

/* A PUT body: next = cur with every key present applied, *keys = the keys present. All or nothing: on
 * false, err names the first bad key and the caller writes nothing. */
static bool cfg_parse(const char *body, const cfg_t *cur, cfg_t *next, uint32_t *keys, char *err, size_t esz)
{
    *next = *cur;
    *keys = 0;
    if (!cfg_nesting_ok(body)) {
        snprintf(err, esz, "body: a JSON object with no nested objects, arrays or NUL characters");
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, esz, "body: not a JSON object");
        return false;
    }
    bool ok = true;
    for (const cJSON *it = root->child; it && ok; it = it->next) {
        ok = cfg_parse_item(it, next, keys, err, esz);
    }
    cJSON_Delete(root);
    return ok;
}

/* ---------------------------------------------------------------- load, get, store */

void cfg_load(void)
{
    cfg_t c = {.role = CFG_ROLE_NONE};
    for (size_t i = 0; i < CFG_NUM_COUNT; i++) {
        cfg_num_set(&c, i, CFG_NUMS[i].def);
    }
    c.hue = true;
    snprintf(c.ext_pan, sizeof c.ext_pan, "%s", CONFIG_C6_ZB_EXT_PANID);
    cfg_default_name(c.name, sizeof c.name);
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t role;
        esp_err_t e = nvs_get_u8(h, "role", &role);
        if (e == ESP_OK && role < CFG_ROLE_COUNT) {
            c.role = role;
            c.stored |= CFG_K_ROLE;
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "CFG role: stored value not usable (%s); default used",
                     e == ESP_OK ? "out of range" : esp_err_to_name(e));
        }
        size_t len = sizeof c.webhook;
        e = nvs_get_str(h, "webhook", c.webhook, &len);
        if (e == ESP_OK && cfg_webhook_ok(c.webhook)) {
            c.stored |= CFG_K_WEBHOOK;
        } else {
            c.webhook[0] = 0;
            if (e != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "CFG webhook: stored value not usable (%s); default used",
                         e == ESP_OK ? "not a valid URL" : esp_err_to_name(e));
            }
        }
        for (size_t i = 0; i < CFG_NUM_COUNT; i++) {
            uint32_t v, w;
            e = nvs_get_u32(h, CFG_NUMS[i].key, &v);
            if (e == ESP_OK && cfg_num_ok((double)v, i, &w)) {
                cfg_num_set(&c, i, w);
                c.stored |= CFG_NUMS[i].bit;
            } else if (e != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "CFG %s: stored value not usable (%s); default used", CFG_NUMS[i].key,
                         e == ESP_OK ? "out of range" : esp_err_to_name(e));
            }
        }
        char ep[sizeof c.ext_pan];
        len = sizeof ep;
        e = nvs_get_str(h, "ext_pan", ep, &len);
        if (e == ESP_OK && cfg_ext_pan_ok(ep)) {
            snprintf(c.ext_pan, sizeof c.ext_pan, "%s", ep);
            c.stored |= CFG_K_EXT_PAN;
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "CFG ext_pan: stored value not usable (%s); default used",
                     e == ESP_OK ? "not empty or 16 hex digits" : esp_err_to_name(e));
        }
        char nm[sizeof c.name];
        len = sizeof nm;
        e = nvs_get_str(h, "name", nm, &len);
        if (e == ESP_OK && cfg_name_ok(nm)) {
            memcpy(c.name, nm, sizeof c.name);
            c.stored |= CFG_K_NAME;
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "CFG name: stored value not usable (%s); default used",
                     e == ESP_OK ? "not 1-40 characters without control characters" : esp_err_to_name(e));
        }
        uint8_t hue;
        e = nvs_get_u8(h, "hue", &hue);
        if (e == ESP_OK && hue <= 1) {
            c.hue = hue != 0;
            c.stored |= CFG_K_HUE;
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "CFG hue: stored value not usable (%s); default used",
                     e == ESP_OK ? "out of range" : esp_err_to_name(e));
        }
        nvs_close(h);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "CFG namespace %s not readable (%s); defaults used", CFG_NVS_NS, esp_err_to_name(err));
    }
    portENTER_CRITICAL(&s_light_mux);
    cfg_live = c;
    memcpy(s_ext_pan_boot, c.ext_pan, sizeof s_ext_pan_boot);
    s_hue_boot = c.hue;
    portEXIT_CRITICAL(&s_light_mux);
    char st[96], df[96];
    cfg_key_list(c.stored, st, sizeof st);
    cfg_key_list(((1u << CFG_K_COUNT) - 1) & ~c.stored, df, sizeof df);
    ESP_LOGI(TAG, "CFG stored: %s; defaulted: %s", st, df);
    ESP_LOGI(TAG, "CFG role %s, webhook %s, rx_wd_s %lu, ext_pan %s, hue %s",
             cfg_role_name(c.role), c.webhook[0] ? "set" : "none",
             (unsigned long)c.rx_wd_s, c.ext_pan[0] ? c.ext_pan : "any", c.hue ? "on" : "off");
    ESP_LOGI(TAG, "CFG name \"%s\"", c.name);
}

void cfg_get(cfg_t *out)
{
    portENTER_CRITICAL(&s_light_mux);
    *out = cfg_live;
    portEXIT_CRITICAL(&s_light_mux);
}

uint8_t cfg_role(void)
{
    portENTER_CRITICAL(&s_light_mux);
    uint8_t role = cfg_live.role;
    portEXIT_CRITICAL(&s_light_mux);
    return role;
}

uint32_t cfg_rx_wd_s(void)
{
    portENTER_CRITICAL(&s_light_mux);
    uint32_t s = cfg_live.rx_wd_s;
    portEXIT_CRITICAL(&s_light_mux);
    return s;
}

bool cfg_has_webhook(void)
{
    portENTER_CRITICAL(&s_light_mux);
    bool set = cfg_live.webhook[0] != 0;
    portEXIT_CRITICAL(&s_light_mux);
    return set;
}

bool cfg_ext_pan(char out[17])
{
    portENTER_CRITICAL(&s_light_mux);
    memcpy(out, cfg_live.ext_pan, sizeof cfg_live.ext_pan);
    portEXIT_CRITICAL(&s_light_mux);
    out[16] = 0;
    return out[0] != 0;
}

bool cfg_hue(void)
{
    portENTER_CRITICAL(&s_light_mux);
    bool hue = cfg_live.hue;
    portEXIT_CRITICAL(&s_light_mux);
    return hue;
}

bool cfg_hue_boot(void)
{
    portENTER_CRITICAL(&s_light_mux);
    bool hue = s_hue_boot;
    portEXIT_CRITICAL(&s_light_mux);
    return hue;
}

size_t cfg_webhook(char *out, size_t sz)
{
    portENTER_CRITICAL(&s_light_mux);
    size_t n = strnlen(cfg_live.webhook, sizeof cfg_live.webhook);
    if (n >= sz) {
        n = sz - 1;
    }
    memcpy(out, cfg_live.webhook, n);
    portEXIT_CRITICAL(&s_light_mux);
    out[n] = 0;
    return n;
}

/* The keys in `keys` from c into the open handle, stopping at the first failure; *done gets a bit for
 * every key that was set. */
static esp_err_t cfg_nvs_set(nvs_handle_t h, const cfg_t *c, uint32_t keys, uint32_t *done)
{
    esp_err_t err = ESP_OK;
    if (keys & CFG_K_ROLE) {
        err = nvs_set_u8(h, "role", c->role);
        if (err == ESP_OK) {
            *done |= CFG_K_ROLE;
        }
    }
    if (err == ESP_OK && (keys & CFG_K_WEBHOOK)) {
        err = nvs_set_str(h, "webhook", c->webhook);
        if (err == ESP_OK) {
            *done |= CFG_K_WEBHOOK;
        }
    }
    for (size_t i = 0; i < CFG_NUM_COUNT && err == ESP_OK; i++) {
        if (keys & CFG_NUMS[i].bit) {
            err = nvs_set_u32(h, CFG_NUMS[i].key, cfg_num_get(c, i));
            if (err == ESP_OK) {
                *done |= CFG_NUMS[i].bit;
            }
        }
    }
    if (err == ESP_OK && (keys & CFG_K_EXT_PAN)) {
        err = nvs_set_str(h, "ext_pan", c->ext_pan);
        if (err == ESP_OK) {
            *done |= CFG_K_EXT_PAN;
        }
    }
    if (err == ESP_OK && (keys & CFG_K_NAME)) {
        err = nvs_set_str(h, "name", c->name);
        if (err == ESP_OK) {
            *done |= CFG_K_NAME;
        }
    }
    if (err == ESP_OK && (keys & CFG_K_HUE)) {
        err = nvs_set_u8(h, "hue", c->hue ? 1 : 0);
        if (err == ESP_OK) {
            *done |= CFG_K_HUE;
        }
    }
    return err;
}

esp_err_t cfg_nvs_write(const cfg_t *next, uint32_t keys)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t done = 0;
    err = cfg_nvs_set(h, next, keys, &done);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK && done) {
        /* Each set persists on its own, so put back the live values of the keys already set: all or nothing. */
        cfg_t old;
        uint32_t undone = 0;
        cfg_get(&old);
        esp_err_t back = cfg_nvs_set(h, &old, done, &undone);
        if (back == ESP_OK) {
            back = nvs_commit(h);
        }
        if (back != ESP_OK) {
            ESP_LOGE(TAG, "CFG PUT: could not put the old values back (%s)", esp_err_to_name(back));
        }
    }
    nvs_close(h);
    return err;
}

void cfg_publish_locked(const cfg_t *next, uint32_t keys)
{
    if (keys & CFG_K_ROLE) {
        cfg_live.role = next->role;
    }
    if (keys & CFG_K_WEBHOOK) {
        memcpy(cfg_live.webhook, next->webhook, sizeof cfg_live.webhook);
    }
    for (size_t i = 0; i < CFG_NUM_COUNT; i++) {
        if (keys & CFG_NUMS[i].bit) {
            cfg_num_set(&cfg_live, i, cfg_num_get(next, i));
        }
    }
    if (keys & CFG_K_EXT_PAN) {
        memcpy(cfg_live.ext_pan, next->ext_pan, sizeof cfg_live.ext_pan);
    }
    if (keys & CFG_K_NAME) {
        memcpy(cfg_live.name, next->name, sizeof cfg_live.name);
    }
    if (keys & CFG_K_HUE) {
        cfg_live.hue = next->hue;
    }
    cfg_live.stored |= keys;
    state_mark(ST_SETTINGS);
}

/* Keys other than a changed role: NVS first, then published under the mux. */
static esp_err_t cfg_store(const cfg_t *next, uint32_t keys)
{
    esp_err_t err = cfg_nvs_write(next, keys);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_light_mux);
        cfg_publish_locked(next, keys);
        portEXIT_CRITICAL(&s_light_mux);
    }
    return err;
}

/* CFG <key> <old> -> <new> for every key written; the webhook as set or cleared, never its value. */
static void cfg_log_changes(const cfg_t *old, const cfg_t *now, uint32_t keys)
{
    if (keys & CFG_K_ROLE) {
        ESP_LOGI(TAG, "CFG role %s -> %s", cfg_role_name(old->role), cfg_role_name(now->role));
    }
    if (keys & CFG_K_WEBHOOK) {
        ESP_LOGI(TAG, "CFG webhook %s", now->webhook[0] ? "set" : "cleared");
    }
    for (size_t i = 0; i < CFG_NUM_COUNT; i++) {
        if (keys & CFG_NUMS[i].bit) {
            ESP_LOGI(TAG, "CFG %s %lu -> %lu", CFG_NUMS[i].key, (unsigned long)cfg_num_get(old, i),
                     (unsigned long)cfg_num_get(now, i));
        }
    }
    if (keys & CFG_K_EXT_PAN) {
        ESP_LOGI(TAG, "CFG ext_pan %s -> %s (takes effect at the next reboot or /zigbee/erase)",
                 old->ext_pan[0] ? old->ext_pan : "any", now->ext_pan[0] ? now->ext_pan : "any");
    }
    if (keys & CFG_K_NAME) {
        ESP_LOGI(TAG, "CFG name \"%s\" -> \"%s\"", old->name, now->name);
    }
    if (keys & CFG_K_HUE) {
        ESP_LOGI(TAG, "CFG hue %s -> %s (takes effect at the next reboot or /zigbee/erase)",
                 old->hue ? "on" : "off", now->hue ? "on" : "off");
    }
}

/* ---------------------------------------------------------------- HTTP */

/* Sends o (deleted here) as the JSON answer with this status. */
static esp_err_t cfg_send_json(httpd_req_t *req, const char *status, cJSON *o)
{
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!s) {
        return c6_json_error(req, 500, "internal", "out of memory; try again");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return err;
}

bool cfg_json_add(struct cJSON *o, const cfg_t *c)
{
    char masked[CFG_WEBHOOK_MAX + 4];             /* the URL at most, "…" (3 bytes), NUL */
    cfg_mask_webhook(c->webhook, masked, sizeof masked);
    bool ok = cJSON_AddStringToObject(o, "role", cfg_role_name(c->role)) != NULL;
    ok = ok && cJSON_AddStringToObject(o, "webhook", masked) != NULL;
    for (size_t i = 0; i < CFG_NUM_COUNT && ok; i++) {
        ok = cJSON_AddNumberToObject(o, CFG_NUMS[i].key, cfg_num_get(c, i)) != NULL;
    }
    ok = ok && cJSON_AddStringToObject(o, "ext_pan", c->ext_pan) != NULL;
    ok = ok && cJSON_AddStringToObject(o, "name", c->name) != NULL;
    ok = ok && cJSON_AddBoolToObject(o, "hue", c->hue) != NULL;
    return ok;
}

/* The GET /api/v1/settings JSON, also PUT's answer. PUT's (put) adds "pending", listing each key read once
 * before the stack starts (ext_pan, hue) whose live value differs from the one the stack started with: it
 * applies at the next reboot or erase. */
static esp_err_t cfg_send_settings(httpd_req_t *req, bool put)
{
    cfg_t c;
    uint32_t pend;
    portENTER_CRITICAL(&s_light_mux);
    c = cfg_live;
    pend = (strcasecmp(c.ext_pan, s_ext_pan_boot) != 0 ? CFG_K_EXT_PAN : 0)   /* the stack reads either case alike */
           | (c.hue != s_hue_boot ? CFG_K_HUE : 0);
    portEXIT_CRITICAL(&s_light_mux);
    cJSON *o = cJSON_CreateObject();
    if (o && !cfg_json_add(o, &c)) {
        cJSON_Delete(o);                          /* out of memory: cfg_send_json answers 500 */
        o = NULL;
    }
    if (o) {
        cJSON *st = cJSON_AddArrayToObject(o, "stored");
        for (int i = 0; st && i < CFG_K_COUNT; i++) {
            if (c.stored & (1u << i)) {
                cJSON_AddItemToArray(st, cJSON_CreateString(CFG_KEY_NAMES[i]));
            }
        }
        if (put && pend) {
            cJSON *pd = cJSON_AddArrayToObject(o, "pending");
            for (int i = 0; pd && i < CFG_K_COUNT; i++) {
                if (pend & (1u << i)) {
                    cJSON_AddItemToArray(pd, cJSON_CreateString(CFG_KEY_NAMES[i]));
                }
            }
        }
    }
    return cfg_send_json(req, "200 OK", o);
}

esp_err_t settings_get(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    return cfg_send_settings(req, false);
}

esp_err_t settings_put(httpd_req_t *req)
{
    static char body[CFG_BODY_MAX + 1];           /* the server runs one handler at a time */
    char err[120];
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > CFG_BODY_MAX) {
        ESP_LOGW(TAG, "CFG PUT refused: body of %u bytes", (unsigned)req->content_len);
        return c6_json_error(req, 400, "bad_request", "body: a JSON object of at most 512 bytes");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            ESP_LOGW(TAG, "CFG PUT: body not received (%d)", r);
            return ESP_FAIL;                      /* the server closes the connection */
        }
        got += (size_t)r;
    }
    body[got] = 0;
    cfg_t cur, next;
    uint32_t keys;
    cfg_get(&cur);
    bool ok = memchr(body, 0, got) == NULL;
    if (ok) {
        ok = cfg_parse(body, &cur, &next, &keys, err, sizeof err);
    } else {
        snprintf(err, sizeof err, "body: NUL characters are not JSON");
    }
    memset(body, 0, sizeof body);                 /* the URL does not linger */
    if (!ok) {
        ESP_LOGW(TAG, "CFG PUT refused: %s", err);
        return c6_json_error(req, 400, "bad_request", err);
    }
    esp_err_t e = ESP_OK;
    if ((keys & CFG_K_ROLE) && next.role != cur.role) {
        e = bulb_role_switch(&next, keys);        /* every key, with the role, after the Off */
    } else if (keys) {
        e = cfg_store(&next, keys);
    }
    if (e == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "CFG PUT refused: the Off could not be applied; role stays %s, nothing stored",
                 cfg_role_name(cur.role));
        return c6_json_error(req, 503, "zigbee_busy", "the light could not be turned off first; nothing stored, try again");
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "CFG PUT: NVS write failed (%s)", esp_err_to_name(e));
        snprintf(err, sizeof err, "NVS write failed: %s", esp_err_to_name(e));
        return c6_json_error(req, 500, "storage", err);
    }
    cfg_log_changes(&cur, &next, keys);
    if ((keys & CFG_K_ROLE) && next.role != cur.role) {
        c6_discovery_update();                    /* the TXT role: here, after the mux, never inside it */
    }
    if ((keys & CFG_K_NAME) && strcmp(next.name, cur.name) != 0) {
        c6_discovery_set_name(next.name);         /* the mDNS instance name */
        state_mark(ST_BOARD);                     /* board.name; settings was marked when it was published */
    }
    return cfg_send_settings(req, true);
}
