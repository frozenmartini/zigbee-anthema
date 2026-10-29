#pragma once
/*
 * The board's state object (protocol §5, 0.5.0): the seven top-level keys, the change
 * tracking and the stream. state.c builds the object, events.c owns GET /api/v1/events and defines the
 * two app hooks of c6_base.h, light.c answers POST /api/v1/light and /api/v1/button/release. bulb.c
 * fills the light, controller, button and Zigbee halves through the getters below.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "c6_base.h"

/* One bit per top-level key; the C6_ST_* bits of c6_base.h under main's names. */
#define ST_BOARD    C6_ST_BOARD
#define ST_SETTINGS C6_ST_SETTINGS
#define ST_ZIGBEE   C6_ST_ZIGBEE
#define ST_LIGHT    C6_ST_LIGHT
#define ST_HUB      C6_ST_HUB
#define ST_BUTTON   C6_ST_BUTTON
#define ST_UPDATE   C6_ST_UPDATE
#define ST_ALL      (ST_BOARD | ST_SETTINGS | ST_ZIGBEE | ST_LIGHT | ST_HUB | ST_BUTTON | ST_UPDATE)

/* The Zigbee half of the object; bulb.c implements the getter. */
typedef struct {
    bool joined, reporting, hue;        /* reporting: restore_reporting succeeded this boot */
    char ieee[17], ext_pan[17];         /* MSB first, 16 hex; ext_pan "" when not joined */
    uint16_t short_addr, pan, group;    /* group 0: none */
    uint8_t channel;
    int64_t last_rx_us;                 /* c6_zb_rx_stats; 0: nothing since boot */
    const char *profile;                /* "sengled_e11" | "espressif" */
} zb_snapshot_t;
void bulb_zigbee_snapshot(zb_snapshot_t *out);   /* takes the Zigbee lock ≤ 100 ms; on timeout, the last copy */

/* The light's attributes, in bulb.c's L_* order (bulb.c asserts the two agree). */
enum { SNAP_ON, SNAP_LEVEL, SNAP_MODE, SNAP_X, SNAP_Y, SNAP_CT, SNAP_HUE, SNAP_SAT, SNAP_COUNT };

/* The light and controller halves, copied under s_light_mux (bulb.c). */
typedef struct bulb_light_snap {
    uint16_t seen, val[SNAP_COUNT];     /* the fake bulb's own attributes; bit per SNAP_* in seen */
    const char *by;                     /* who set it last: "zigbee", "api", "self", "restore"; NULL: nobody yet */
    int64_t at_us;                      /* when; 0: never */
    bool unsaved;                       /* s_light_gen != s_saved_gen */
    uint16_t hub_seen, hub_val[SNAP_COUNT];      /* what the Zigbee controller's commands asked for (s_hub) */
    uint32_t hub_seq;                   /* decoded Zigbee controller commands since boot */
    int64_t hub_at_us;                  /* the last one; 0: none */
} bulb_light_snap_t;
void bulb_light_snapshot(bulb_light_snap_t *out);

/* The shortcut button's half, copied under s_light_mux (bulb.c). */
typedef struct {
    bool held;                          /* a pushed press not yet released */
    uint32_t press;                     /* presses since boot */
    const char *release_by;             /* the last release: "api", "zigbee"; NULL: none yet */
    int64_t at_us;                      /* the last change of held or press; 0: never */
} bulb_button_snap_t;
void bulb_button_snapshot(bulb_button_snap_t *out);

/* The object, or only the keys named, into out (NUL-terminated); returns its length, or -1 when it does
 * not fit (out is then "" and nothing past out[n-1] is written) or the JSON could not be built. */
int state_json(char *out, size_t n, uint32_t keys);

/* The two app hooks of c6_base.h under main's names; events.c defines them. state_mark may be called from
 * the stack's context and inside s_light_mux: it does not block, allocate or log. */
#define state_mark(keys) c6_app_state_mark(keys)
#define events_close()   c6_app_events_close()

/* Creates the stream's lock and the events task; app_main, before the handlers are registered. */
void events_start(void);

/* The handlers, registered by bulb.c's app_main. */
esp_err_t state_get(httpd_req_t *req);           /* GET  /api/v1/state (owner) */
esp_err_t events_get(httpd_req_t *req);          /* GET  /api/v1/events (owner) */
esp_err_t light_v1_post(httpd_req_t *req);       /* POST /api/v1/light (token) */
esp_err_t button_release_post(httpd_req_t *req); /* POST /api/v1/button/release (token) */

/* The cores of the two commands, in bulb.c (they need its statics). want[] is indexed by SNAP_*, -1:
 * leave; msg gets light_apply's text. */
enum {
    BULB_CMD_OK = 0,
    BULB_CMD_WRONG_ROLE = -1,           /* 409 wrong_role */
    BULB_CMD_ECHO = -2,                 /* 200 {"ok":false,"ignored":"controller_command_3s"} */
    BULB_CMD_BUSY = -3,                 /* 503 zigbee_busy: the stack is not up or the lock was busy */
    BULB_CMD_REFUSED = -4,              /* an attribute was refused: msg says which */
    BULB_CMD_NO_PRESS = -5,             /* 409 no_press */
};
int bulb_light_set(int want[SNAP_COUNT], char *msg, size_t msz);
int bulb_button_release(char *msg, size_t msz);
