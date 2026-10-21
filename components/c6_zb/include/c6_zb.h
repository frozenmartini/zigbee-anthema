#pragma once
/* c6_zb: shared by the bulb and the bench coordinator. See c6_zb.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_zigbee.h"

/* Log every APSDE-DATA.indication as one "ZB-RX" line; only the tap below may consume a frame. */
void c6_zb_install_rx_logger(void);

/* Also hand every frame the APS logger sees to `tap` (NULL: none), after it is logged. It runs in the
 * stack's context: copy what it needs and return. Returning true consumes the frame: the stack never
 * sees it. */
typedef bool (*c6_zb_rx_tap_t)(const ezb_apsde_data_ind_t *ind);
void c6_zb_set_rx_tap(c6_zb_rx_tap_t tap);

/* Log every ZCL frame as "ZB-ZCL" before the stack handles it (raw ZCL hook); never consumes. */
void c6_zb_install_zcl_logger(void);

/* Frames seen by the APS logger and the ZCL logger since boot, and when the last APS frame came. */
void c6_zb_rx_stats(uint32_t *aps, uint32_t *zcl, int64_t *last_us);

/* Start BDB commissioning now and log the result. Hold the lock. */
ezb_err_t c6_zb_commission(uint8_t mode);

/* Start BDB commissioning in `mode` after `ms`, from a timer; takes the Zigbee lock itself. */
void c6_zb_commission_after(uint8_t mode, uint32_t ms);

/* The common /zb JSON fields, without braces; returns the length written. Hold the lock. */
int c6_zb_status_json(char *out, size_t len);

void c6_zb_eui64_str(const ezb_extaddr_t *a, char *out, size_t len);   /* MSB first, 16 hex */
void c6_zb_addr_str(const ezb_address_t *a, char *out, size_t len);
int c6_zb_parse_hex(const char *hex, uint8_t *out, size_t max);        /* bytes, or -1 */
bool c6_zb_parse_eui64(const char *hex, ezb_extaddr_t *out);           /* MSB first */
