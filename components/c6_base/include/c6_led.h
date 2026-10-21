#pragma once
/*
 * c6_led: the NanoC6's colour LED (a WS2812 on CONFIG_C6_RGB_GPIO, powered through CONFIG_C6_RGB_PWR_GPIO),
 * shared by five sources through one task. The blue LED on CONFIG_C6_LED_GPIO is held off.
 *
 * Each source is set and cleared independently; the lowest active enum value is the one shown. None
 * active: off. Every colour is scaled by CONFIG_C6_RGB_LEVEL (0-255).
 *
 *   IDENTIFY    POST /identify, /api/v1/identify, for its seconds    solid, in the colour asked for
 *                                                                    (c6_led_identify_rgb); white if none
 *   HOLD_RED    a hold from the 10 s step (the claim cleared) until   rapid red flash, 8 Hz
 *               the release
 *   HOLD_GREEN  a hold from 3 s until the release, or until 10 s      rapid green flash, 8 Hz
 *   AP          the setup hotspot is up                              green double flash: two 100 ms
 *                                                                    flashes at the top of every 2 s
 *   UNCLAIMED   the board holds no claim, until a client claims it   red double flash, the same rhythm
 *
 * c6_led_set and c6_led_identify_rgb may be called from any task and never block. An out-of-range source
 * is ignored.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    C6_LED_IDENTIFY, C6_LED_HOLD_RED, C6_LED_HOLD_GREEN, C6_LED_AP, C6_LED_UNCLAIMED, C6_LED_SOURCES
} c6_led_source_t;

#define C6_LED_WHITE 0xFFFFFFu                              /* IDENTIFY's colour when none is given */

void c6_led_start(void);                                    /* from c6_base_start */
void c6_led_set(c6_led_source_t src, bool on);              /* the lowest active enum value wins */
void c6_led_identify_rgb(uint32_t rgb);                     /* IDENTIFY's colour, 0xRRGGBB, before its set */
