#pragma once
/*
 * c6_wifi: the Wi-Fi station of c6_base, its stored credentials, the setup hotspot and the
 * failure timer.
 *
 * Credentials live in NVS namespace "c6wifi" (keys ssid, pass). At c6_wifi_start(): stored → used;
 * else a non-empty build SSID → used and stored once ("WIFI credentials migrated from the build");
 * else no station attempt and the hotspot opens at once. The password is never logged and never
 * returned by anything here. The SSID may be shown.
 *
 * The hotspot (WIFI_MODE_APSTA while up, WIFI_MODE_STA otherwise) is open, SSID
 * CONFIG_C6_AP_SSID_PREFIX + the six-hex serial, at 192.168.44.1/24, two clients at most.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef enum { C6_WIFI_NO_CREDENTIALS, C6_WIFI_CONNECTING, C6_WIFI_CONNECTED, C6_WIFI_FAILED } c6_wifi_state_t;

void c6_wifi_start(void);                                   /* from c6_base_start, after NVS */
esp_netif_t *c6_wifi_netif(void);                           /* the STA netif, for info/ip */
c6_wifi_state_t c6_wifi_state(void);
bool c6_wifi_ssid(char *out, size_t n);                     /* the stored SSID; false if none */
esp_err_t c6_wifi_set(const char *ssid, const char *pass);  /* validate, store, apply now (disconnect + connect) */
typedef void (*c6_wifi_scan_cb_t)(const char *ssid, int rssi, bool secured, void *ctx);
esp_err_t c6_wifi_scan(c6_wifi_scan_cb_t cb, void *ctx);    /* blocking active scan, <= 16 results, strongest first, duplicates dropped */
void c6_wifi_ap_start(uint32_t seconds);                    /* 0: until STA has an IP + 10 s; else for that long */
void c6_wifi_ap_stop(void);
bool c6_wifi_ap_up(void);
typedef void (*c6_wifi_ap_cb_t)(bool up);                   /* WIFI-UI registers one: start/stop the captive DNS */
void c6_wifi_on_ap(c6_wifi_ap_cb_t cb);
