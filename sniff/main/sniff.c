/*
 * sniff — a board as a receive-only 802.15.4 sniffer, for reading the Zigbee controller's traffic.
 *
 * Promiscuous mode on CONFIG_C6_SNIFF_CHANNEL. It never transmits: esp_ieee802154_set_promiscuous
 * turns both auto-ACK settings off (esp_ieee802154.c), and nothing here calls transmit. No Wi-Fi,
 * so the radio is never shared.
 *
 * The receive callback runs in the ISR: it copies the frame into a queue and hands the buffer
 * back; a task prints one line per frame on the USB console, read by tools/sniff_capture.py:
 *   F <sfd_timestamp_us> <rssi> <lqi> <hex: PSDU, FCS included>
 *   S <frames> <dropped>        every 10 s; dropped = queue full in the ISR
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

typedef struct {
    uint64_t ts;
    int8_t rssi;
    uint8_t lqi;
    uint8_t len;
    uint8_t psdu[127];
} rx_frame_t;

static QueueHandle_t s_rx_q;
static volatile uint32_t s_frames, s_dropped;

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    rx_frame_t f = {.ts = info->timestamp, .rssi = info->rssi, .lqi = info->lqi};
    f.len = frame[0] > sizeof(f.psdu) ? sizeof(f.psdu) : frame[0];
    memcpy(f.psdu, frame + 1, f.len);
    esp_ieee802154_receive_handle_done(frame);
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_rx_q, &f, &woken) == pdTRUE) {
        s_frames++;
    } else {
        s_dropped++;
    }
    portYIELD_FROM_ISR(woken);
}

static void print_task(void *arg)
{
    static char line[32 + 2 * 127 + 2];
    rx_frame_t f;
    TickType_t next_stat = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    for (;;) {
        if (xQueueReceive(s_rx_q, &f, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int n = snprintf(line, sizeof(line), "F %" PRIu64 " %d %u ", f.ts, f.rssi, f.lqi);
            for (int i = 0; i < f.len; i++) {
                n += snprintf(line + n, sizeof(line) - n, "%02x", f.psdu[i]);
            }
            puts(line);
        }
        if (xTaskGetTickCount() >= next_stat) {
            printf("S %" PRIu32 " %" PRIu32 "\n", s_frames, s_dropped);
            next_stat += pdMS_TO_TICKS(10000);
        }
    }
}

void app_main(void)
{
    // PHY calibration data lives in NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_rx_q = xQueueCreate(64, sizeof(rx_frame_t));
    xTaskCreate(print_task, "sniff_print", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(esp_ieee802154_enable());
    ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_ieee802154_set_channel(CONFIG_C6_SNIFF_CHANNEL));
    ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
    ESP_ERROR_CHECK(esp_ieee802154_receive());
    printf("I sniff %s channel %d promiscuous %d\n", esp_app_get_description()->version,
           esp_ieee802154_get_channel(), esp_ieee802154_get_promiscuous());
}
