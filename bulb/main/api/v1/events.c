/*
 * events — GET /api/v1/events, the board's Server-Sent Events stream (protocol §5, 0.5.0),
 * and the two app hooks of c6_base.h that feed and end it.
 *
 *   c6_app_state_mark(keys)  (state_mark)  ORs the keys into s_dirty and counts the change in s_seq, in one
 *                            short critical section, then wakes the events task. Called from the Zigbee
 *                            stack's context and inside s_light_mux: no log, no allocation, no wait.
 *   c6_app_events_close()    (events_close) ends the open stream: first one "event: change" with the keys
 *                            marked and not yet sent (so an update's last marks, rebooting and version, go
 *                            out before the end), then "event: end"; /release, /reboot, the 10 s hold,
 *                            before an OTA reboot and before the /zigbee/erase reboot.
 *
 * The stream: the handler takes the request asynchronous (httpd_req_async_handler_begin), sends
 * "id: <seq>\nevent: state\ndata: <the whole object>\n\n" and returns; the events task owns the socket from
 * then on. It sends "event: change" with the keys marked since the last event (one event for every mark it
 * has not caught up with, id the change counter read then), ": ping" after 15 s with nothing sent, and
 * every 60 s a change carrying board and zigbee so uptime, RSSI and last_rx_s stay fresh in the client.
 * One stream at a time: a second GET ends the first (event: end, completed) and takes over. A failed send
 * completes the request and drops the stream. Every send happens under s_ev_lock (a mutex, never inside a
 * critical section), which also owns the one 1,536 B buffer.
 *
 * The task runs at priority 3, below every task that marks a change (httpd and the Zigbee task 5, the
 * button, push and watchdog tasks 4, the event loop and esp_timer above them), so the notify that
 * c6_app_state_mark gives inside s_light_mux never asks for a yield there.
 */
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "c6_base.h"
#include "c6_auth.h"
#include "state.h"

static const char *TAG = "events";

#define EV_BUF_SIZE     1536                   /* one event: the frame (35 B) and the whole object (about 1,150 B at
                                                  * the tests' maximum; a name and webhook made of quotes and
                                                  * backslashes, escaped, can pass the 1,499 B left) */
#define EV_PING_US      (15 * 1000000LL)       /* a ping after this long with nothing sent */
#define EV_REFRESH_US   (60 * 1000000LL)       /* board and zigbee again after this long */
#define EV_TASK_PRIO    3
#define EV_TASK_STACK   6144
#define EV_WAIT_FOREVER UINT32_MAX

static portMUX_TYPE s_ev_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dirty;                       /* ST_* keys marked since the last event (s_ev_mux) */
static uint32_t s_seq;                         /* the change counter, the state event's id (s_ev_mux) */
static TaskHandle_t s_events_task;             /* set once by events_start */
static SemaphoreHandle_t s_ev_lock;            /* owns the stream, its times and the buffer */
static httpd_req_t *s_stream;                  /* the open stream's async request; NULL: none (s_ev_lock) */
static int64_t s_sent_us;                      /* the last send on it (s_ev_lock) */
static uint32_t s_ev_id;                       /* the id of the last event on it: the state's is s_seq, each change
                                                * +1, so the client sees no gap when marks merge or a refresh is sent (s_ev_lock) */
static int64_t s_refresh_us;                   /* its start, or the last board+zigbee refresh (s_ev_lock) */
static char s_buf[EV_BUF_SIZE];                /* (s_ev_lock) */

/* ---------------------------------------------------------------- the hooks */

void c6_app_state_mark(uint32_t keys)
{
    portENTER_CRITICAL(&s_ev_mux);
    s_dirty |= keys;
    s_seq++;
    portEXIT_CRITICAL(&s_ev_mux);
    if (s_events_task) {
        xTaskNotifyGive(s_events_task);
    }
}

/* Ends the open stream: "event: end" and the closing chunk when `say`, then the request is completed, once.
 * Hold s_ev_lock. */
static void ev_finish(bool say)
{
    httpd_req_t *r = s_stream;
    if (!r) {
        return;
    }
    s_stream = NULL;
    if (say && httpd_resp_send_chunk(r, "event: end\n\n", 12) == ESP_OK) {
        httpd_resp_send_chunk(r, NULL, 0);
    }
    httpd_req_async_handler_complete(r);
}

/* ---------------------------------------------------------------- the stream */

/* One event into s_buf: "id: <seq>\nevent: <name>\ndata: <the keys' JSON>\n\n". Its length, or -1 when the
 * object does not fit. Hold s_ev_lock. */
static int ev_frame(const char *name, uint32_t seq, uint32_t keys)
{
    int k = snprintf(s_buf, sizeof s_buf, "id: %lu\nevent: %s\ndata: ", (unsigned long)seq, name);
    if (k < 0 || (size_t)k + 3 > sizeof s_buf) {
        return -1;
    }
    int j = state_json(s_buf + k, sizeof s_buf - (size_t)k - 2, keys);
    if (j < 0) {
        return -1;
    }
    k += j;                                       /* j <= sizeof s_buf - k - 3: room for "\n\n" and the NUL */
    s_buf[k++] = '\n';
    s_buf[k++] = '\n';
    s_buf[k] = 0;
    return k;
}

/* The first n bytes of s_buf on the open stream; n < 0 (the frame did not fit) or a failed send completes the
 * request and drops the stream. Hold s_ev_lock. */
static bool ev_send(int n, int64_t now)
{
    if (!s_stream) {
        return false;
    }
    if (n < 0 || httpd_resp_send_chunk(s_stream, s_buf, n) != ESP_OK) {
        ev_finish(false);
        ESP_LOGW(TAG, "EVENTS stream dropped: %s", n < 0 ? "the state object did not fit" : "a send failed");
        return false;
    }
    s_sent_us = now;
    return true;
}

/* Ends the open stream, flushing first: the keys marked and not yet sent go out as one "event: change"
 * (only those, no refresh keys), then "event: end". A failed flush send drops the stream in ev_send, so the
 * end is not written and the request is still completed once. No stream: nothing is sent or cleared. */
void c6_app_events_close(void)
{
    if (!s_ev_lock) {
        return;                                   /* before events_start: nothing can be open */
    }
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    bool open = s_stream != NULL;
    if (open) {
        uint32_t keys;
        portENTER_CRITICAL(&s_ev_mux);            /* read and cleared in one section, as events_step does */
        keys = s_dirty;
        s_dirty = 0;
        portEXIT_CRITICAL(&s_ev_mux);
        if (keys) {
            ev_send(ev_frame("change", ++s_ev_id, keys), esp_timer_get_time());
        }
    }
    ev_finish(true);
    xSemaphoreGive(s_ev_lock);
    if (open) {
        ESP_LOGI(TAG, "EVENTS stream ended by the board");
    }
}

/* One pass of the events task at `now`: the keys marked since the last pass, plus board and zigbee when the
 * 60 s refresh is due, as one change; else a ping after 15 s with nothing sent. Nothing without a stream.
 * Returns the ms until the next timed send, EV_WAIT_FOREVER with no stream. */
static uint32_t events_step(int64_t now)
{
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    if (!s_stream) {
        xSemaphoreGive(s_ev_lock);
        return EV_WAIT_FOREVER;
    }
    uint32_t keys;
    portENTER_CRITICAL(&s_ev_mux);
    keys = s_dirty;
    s_dirty = 0;
    portEXIT_CRITICAL(&s_ev_mux);
    if (now - s_refresh_us >= EV_REFRESH_US) {
        keys |= ST_BOARD | ST_ZIGBEE;
        s_refresh_us = now;
    }
    if (keys) {
        ev_send(ev_frame("change", ++s_ev_id, keys), now);
    } else if (now - s_sent_us >= EV_PING_US) {
        int n = snprintf(s_buf, sizeof s_buf, ": ping\n\n");
        ev_send(n, now);
    }
    uint32_t wait = EV_WAIT_FOREVER;
    if (s_stream) {
        int64_t due = MIN(s_sent_us + EV_PING_US, s_refresh_us + EV_REFRESH_US) - now;
        wait = due <= 0 ? 0 : (uint32_t)((due + 999) / 1000);
    }
    xSemaphoreGive(s_ev_lock);
    return wait;
}

static void events_task(void *arg)
{
    uint32_t wait = EV_WAIT_FOREVER;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait == EV_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(wait) + 1);
        wait = events_step(esp_timer_get_time());
    }
}

void events_start(void)
{
    s_ev_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_ev_lock ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(events_task, "events", EV_TASK_STACK, NULL, EV_TASK_PRIO, &s_events_task) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}

/* GET /api/v1/events (owner): authorised first, then the request is taken asynchronous, an open stream is
 * ended, and the whole object goes out as the first event before this returns. */
esp_err_t events_get(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_OWNER)) {
        return ESP_OK;
    }
    httpd_req_t *a = NULL;
    if (httpd_req_async_handler_begin(req, &a) != ESP_OK) {
        return c6_json_error(req, 500, "internal", "the stream could not be opened (out of memory); try again");
    }
    httpd_resp_set_type(a, "text/event-stream");
    httpd_resp_set_hdr(a, "Cache-Control", "no-cache");
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    bool replaced = s_stream != NULL;
    ev_finish(true);
    uint32_t seq;
    portENTER_CRITICAL(&s_ev_mux);
    s_dirty = 0;                                  /* the whole object follows: every change so far is in it */
    seq = s_seq;
    portEXIT_CRITICAL(&s_ev_mux);
    s_stream = a;
    s_ev_id = seq;
    s_refresh_us = now;
    bool open = ev_send(ev_frame("state", seq, ST_ALL), now);
    xSemaphoreGive(s_ev_lock);
    if (open) {
        ESP_LOGI(TAG, "EVENTS stream opened at id %lu%s", (unsigned long)seq, replaced ? ", replacing the open one" : "");
        xTaskNotifyGive(s_events_task);           /* the task sets its timers from this stream */
    }
    return ESP_OK;
}
