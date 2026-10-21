#pragma once
/* The board's settings (0.4.0): its role, the webhook, the shortcut button's
 * timings and the RX watchdog (0.5.0: also the Zigbee ext_pan pin and the board's name; 0.7.0: the
 * hue answer, hue), stored in NVS namespace "c6cfg" and changed live over HTTP
 * (GET/PUT /api/v1/settings). See settings.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* The role, as stored in NVS (u8 "role"). */
enum { CFG_ROLE_NONE = 0, CFG_ROLE_MIRROR = 1, CFG_ROLE_SHORTCUT = 2, CFG_ROLE_COUNT = 3 };

/* One bit per key, for "which keys" masks: stored in NVS, present in a request. */
#define CFG_K_ROLE        (1u << 0)
#define CFG_K_WEBHOOK     (1u << 1)
#define CFG_K_SETTLE_MS   (1u << 2)
#define CFG_K_REARM_MS    (1u << 3)
#define CFG_K_OFF_AFTER_S (1u << 4)
#define CFG_K_RX_WD_S     (1u << 5)
#define CFG_K_EXT_PAN     (1u << 6)
#define CFG_K_NAME        (1u << 7)
#define CFG_K_HUE         (1u << 8)
#define CFG_K_COUNT       9

#define CFG_NAME_MAX 40                    /* bytes of the board's name, without the NUL */

#define CFG_WEBHOOK_MAX 159                /* characters, without the NUL */

typedef struct {
    uint8_t role;                          /* CFG_ROLE_* */
    char webhook[CFG_WEBHOOK_MAX + 1];     /* empty: no pushes */
    uint32_t settle_ms;                    /* shortcut: Zigbee controller quiet time before a press is pushed */
    uint32_t rearm_ms;                     /* shortcut: an On this soon after a release is released */
    uint32_t off_after_s;                  /* shortcut: self-release timeout; 0: never (default 0 since 0.6.0) */
    uint32_t rx_wd_s;                      /* RX-silence watchdog; 0: off */
    char ext_pan[17];                      /* the only extended PAN ID to join, 16 hex MSB first; empty: any (0.5.0) */
    bool hue;                              /* the hue answer: on, CurrentHue is served; off, it is 0x86 — read once by
                                            * create_device (zb_task), applies at the next reboot or erase (default on, 0.7.0) */
    char name[CFG_NAME_MAX + 1];           /* the board's name, mDNS instance and the client's; default "Zigbee Anthema <id>" (0.5.0) */
    uint32_t stored;                       /* CFG_K_* bits of the keys present in NVS */
} cfg_t;

/* bulb.c's spinlock guards the settings too, so the code that already holds it reads them in place. */
extern portMUX_TYPE s_light_mux;
/* The live settings. Read only under s_light_mux; written only by settings.c. */
extern cfg_t cfg_live;

/* Read NVS once at boot, after c6_base_start() and before any task that reads a setting. */
void cfg_load(void);

/* Getters: each copies under s_light_mux. Do not call them while holding it. */
void cfg_get(cfg_t *out);
uint8_t cfg_role(void);
uint32_t cfg_rx_wd_s(void);
bool cfg_has_webhook(void);
size_t cfg_webhook(char *out, size_t sz);  /* the URL into out (sz >= 1); returns its length, 0: none */
/* The effective ext_pan (stored, else CONFIG_C6_ZB_EXT_PANID) into out; false when it is empty (any
 * network). zb_task reads it once, before the stack starts: a change applies at the next boot. */
bool cfg_ext_pan(char out[17]);
/* The hue answer: cfg_hue the live setting, read once by create_device before the stack starts (a change
 * applies at the next reboot or /zigbee/erase); cfg_hue_boot the value the stack started with. */
bool cfg_hue(void);
bool cfg_hue_boot(void);

const char *cfg_role_name(uint8_t role);   /* "none", "mirror", "shortcut" */

/* Writes the keys in `keys` from next to NVS and commits; no lock taken. */
esp_err_t cfg_nvs_write(const cfg_t *next, uint32_t keys);
/* Copies the keys in `keys` from next into cfg_live and marks them stored. Hold s_light_mux. */
void cfg_publish_locked(const cfg_t *next, uint32_t keys);

/* In bulb.c: update a held button's timeout after publishing settings (hold s_light_mux), then wake
 * the button task after a successful PUT (outside the mux) so pending timings apply at once. */
void bulb_settings_changed_locked(uint32_t keys);
void bulb_settings_changed(void);

/* The settings as JSON keys into o: role, webhook (masked), settle_ms, rearm_ms, off_after_s, rx_wd_s, ext_pan,
 * name, hue. GET /api/v1/settings adds "stored" (PUT also "pending", listing ext_pan and/or hue); the state
 * object's "settings" is exactly this (api/v1/state.c). False: out of memory. */
struct cJSON;
bool cfg_json_add(struct cJSON *o, const cfg_t *c);

/* The HTTP handlers, registered by bulb.c's app_main. Class token (c6_auth_ok): the claim
 * token, or the developer credential while the build carries one; refusals are JSON (c6_json_error). */
esp_err_t settings_get(httpd_req_t *req);
esp_err_t settings_put(httpd_req_t *req);

/* In bulb.c: a role change. Under the Zigbee lock, then s_light_mux: a light that is on
 * reports Off first (ROLE-OFF); then every key in `keys` is written to NVS, and one s_light_mux section
 * clears the button's state and publishes them. ESP_ERR_INVALID_STATE: the Off could not be applied
 * (stack not up, lock busy, attribute refused); nothing stored, the old role stands. */
esp_err_t bulb_role_switch(const cfg_t *next, uint32_t keys);
