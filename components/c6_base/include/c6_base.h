#pragma once
/* c6_base: Wi-Fi, push OTA with rollback, the HTTP log ring, SNTP, mDNS, the claim (c6_auth.h), the
 * button (c6_button.h), discovery (c6_discovery.h) and the /api/v1 base endpoints. See c6_base.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

/* NVS, Wi-Fi, HTTP, mDNS, SNTP and the rollback check. Call first, from app_main. */
void c6_base_start(void);

/* The running HTTP server, for an image to register its own endpoints (48 handlers in all). */
httpd_handle_t c6_base_httpd(void);

/* The developer endpoints' check (c6_auth_dev_ok): true for the claim token or the build's developer
 * credential; otherwise it has already answered a plain-text 403. */
bool c6_base_token_ok(httpd_req_t *req);

/* Every /api/v1 refusal (protocol §7): sets the status line and application/json and sends
 * {"error":"<code>","message":"<message>"}, both JSON-escaped. Returns httpd_resp_send's result. */
esp_err_t c6_json_error(httpd_req_t *req, int status, const char *code, const char *message);

/* The board's identity and health, for /api/v1/info and the state object's board key. */
typedef struct { char id[8]; char mac[18]; char fw[32]; uint32_t uptime_s; char ip[16]; int rssi;
                 char ota_state[16]; char partition[16]; } c6_base_info_t;
void c6_base_info(c6_base_info_t *out);
void c6_base_name(char *out, size_t n);   /* NVS c6cfg key "name" if stored, else "Zigbee Anthema <id>" */

/* One query-string value; false if absent. */
bool c6_base_query(httpd_req_t *req, const char *key, char *val, size_t len);

/* c6-<last three MAC bytes>, as used for mDNS and DHCP. */
const char *c6_base_host(void);

/* The app hooks: how a component tells the image about a change. One bit per key of the state
 * object; the image defines the strong functions, c6_base.c carries weak no-op defaults.
 * c6_app_state_mark may be called from the stack's context and inside a critical section: an
 * image's definition must not block, allocate or log. */
#define C6_ST_BOARD    (1u << 0)
#define C6_ST_SETTINGS (1u << 1)
#define C6_ST_ZIGBEE   (1u << 2)
#define C6_ST_LIGHT    (1u << 3)
#define C6_ST_HUB      (1u << 4)
#define C6_ST_BUTTON   (1u << 5)
#define C6_ST_UPDATE   (1u << 6)
void c6_app_state_mark(uint32_t keys);
void c6_app_events_close(void);

/* The first-start check's Zigbee half. The image says whether a new image must
 * also get Zigbee back before it is marked valid (weak default false: c6_base, c6_coord), and tells
 * the base when it is: joined, or factory new with nothing to join. From a task, never from a
 * critical section or the stack's callbacks under a lock the stack holds; safe to call more than once. */
bool c6_app_verify_zigbee(void);
void c6_base_zigbee_ready(void);

/* The update object (protocol §5 `update`), copied in one critical section.
 * state: "idle" | "receiving" | "verifying" | "rebooting". percent -1 and error "" are JSON null. */
typedef struct { char state[12]; char version[32]; int percent; char error[20]; } c6_update_t;
void c6_base_update_info(c6_update_t *out);
