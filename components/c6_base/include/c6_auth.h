#pragma once
/*
 * c6_auth: the board's claim, the claim window and the four auth classes of the /api/v1 endpoints.
 * Protocol §3 (ownership), §4 (the classes), §7 (the codes).
 *
 * The claim is a 64-hex token made from 32 random bytes and returned once by POST /claim; the board keeps
 * only the SHA-256 of the hex string, in NVS namespace "c6claim", key "sha" (a 32-byte blob). The window
 * is open 600 s from boot, 600 s from a button press and 600 s from a POST /release.
 *
 *   NONE    anyone
 *   WINDOW  unclaimed and the window open; else 409 already_claimed / 409 window_closed
 *   OWNER   unclaimed: no credential while the window is open, a Bearer or a closed window 403 claim_required;
 *           claimed: the token, else 401 unauthorized
 *   TOKEN   unclaimed: 403 claim_required; claimed: the token, else 401 unauthorized
 *
 * While the build carries a developer credential (CONFIG_C6_OTA_TOKEN of 16 characters or more), OWNER and
 * TOKEN also accept it in X-OTA-Token, on a claimed or an unclaimed board. The token is never logged.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef enum { C6_AUTH_NONE, C6_AUTH_WINDOW, C6_AUTH_OWNER, C6_AUTH_TOKEN } c6_auth_class_t;
void c6_auth_start(void);                                  /* load; open the window for 600 s */
bool c6_auth_claimed(void);
bool c6_auth_window_open(void);
void c6_auth_window_open_for(uint32_t seconds);            /* the button press; a release */
esp_err_t c6_auth_claim(char token_hex[65]);               /* 32 random bytes → hex out; SHA-256 stored; ESP_ERR_INVALID_STATE if claimed */
esp_err_t c6_auth_clear(void);                             /* erase the key; LED UNCLAIMED; events_close(); failure keeps the claim */
bool c6_auth_ok(httpd_req_t *req, c6_auth_class_t cls);    /* false: the JSON error is already sent */
bool c6_auth_dev_ok(httpd_req_t *req);                     /* the claim token, or X-OTA-Token == CONFIG_C6_OTA_TOKEN while non-empty */
