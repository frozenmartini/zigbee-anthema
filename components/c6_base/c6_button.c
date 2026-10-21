/*
 * c6_button — the NanoC6's button: three gestures on one GPIO.
 *
 * CONFIG_C6_BUTTON_GPIO, input, internal pull-up, active low, sampled every 20 ms by one task (no ISR). Two
 * consecutive equal samples change the debounced state. A hold is timed from the first sample of the two
 * that made the press to the first of the two that made the release, so it measures what the pin did.
 * "Still down" at 10 s is the debounced state.
 *
 *   < 1 s          C6_BUTTON_PRESS       on the release
 *   1 s to < 3 s   nothing
 *   3 s to < 10 s  C6_BUTTON_HOLD_SHORT  on the release
 *   10 s           C6_BUTTON_HOLD_LONG   at 10 s, the button still down; its release then gives nothing
 *
 * A button down at power-up gives nothing until it has been released. btn_step is the whole detector: a
 * sample and a time in, a gesture out. The callback runs on this task, outside any critical section.
 *
 * The hold cues on the colour LED, from the debounced state alone: HOLD_GREEN from 3 s of an armed hold
 * until its release or the 10 s step, HOLD_RED from the 10 s step until the release. btn_poll applies them
 * after the gesture's callback of the same sample, so a 3-9 s release starts the hotspot before green
 * ends, and at 10 s the claim is cleared before red shows.
 */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "c6_button.h"
#include "c6_led.h"

static const char *TAG = "button";

#define BTN_POLL_MS       20
#define BTN_PRESS_MAX_MS  1000             /* a press is shorter than this */
#define BTN_SHORT_MIN_MS  3000             /* a short hold is at least this */
#define BTN_LONG_MS       10000            /* a long hold is acted at this, the button still down */

typedef struct {
    bool last_low;                         /* the previous raw sample */
    int64_t last_ms;                       /* and its time */
    bool down;                             /* the debounced state */
    bool armed;                            /* false while a press that began at power-up is held */
    bool long_done;                        /* HOLD_LONG was given for this press */
    int64_t down_ms;                       /* the first low sample of the debounced press */
    bool green;                            /* the hold cue: armed, down, past 3 s, before the 10 s step */
    bool red;                              /* the hold cue: down, after the 10 s step */
    bool green_on;                         /* HOLD_GREEN as last set on the LED */
    bool red_on;                           /* HOLD_RED as last set on the LED */
} btn_t;

static c6_button_cb_t s_cb;
static bool s_started;

/* The detector's state from the first sample: a button down at power-up is not armed. */
static void btn_init(btn_t *b, bool low, int64_t now_ms)
{
    b->last_low = low;
    b->last_ms = now_ms;
    b->down = low;
    b->armed = !low;
    b->long_done = false;
    b->down_ms = 0;
    b->green = false;
    b->red = false;
    b->green_on = false;
    b->red_on = false;
}

/* The gestures: one sample (low: the button is down) at now_ms. Returns a C6_BUTTON_* gesture, or
 * C6_BUTTON_NONE. */
static int btn_detect(btn_t *b, bool low, int64_t now_ms)
{
    bool same = low == b->last_low;
    int64_t first_ms = b->last_ms;         /* the first sample of a run of two */
    b->last_low = low;
    b->last_ms = now_ms;
    if (same && low != b->down) {
        b->down = low;
        if (low) {                         /* the debounced press */
            b->down_ms = first_ms;
            b->long_done = false;
            return C6_BUTTON_NONE;
        }
        if (!b->armed) {                   /* the release of a power-up press */
            b->armed = true;
            return C6_BUTTON_NONE;
        }
        if (b->long_done) {                /* acted at 10 s already */
            return C6_BUTTON_NONE;
        }
        int64_t held = first_ms - b->down_ms;
        if (held < BTN_PRESS_MAX_MS) {
            return C6_BUTTON_PRESS;
        }
        if (held >= BTN_SHORT_MIN_MS && held < BTN_LONG_MS) {
            return C6_BUTTON_HOLD_SHORT;
        }
        return C6_BUTTON_NONE;             /* 1 s to 3 s */
    }
    if (b->down && b->armed && !b->long_done && now_ms - b->down_ms >= BTN_LONG_MS) {
        b->long_done = true;
        return C6_BUTTON_HOLD_LONG;
    }
    return C6_BUTTON_NONE;
}

/* One sample (low: the button is down) at now_ms. Returns a C6_BUTTON_* gesture, or C6_BUTTON_NONE, and sets
 * the hold cues from the debounced state the sample left. */
static int btn_step(btn_t *b, bool low, int64_t now_ms)
{
    int g = btn_detect(b, low, now_ms);
    b->green = b->down && b->armed && !b->long_done && now_ms - b->down_ms >= BTN_SHORT_MIN_MS;
    b->red = b->down && b->long_done;
    return g;
}

static const char *btn_name(int g)
{
    switch (g) {
    case C6_BUTTON_PRESS:      return "press";
    case C6_BUTTON_HOLD_SHORT: return "hold 3-9 s";
    case C6_BUTTON_HOLD_LONG:  return "hold 10 s";
    default:                   return "?";
    }
}

/* One sample through the detector: the gesture's log line and callback, then the hold cues on the LED. */
static void btn_poll(btn_t *b, bool low, int64_t now_ms)
{
    int g = btn_step(b, low, now_ms);
    if (g != C6_BUTTON_NONE) {
        ESP_LOGI(TAG, "BUTTON %s", btn_name(g));
        if (s_cb) {
            s_cb(g);
        }
    }
    if (b->green != b->green_on) {
        b->green_on = b->green;
        if (b->green) {
            ESP_LOGI(TAG, "BUTTON held 3 s");
        }
        c6_led_set(C6_LED_HOLD_GREEN, b->green);
    }
    if (b->red != b->red_on) {
        b->red_on = b->red;
        c6_led_set(C6_LED_HOLD_RED, b->red);
    }
}

static void btn_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_C6_BUTTON_GPIO;
    btn_t b;
    btn_init(&b, gpio_get_level(pin) == 0, esp_timer_get_time() / 1000);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
        btn_poll(&b, gpio_get_level(pin) == 0, esp_timer_get_time() / 1000);
    }
}

void c6_button_start(c6_button_cb_t cb)
{
    if (s_started) {
        return;
    }
    s_started = true;
    s_cb = cb;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_C6_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreate(btn_task, "button", 6144, NULL, 3, NULL);   /* 6144: the 10 s hold runs c6_auth_clear, which ends the event stream (0.7.0) */
}
