/*
 * state — the board's state object (protocol §5, 0.5.0) and GET /api/v1/state.
 *
 *   {"board":{..},"settings":{..},"zigbee":{..},"light":{..},"controller":{..},"button":{..},"update":{..}}
 *
 * state_json builds the keys asked for, in that order, from the getters (c6_base_info, cfg_get,
 * bulb_zigbee_snapshot, bulb_light_snapshot, bulb_button_snapshot, and c6_base_update_info for the update
 * object) with cJSON, and prints it into the
 * caller's buffer (cJSON_PrintPreallocated: a buffer too short fails, it is never overrun). cJSON escapes
 * the name and the masked webhook. The settings block is settings.c's own (cfg_json_add): the webhook masked
 * as GET /api/v1/settings shows it; the Wi-Fi password, the claim token and its digest are not in any getter.
 * Timestamps are board uptime seconds.
 */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "c6_base.h"
#include "c6_auth.h"
#include "settings.h"
#include "state.h"

#define STATE_BUF_SIZE 1536                    /* GET /api/v1/state's answer, as the stream's buffer */

/* The light's attributes in the protocol's order. */
static const struct {
    int idx;
    const char *name;
} ST_LIGHT_KEYS[SNAP_COUNT] = {
    {SNAP_ON, "on"}, {SNAP_LEVEL, "level"}, {SNAP_MODE, "mode"}, {SNAP_CT, "ct"},
    {SNAP_X, "x"}, {SNAP_Y, "y"}, {SNAP_HUE, "hue"}, {SNAP_SAT, "sat"},
};

/* ---------------------------------------------------------------- one value each; false: out of memory */

static bool st_num(cJSON *o, const char *k, double v)
{
    return cJSON_AddNumberToObject(o, k, v) != NULL;
}

static bool st_str(cJSON *o, const char *k, const char *v)
{
    return cJSON_AddStringToObject(o, k, v) != NULL;
}

static bool st_null(cJSON *o, const char *k)
{
    return cJSON_AddNullToObject(o, k) != NULL;
}

static bool st_bool(cJSON *o, const char *k, bool v)
{
    return cJSON_AddBoolToObject(o, k, v) != NULL;
}

/* Board uptime seconds of a time stamp; null for 0 (never). */
static bool st_at(cJSON *o, const char *k, int64_t us)
{
    return us > 0 ? st_num(o, k, (double)(us / 1000000)) : st_null(o, k);
}

/* "0x%04X", or null for 0 when `zero_null`. */
static bool st_hex16(cJSON *o, const char *k, uint16_t v, bool zero_null)
{
    char s[8];
    if (zero_null && v == 0) {
        return st_null(o, k);
    }
    snprintf(s, sizeof s, "0x%04X", (unsigned)v);
    return st_str(o, k, s);
}

/* ---------------------------------------------------------------- the seven keys */

static bool st_board(cJSON *o, const c6_base_info_t *bi, const char *name)
{
    bool ok = st_str(o, "id", bi->id);
    ok = ok && st_str(o, "name", name);
    ok = ok && st_str(o, "fw", bi->fw);
    ok = ok && st_num(o, "api", 1);
    ok = ok && st_str(o, "hw", "nanoc6");
    ok = ok && st_num(o, "uptime_s", bi->uptime_s);
    ok = ok && st_str(o, "ip", bi->ip);
    ok = ok && st_num(o, "rssi", bi->rssi);
    ok = ok && st_str(o, "ota_state", bi->ota_state);
    ok = ok && st_str(o, "partition", bi->partition);
    return ok;
}

/* now: esp_timer_get_time() when the object is built, for last_rx_s. */
static bool st_zigbee(cJSON *o, const zb_snapshot_t *z, int64_t now)
{
    bool ok = st_bool(o, "joined", z->joined);
    ok = ok && st_str(o, "ieee", z->ieee);
    ok = ok && (z->joined ? st_hex16(o, "short", z->short_addr, false) : st_null(o, "short"));
    ok = ok && (z->joined ? st_num(o, "channel", z->channel) : st_null(o, "channel"));
    ok = ok && (z->joined ? st_hex16(o, "pan", z->pan, false) : st_null(o, "pan"));
    ok = ok && st_str(o, "ext_pan", z->ext_pan);
    ok = ok && st_bool(o, "reporting", z->reporting);
    if (z->last_rx_us > 0) {
        int64_t ago = now > z->last_rx_us ? (now - z->last_rx_us) / 1000000 : 0;
        ok = ok && st_num(o, "last_rx_s", (double)ago);
    } else {
        ok = ok && st_null(o, "last_rx_s");
    }
    ok = ok && st_str(o, "profile", z->profile ? z->profile : "");
    ok = ok && st_bool(o, "hue", z->hue);
    ok = ok && st_hex16(o, "group", z->group, true);
    return ok;
}

static bool st_light(cJSON *o, const bulb_light_snap_t *l)
{
    bool ok = true;
    for (int i = 0; i < SNAP_COUNT && ok; i++) {
        int a = ST_LIGHT_KEYS[i].idx;
        ok = (l->seen & (1u << a)) ? st_num(o, ST_LIGHT_KEYS[i].name, l->val[a]) : st_null(o, ST_LIGHT_KEYS[i].name);
    }
    ok = ok && (l->by ? st_str(o, "by", l->by) : st_null(o, "by"));
    ok = ok && st_at(o, "at_s", l->at_us);
    ok = ok && st_bool(o, "unsaved", l->unsaved);
    return ok;
}

/* What the Zigbee controller has asked for, as the webhook pushes it (only what it has set), with seq and at_s. */
static bool st_hub(cJSON *o, const bulb_light_snap_t *l)
{
    bool ok = st_num(o, "seq", l->hub_seq);
    ok = ok && st_at(o, "at_s", l->hub_at_us);
    for (int i = 0; i < SNAP_COUNT && ok; i++) {
        int a = ST_LIGHT_KEYS[i].idx;
        if (l->hub_seen & (1u << a)) {
            ok = st_num(o, ST_LIGHT_KEYS[i].name, l->hub_val[a]);
        }
    }
    ok = ok && st_bool(o, "settled", true);       /* 0.4.0 pushes the Zigbee controller's target at once */
    return ok;
}

static bool st_button(cJSON *o, const bulb_button_snap_t *b)
{
    bool ok = st_bool(o, "held", b->held);
    ok = ok && st_num(o, "press", b->press);
    ok = ok && st_str(o, "release_by", b->release_by ? b->release_by : "none");
    ok = ok && st_at(o, "at_s", b->at_us);
    return ok;
}

/* c6_base's update object: version and error null when "", percent null when -1. Idle with
 * no error is the 0.5.x object exactly: {"state":"idle","version":null,"percent":null,"error":null}. */
static bool st_update(cJSON *o)
{
    c6_update_t u;
    c6_base_update_info(&u);
    bool ok = st_str(o, "state", u.state);
    ok = ok && (u.version[0] ? st_str(o, "version", u.version) : st_null(o, "version"));
    ok = ok && (u.percent >= 0 ? st_num(o, "percent", (double)u.percent) : st_null(o, "percent"));
    ok = ok && (u.error[0] ? st_str(o, "error", u.error) : st_null(o, "error"));
    return ok;
}

/* ---------------------------------------------------------------- the object */

int state_json(char *out, size_t n, uint32_t keys)
{
    if (!out || n == 0) {
        return -1;
    }
    out[0] = 0;
    if (n > INT_MAX) {
        n = INT_MAX;
    }
    cfg_t c = {0};
    if (keys & (ST_BOARD | ST_SETTINGS)) {
        cfg_get(&c);
    }
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL;
    if (ok && (keys & ST_BOARD)) {
        c6_base_info_t bi;
        c6_base_info(&bi);
        cJSON *o = cJSON_AddObjectToObject(root, "board");
        ok = o && st_board(o, &bi, c.name);
    }
    if (ok && (keys & ST_SETTINGS)) {
        cJSON *o = cJSON_AddObjectToObject(root, "settings");
        ok = o && cfg_json_add(o, &c);
    }
    if (ok && (keys & ST_ZIGBEE)) {
        zb_snapshot_t z;
        bulb_zigbee_snapshot(&z);
        cJSON *o = cJSON_AddObjectToObject(root, "zigbee");
        ok = o && st_zigbee(o, &z, esp_timer_get_time());
    }
    bulb_light_snap_t l = {0};
    if (keys & (ST_LIGHT | ST_HUB)) {
        bulb_light_snapshot(&l);
    }
    if (ok && (keys & ST_LIGHT)) {
        cJSON *o = cJSON_AddObjectToObject(root, "light");
        ok = o && st_light(o, &l);
    }
    if (ok && (keys & ST_HUB)) {
        cJSON *o = cJSON_AddObjectToObject(root, "controller");
        ok = o && st_hub(o, &l);
    }
    if (ok && (keys & ST_BUTTON)) {
        bulb_button_snap_t b;
        bulb_button_snapshot(&b);
        cJSON *o = cJSON_AddObjectToObject(root, "button");
        ok = o && st_button(o, &b);
    }
    if (ok && (keys & ST_UPDATE)) {
        cJSON *o = cJSON_AddObjectToObject(root, "update");
        ok = o && st_update(o);
    }
    int len = -1;
    if (ok && cJSON_PrintPreallocated(root, out, (int)n, false)) {
        len = (int)strnlen(out, n);
    }
    if (len < 0 || (size_t)len >= n) {
        out[0] = 0;                               /* did not fit, or out of memory: nothing half-written */
        len = -1;
    }
    cJSON_Delete(root);
    return len;
}

/* GET /api/v1/state (owner): the whole object. */
esp_err_t state_get(httpd_req_t *req)
{
    static char buf[STATE_BUF_SIZE];              /* the server runs one handler at a time */
    if (!c6_auth_ok(req, C6_AUTH_OWNER)) {
        return ESP_OK;
    }
    int n = state_json(buf, sizeof buf, ST_ALL);
    if (n < 0) {
        return c6_json_error(req, 500, "internal", "the state object could not be built; try again");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, buf, n);
}
