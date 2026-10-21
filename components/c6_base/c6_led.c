/*
 * c6_led — one task owns the colour LED (a WS2812 on CONFIG_C6_RGB_GPIO, through Espressif's led_strip on
 * the RMT); five sources share it. The blue LED on CONFIG_C6_LED_GPIO is configured and held off.
 *
 * The lowest active enum value wins (IDENTIFY, then HOLD_RED, HOLD_GREEN, AP, UNCLAIMED); none: off. Each
 * pattern is phased from the moment its source was set, so a source shows "on" at once. Every colour is
 * scaled by CONFIG_C6_RGB_LEVEL. The strip is written only when the colour shown changes, by this task,
 * outside any critical section. c6_led_set and c6_led_identify_rgb are for any task, never block, and
 * c6_led_set ignores an out-of-range source.
 */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "c6_led.h"

static const char *TAG = "led";

#define LED_TICK_MS      10
#define LED_RMT_RES_HZ   (10 * 1000 * 1000)    /* 0.1 us a tick: the WS2812's bit timing */
#define LED_RED          0xFF0000u
#define LED_GREEN        0x00FF00u

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_active;                       /* one bit per source */
static int64_t s_since_us[C6_LED_SOURCES];      /* when each source was last set */
static uint32_t s_identify_rgb = C6_LED_WHITE;  /* IDENTIFY's colour, 0xRRGGBB (s_mux) */
static led_strip_handle_t s_strip;              /* NULL: the strip could not be set up, nothing is written */
static uint32_t s_shown;                        /* the scaled colour last written to the strip */
static bool s_started;

/* The source shown for a set of active bits: the lowest; -1 when none. */
static int led_winner(uint32_t active)
{
    for (int s = 0; s < C6_LED_SOURCES; s++) {
        if (active & (1u << s)) {
            return s;
        }
    }
    return -1;
}

/* Whether one pattern is lit, elapsed_ms after its source was set. */
static int led_level(int src, uint32_t elapsed_ms)
{
    uint32_t t;
    switch (src) {
    case C6_LED_IDENTIFY:                       /* solid */
        return 1;
    case C6_LED_HOLD_RED:                       /* rapid flash, 8 Hz: 125 ms period */
    case C6_LED_HOLD_GREEN:
        return (elapsed_ms % 125) < 63;
    case C6_LED_AP:                             /* double flash: two 100 ms flashes at the top of every 2 s */
    case C6_LED_UNCLAIMED:
        t = elapsed_ms % 2000;
        return t < 100 || (t >= 200 && t < 300);
    default:
        return 0;
    }
}

/* The colour of one source, 0xRRGGBB, before the level scales it; identify_rgb is IDENTIFY's. */
static uint32_t led_colour(int src, uint32_t identify_rgb)
{
    switch (src) {
    case C6_LED_IDENTIFY:
        return identify_rgb;
    case C6_LED_HOLD_RED:
    case C6_LED_UNCLAIMED:
        return LED_RED;
    case C6_LED_HOLD_GREEN:
    case C6_LED_AP:
        return LED_GREEN;
    default:
        return 0;
    }
}

/* Each of the three channels times CONFIG_C6_RGB_LEVEL / 255, rounded down. */
static uint32_t led_scale(uint32_t rgb)
{
    uint32_t r = ((rgb >> 16) & 0xFF) * CONFIG_C6_RGB_LEVEL / 255;
    uint32_t g = ((rgb >> 8) & 0xFF) * CONFIG_C6_RGB_LEVEL / 255;
    uint32_t b = (rgb & 0xFF) * CONFIG_C6_RGB_LEVEL / 255;
    return r << 16 | g << 8 | b;
}

/* One pass: pick the winner, its colour now, and write the strip when that changes. */
static void led_step(int64_t now_us)
{
    uint32_t active, identify_rgb;
    int64_t since[C6_LED_SOURCES];
    portENTER_CRITICAL(&s_mux);
    active = s_active;
    identify_rgb = s_identify_rgb;
    for (int s = 0; s < C6_LED_SOURCES; s++) {
        since[s] = s_since_us[s];
    }
    portEXIT_CRITICAL(&s_mux);

    uint32_t rgb = 0;
    int w = led_winner(active);
    if (w >= 0) {
        int64_t el = now_us - since[w];
        if (led_level(w, (uint32_t)((el < 0 ? 0 : el) / 1000))) {
            rgb = led_scale(led_colour(w, identify_rgb));
        }
    }
    if (rgb != s_shown && s_strip) {
        s_shown = rgb;
        led_strip_set_pixel(s_strip, 0, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        led_strip_refresh(s_strip);
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        led_step(esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

/* The blue LED held off, the colour LED's power on, the strip set up and written dark once, then the task. */
void c6_led_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    gpio_reset_pin(CONFIG_C6_LED_GPIO);
    gpio_set_direction(CONFIG_C6_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_C6_LED_GPIO, 0);
    gpio_reset_pin(CONFIG_C6_RGB_PWR_GPIO);
    gpio_set_direction(CONFIG_C6_RGB_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_C6_RGB_PWR_GPIO, 1);
    const led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_C6_RGB_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RES_HZ,
    };
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) {
        s_strip = NULL;
        ESP_LOGE(TAG, "LED: the colour LED on GPIO %d was not set up (%s); it stays dark", CONFIG_C6_RGB_GPIO,
                 esp_err_to_name(err));
    } else {
        led_strip_set_pixel(s_strip, 0, 0, 0, 0);
        led_strip_refresh(s_strip);
    }
    s_shown = 0;
    xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
}

void c6_led_set(c6_led_source_t src, bool on)
{
    if ((unsigned)src >= (unsigned)C6_LED_SOURCES) {
        return;
    }
    uint32_t bit = 1u << (unsigned)src;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (on && !(s_active & bit)) {              /* a second set changes nothing, the phase included */
        s_active |= bit;
        s_since_us[src] = now;
    } else if (!on) {
        s_active &= ~bit;
    }
    portEXIT_CRITICAL(&s_mux);
}

void c6_led_identify_rgb(uint32_t rgb)
{
    portENTER_CRITICAL(&s_mux);
    s_identify_rgb = rgb & 0xFFFFFFu;
    portEXIT_CRITICAL(&s_mux);
}
